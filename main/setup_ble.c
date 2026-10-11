#include "sdkconfig.h"

#if CONFIG_BR_SETUP_BLE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "setup.h"
#include "setup_ble.h"

static const char *TAG = "setup_ble";

#define RX_MAX          256
#define TX_MAX          1200                    // \n → \r\n 변환 후 최대
#define LINE_IDLE_US    (300 * 1000)            // 줄바꿈 없이 보내는 앱 대비: 마지막 수신 0.3초 후 한 줄로 처리
#define NOTIFY_TRIES    25

// Nordic UART Service (NUS) — 대부분의 BLE 터미널 앱이 지원하는 고정 UUID (little-endian)
#define NUS_UUID(b) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                                     0x93, 0xf3, 0xa3, 0xb5, (b), 0x00, 0x40, 0x6e)
static const ble_uuid128_t SVC_UUID = NUS_UUID(0x01);
static const ble_uuid128_t RX_UUID  = NUS_UUID(0x02);   // 휴대폰 → ESP (write)
static const ble_uuid128_t TX_UUID  = NUS_UUID(0x03);   // ESP → 휴대폰 (notify)

static char              s_name[32];
static uint8_t           s_addr_type;
static uint16_t          s_tx_handle;
static bool              s_running;
static volatile bool     s_stopping;            // 닫는 중 — 재광고 금지
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t s_session;
static volatile bool     s_subscribed;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static char              s_rx[RX_MAX];          // 수신 누적 (host 태스크 → setup 태스크)
static size_t            s_rx_len;
static int64_t           s_rx_last;

static int gap_event(struct ble_gap_event *ev, void *arg);

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t  buf[RX_MAX];
    uint16_t n = 0;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n);    // 넘치는 부분은 버림
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    if (n > RX_MAX - s_rx_len) {
        n = (uint16_t)(RX_MAX - s_rx_len);
    }
    memcpy(s_rx + s_rx_len, buf, n);
    s_rx_len += n;
    s_rx_last = now;
    taskEXIT_CRITICAL(&s_mux);
    setup_wake();
    return 0;
}

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &RX_UUID.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &TX_UUID.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_tx_handle },
            { 0 },
        },
    },
    { 0 },
};

static void advertise(void)
{
    // 광고: Flags + 이름 / 스캔 응답: NUS UUID (31바이트 한도 때문에 분리)
    struct ble_hs_adv_fields adv = {
        .flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name             = (uint8_t *)s_name,
        .name_len         = (uint8_t)strlen(s_name),
        .name_is_complete = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .uuids128             = &SVC_UUID,
        .num_uuids128         = 1,
        .uuids128_is_complete = 1,
    };
    // 광고 간격 100~150ms — 짧을수록 Wi-Fi 와 무선부를 더 자주 나눠 씀
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max  = BLE_GAP_ADV_ITVL_MS(150),
    };
    int rc = ble_gap_adv_set_fields(&adv);
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "advertise failed rc=%d", rc);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_session++;
            taskENTER_CRITICAL(&s_mux);
            s_rx_len = 0;
            taskEXIT_CRITICAL(&s_mux);
            ESP_LOGI(TAG, "phone connected");
            // 연결 간격 45~75ms 요청 (iOS 규칙 충족) — 타이핑에는 충분하고 Wi-Fi 몫의 무선 시간을 덜 뺏음
            struct ble_gap_upd_params up = {
                .itvl_min            = BLE_GAP_CONN_ITVL_MS(45),
                .itvl_max            = BLE_GAP_CONN_ITVL_MS(75),
                .latency             = 0,
                .supervision_timeout = 400,    // 4초 (10ms 단위)
            };
            ble_gap_update_params(s_conn, &up);
        } else if (!s_stopping) {
            advertise();
        }
        setup_wake();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason=0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        if (!s_stopping) {
            advertise();
        }
        setup_wake();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_stopping && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle && ev->subscribe.cur_notify) {
            s_subscribed = true;
            setup_wake();
        }
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    advertise();
    ESP_LOGW(TAG, "advertising as \"%s\"", s_name);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();      // nimble_port_stop() 시 반환
    vTaskDelete(NULL);      // freertos_deinit 대신 자기 삭제 — 곧바로 다시 열 때 새 host 태스크를 지우지 않게
}

esp_err_t sble_open(const char *name)
{
    if (s_running) {
        return ESP_OK;
    }
    strlcpy(s_name, name, sizeof(s_name));

    esp_err_t err = nimble_port_init();         // BT 컨트롤러 + NimBLE host
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(s_name);
    if (rc == 0) {
        rc = ble_gatts_count_cfg(SVCS);
    }
    if (rc == 0) {
        rc = ble_gatts_add_svcs(SVCS);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt setup failed rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_stopping   = false;
    s_conn       = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_rx_len     = 0;
    nimble_port_freertos_init(host_task);       // host 시작 → on_sync 에서 광고
    s_running = true;
    return ESP_OK;
}

void sble_close(void)
{
    if (!s_running) {
        return;
    }
    s_stopping = true;
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 50 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();                   // BT 컨트롤러까지 해제 → Wi-Fi 가 무선부 단독 사용
    } else {
        ESP_LOGE(TAG, "nimble stop failed");
    }
    s_conn    = BLE_HS_CONN_HANDLE_NONE;
    s_running = false;
    ESP_LOGI(TAG, "BLE off");
}

bool sble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

uint32_t sble_session(void)
{
    return s_session;
}

bool sble_take_subscribed(void)
{
    bool v = s_subscribed;
    s_subscribed = false;
    return v;
}

bool sble_take_line(char *out, size_t cap)
{
    int64_t now = esp_timer_get_time();
    bool got = false;
    size_t i = 0, take = 0, skip = 0;

    taskENTER_CRITICAL(&s_mux);
    while (i < s_rx_len && s_rx[i] != '\n' && s_rx[i] != '\r') {
        i++;
    }
    if (i < s_rx_len) {                                         // 줄바꿈까지
        take = i;
        skip = i + 1;
        if (s_rx[i] == '\r' && skip < s_rx_len && s_rx[skip] == '\n') {
            skip++;
        }
        got = true;
    } else if (s_rx_len > 0 && now - s_rx_last >= LINE_IDLE_US) {  // 줄바꿈 없이 끝난 입력
        take = skip = s_rx_len;
        got = true;
    }
    if (got) {
        size_t n = take < cap - 1 ? take : cap - 1;
        memcpy(out, s_rx, n);
        out[n] = '\0';
        memmove(s_rx, s_rx + skip, s_rx_len - skip);
        s_rx_len -= skip;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

void sble_send(const char *s, size_t len)
{
    static char tx[TX_MAX];     // setup 태스크 전용
    size_t n = 0;

    for (size_t i = 0; i < len && n + 2 < sizeof(tx); i++) {   // 터미널 앱 호환: 줄바꿈은 CR+LF
        if (s[i] == '\n') {
            tx[n++] = '\r';
        }
        tx[n++] = s[i];
    }

    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint16_t mtu   = ble_att_mtu(conn);
    size_t   chunk = mtu > 3 ? (size_t)mtu - 3 : 20;
    for (size_t off = 0; off < n;) {
        size_t m = n - off < chunk ? n - off : chunk;
        int rc = BLE_HS_ENOMEM;
        for (int t = 0; t < NOTIFY_TRIES && rc != 0; t++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(tx + off, m);
            // 실패 시 om 은 스택이 해제 (BLE_HS_ENOTSUP 만 예외)
            rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
                vTaskDelay(pdMS_TO_TICKS(20));      // 송신 버퍼 대기
            } else if (rc != 0) {
                if (rc == BLE_HS_ENOTSUP) {
                    os_mbuf_free_chain(om);
                }
                ESP_LOGW(TAG, "notify failed rc=%d", rc);
                return;
            }
        }
        if (rc != 0) {
            return;
        }
        off += m;
    }
}

void sble_disconnect(void)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(200));             // 직전 알림(안내 문구)이 나갈 여유
    taskENTER_CRITICAL(&s_mux);
    s_rx_len = 0;                               // 끊긴 연결의 남은 입력 폐기
    taskEXIT_CRITICAL(&s_mux);
    int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "terminate failed rc=%d", rc);
    }
}

#endif // CONFIG_BR_SETUP_BLE
