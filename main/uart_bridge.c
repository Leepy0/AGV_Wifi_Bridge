#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "uart_bridge.h"
#include "bootproto.h"

static const char *TAG = "uart";

#define MAIN_UART      UART_NUM_1
#define RFID_UART      UART_NUM_2
#define RX_BUF_SIZE    1024
#define EVT_Q_LEN      64
#define MAIN_TXQ_LEN   16
#define TOUT_MAX_SYM   100      // S3: 1023비트 / 10비트(8N1) = 102 심볼
#define ETX_TOUT_SYM   2        // ETX 모드: 2문자 무수신이면 즉시 태스크로 전달 (115200 기준 0.17ms)
#define NO_ETX         (-1)

#if CONFIG_BR_MAIN_ETX_FRAMING
#define MAIN_ETX       CONFIG_BR_MAIN_ETX_CHAR
#else
#define MAIN_ETX       NO_ETX
#endif
#define RX_TASK_PRIO   12
#define TX_TASK_PRIO   13
#define BRIDGE_CORE    1        // Wi-Fi/lwIP 는 core 0
#define RAW_TX_DONE_MS 200      // 원시 모드 전환 전 송신 완료 대기
#define APP_FRAME_MIN  8        // 앱 프레임 최소 길이: STX 종류 PAN MSG AGV(2) ... ETX
#define STX_CHAR       0x02

#ifdef BP_BAUD
_Static_assert(BP_BAUD == UART_BRIDGE_BOOT_BAUD, "bootloader baud mismatch (bootproto.h vs uart_bridge.h)");
#endif

typedef struct {
    uart_port_t   uart;
    uint32_t      baud;
    uint32_t      gap_ms;
    int           etx;          // 프레임 종료 문자 (NO_ETX = 무수신 gap 방식)
    frame_cb_t    cb;
    QueueHandle_t evq;
    uart_stat_t   st;
} port_ctx_t;

typedef struct {
    uint16_t len;
    uint8_t  data[FRAME_MAX];
} tx_item_t;

static port_ctx_t    s_port[PORT_COUNT];
static QueueHandle_t s_main_txq;
static volatile bool s_raw;         // 메인보드 UART 원시 모드 (DSP 프로그래밍)
// 마지막 앱 프레임 수신 시각 (ms, esp_timer 기준). 32비트 한 번 쓰기라 다른 태스크에서 읽어도 찢기지 않음
static volatile uint32_t s_main_rx_ms;
static volatile bool     s_main_rx_seen;

// 프레임 종료 판단용 무수신 시간(ms) → UART RX TOUT 심볼 수
static uint8_t gap_to_symbols(uint32_t baud, uint32_t gap_ms)
{
    uint32_t sym = (gap_ms * baud + 9999) / 10000;   // gap_ms * baud / 1000 / 10비트, 올림
    if (sym < 1) {
        sym = 1;
    }
    if (sym > TOUT_MAX_SYM) {
        ESP_LOGW(TAG, "gap %lums @%lu too long, clamped to %d symbols",
                 (unsigned long)gap_ms, (unsigned long)baud, TOUT_MAX_SYM);
        sym = TOUT_MAX_SYM;
    }
    return (uint8_t)sym;
}

static void emit(port_ctx_t *p, const uint8_t *buf, size_t *len)
{
    if (*len == 0) {
        return;
    }
    p->st.rx_frames++;
    p->st.rx_bytes += *len;
    // 메인보드 앱 생존 판단용: STX 로 시작하는 앱 프레임만 (원시 모드 중 부트로더 응답·노이즈는 제외)
    if (p == &s_port[PORT_MAIN] && !s_raw && *len >= APP_FRAME_MIN && buf[0] == STX_CHAR) {
        s_main_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_main_rx_seen = true;
    }
    p->cb(buf, *len);
    *len = 0;
}

// 수신 바이트를 프레임에 추가. ETX 모드면 ETX 를 받는 즉시 전달, FRAME_MAX 도달 시 잘라서 전달
static void push_bytes(port_ctx_t *p, uint8_t *frame, size_t *len, const uint8_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        frame[(*len)++] = src[i];
        if (p->etx != NO_ETX && src[i] == (uint8_t)p->etx) {
            emit(p, frame, len);
        } else if (*len == FRAME_MAX) {
            p->st.overlong++;
            emit(p, frame, len);
        }
    }
}

// 링버퍼에서 count 바이트를 읽어 프레임 처리
static void read_into(port_ctx_t *p, uint8_t *frame, size_t *len, size_t count, TickType_t wait)
{
    uint8_t chunk[128];
    while (count > 0) {
        int n = uart_read_bytes(p->uart, chunk, count < sizeof(chunk) ? count : sizeof(chunk), wait);
        if (n <= 0) {
            break;
        }
        push_bytes(p, frame, len, chunk, (size_t)n);
        count -= n;
    }
}

// 수신 태스크
//  - gap 모드: 하드웨어 RX 타임아웃(무수신 gap)이 프레임 경계
//  - ETX 모드: ETX 문자가 프레임 경계, RX 타임아웃은 바이트를 빨리 넘겨받는 용도로만 사용
static void uart_rx_task(void *arg)
{
    port_ctx_t *p = arg;
    uint8_t frame[FRAME_MAX];
    size_t len = 0;
    uart_event_t ev;
    // TOUT 이벤트가 누락되는 경우(FIFO full 경계 등) 대비 소프트웨어 타임아웃
    // 긴 데이터 수신 중 FIFO full(120자) 이벤트 간격보다 길어야 프레임을 쪼개지 않음
    const uint32_t fifo_ms = 120u * 10000u / p->baud + 1;
    const TickType_t flush_to = pdMS_TO_TICKS(p->gap_ms * 2 + 2 + fifo_ms);

    for (;;) {
        if (xQueueReceive(p->evq, &ev, len ? flush_to : portMAX_DELAY) != pdTRUE) {
            if (s_raw && p == &s_port[PORT_MAIN]) {
                len = 0;
                continue;
            }
            if (p->etx != NO_ETX) {
                p->st.no_etx++;     // ETX 없이 끊긴 프레임 (노이즈·체크섬 충돌 등) — 그대로 전달
            }
            emit(p, frame, &len);
            continue;
        }
        if (s_raw && p == &s_port[PORT_MAIN]) {
            len = 0;                // 원시 모드: 데이터는 raw_read 가 직접 읽음, 이벤트만 소비
            continue;
        }
        switch (ev.type) {
        case UART_DATA:
            read_into(p, frame, &len, ev.size, pdMS_TO_TICKS(20));
            if (ev.timeout_flag) {
                // 재동기: 대기 이벤트가 없는데 링버퍼에 남은 바이트가 있으면 이벤트 유실로 밀린 것
                // → gap 이전 데이터이므로 이 프레임에 포함시켜 경계를 다시 맞춤
                if (uxQueueMessagesWaiting(p->evq) == 0) {
                    size_t extra = 0;
                    uart_get_buffered_data_len(p->uart, &extra);
                    if (extra > 0) {
                        p->st.resync++;
                        read_into(p, frame, &len, extra, 0);
                    }
                }
                if (p->etx == NO_ETX) {
                    emit(p, frame, &len);   // gap 모드: 무수신 = 프레임 끝
                }
            }
            break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            // 처리 지연으로 넘침 → 버퍼 비우고 재동기
            p->st.overflow++;
            uart_flush_input(p->uart);
            xQueueReset(p->evq);
            len = 0;
            break;
        case UART_FRAME_ERR:
        case UART_PARITY_ERR:
            p->st.rx_err++;
            break;
        default:
            break;
        }
    }
}

// 메인보드 송신 전용 태스크: 큐에서 꺼낸 프레임을 통째로 송신 (RFID/ACS 프레임 섞임 방지)
static void main_tx_task(void *arg)
{
    port_ctx_t *p = &s_port[PORT_MAIN];
    static tx_item_t item;
    // +1 tick: vTaskDelay(n) 은 최소 n-1 tick 만 보장
    const TickType_t gap = CONFIG_BR_MAIN_TX_GAP_MS ? pdMS_TO_TICKS(CONFIG_BR_MAIN_TX_GAP_MS) + 1 : 0;

    for (;;) {
        if (xQueueReceive(s_main_txq, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_raw) {
            // raw_begin() 의 큐 비우기와 경쟁해 남은 프레임: 부트로더 패킷 사이에 끼우지 않는다
            p->st.tx_drop++;
            continue;
        }
        uart_write_bytes(p->uart, item.data, item.len);
        p->st.tx_frames++;
        p->st.tx_bytes += item.len;
        if (gap > 0) {
            // 프레임 간 최소 무신호 구간 확보 (메인보드 측 프레임 구분용)
            uint32_t tx_ms = item.len * 10000u / p->baud + 10;   // 10비트/문자 송출 시간 + 여유
            uart_wait_tx_done(p->uart, pdMS_TO_TICKS(tx_ms));
            vTaskDelay(gap);
        }
    }
}

static esp_err_t port_init(port_id_t id, uart_port_t uart, int tx_gpio, int rx_gpio,
                           uint32_t baud, uint32_t gap_ms, int etx, frame_cb_t cb)
{
    port_ctx_t *p = &s_port[id];
    memset(p, 0, sizeof(*p));
    p->uart   = uart;
    p->baud   = baud;
    p->gap_ms = gap_ms;
    p->etx    = etx;
    p->cb     = cb;

    const uart_config_t uc = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // TX 버퍼 0: uart_write_bytes 가 FIFO 에 다 넣을 때까지 블로킹 (송신 태스크 전용이라 무방)
    ESP_RETURN_ON_ERROR(uart_driver_install(uart, RX_BUF_SIZE, 0, EVT_Q_LEN, &p->evq, 0), TAG, "install");
    ESP_RETURN_ON_ERROR(uart_param_config(uart, &uc), TAG, "param");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "pin");
    gpio_pullup_en(rx_gpio);    // 트랜시버 미연결 시 RX 노이즈 방지
    uint8_t tout = etx != NO_ETX ? ETX_TOUT_SYM : gap_to_symbols(baud, gap_ms);
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(uart, tout), TAG, "rx tout");

    if (etx != NO_ETX) {
        ESP_LOGI(TAG, "UART%d tx=%d rx=%d %lu bps, frame end = 0x%02X (fallback %lums)",
                 uart, tx_gpio, rx_gpio, (unsigned long)baud, etx, (unsigned long)gap_ms);
    } else {
        ESP_LOGI(TAG, "UART%d tx=%d rx=%d %lu bps, frame end = idle %lums",
                 uart, tx_gpio, rx_gpio, (unsigned long)baud, (unsigned long)gap_ms);
    }
    return ESP_OK;
}

esp_err_t uart_bridge_start(frame_cb_t on_main, frame_cb_t on_rfid)
{
    ESP_RETURN_ON_ERROR(port_init(PORT_MAIN, MAIN_UART, CONFIG_BR_MAIN_TX_GPIO, CONFIG_BR_MAIN_RX_GPIO,
                                  g_cfg.main_baud, CONFIG_BR_MAIN_GAP_MS, MAIN_ETX, on_main), TAG, "main port");
    ESP_RETURN_ON_ERROR(port_init(PORT_RFID, RFID_UART, CONFIG_BR_RFID_TX_GPIO, CONFIG_BR_RFID_RX_GPIO,
                                  g_cfg.rfid_baud, CONFIG_BR_RFID_GAP_MS, NO_ETX, on_rfid), TAG, "rfid port");

    s_main_txq = xQueueCreate(MAIN_TXQ_LEN, sizeof(tx_item_t));
    ESP_RETURN_ON_FALSE(s_main_txq, ESP_ERR_NO_MEM, TAG, "txq");

    if (xTaskCreatePinnedToCore(main_tx_task, "tx_main", 3072, NULL, TX_TASK_PRIO, NULL, BRIDGE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(uart_rx_task, "rx_main", 5120, &s_port[PORT_MAIN], RX_TASK_PRIO, NULL, BRIDGE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(uart_rx_task, "rx_rfid", 5120, &s_port[PORT_RFID], RX_TASK_PRIO, NULL, BRIDGE_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool uart_bridge_send_main(const uint8_t *data, size_t len)
{
    tx_item_t item;
    if (len == 0 || len > FRAME_MAX || s_raw) {
        s_port[PORT_MAIN].st.tx_drop++;
        return false;
    }
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    if (xQueueSend(s_main_txq, &item, 0) != pdTRUE) {
        s_port[PORT_MAIN].st.tx_drop++;
        return false;
    }
    return true;
}

const uart_stat_t *uart_bridge_stat(port_id_t port)
{
    return &s_port[port].st;
}

uint32_t uart_bridge_main_rx_age_ms(void)
{
    if (!s_main_rx_seen) {
        return UINT32_MAX;
    }
    return (uint32_t)(esp_timer_get_time() / 1000) - s_main_rx_ms;     // 부호 없는 뺄셈: 49일 랩어라운드에도 맞음
}

esp_err_t uart_bridge_raw_begin(void)
{
    if (s_raw) {
        return ESP_ERR_INVALID_STATE;
    }
    s_raw = true;
    xQueueReset(s_main_txq);                                // 대기 중 프레임 폐기
    uart_wait_tx_done(MAIN_UART, pdMS_TO_TICKS(RAW_TX_DONE_MS));   // 송신 중인 프레임 완료
    // 부트로더는 SCI 속도가 고정(115200) — 앱 링크 속도(main_baud)를 바꿔 운용하는 호기도 부트로더와 통신되게
    esp_err_t err = uart_set_baudrate(MAIN_UART, UART_BRIDGE_BOOT_BAUD);
    uart_flush_input(MAIN_UART);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set bootloader baud failed: %s", esp_err_to_name(err));
        uart_bridge_raw_end();
        return err;
    }
    ESP_LOGW(TAG, "main UART raw mode on (bridge paused, %lu bps)", (unsigned long)UART_BRIDGE_BOOT_BAUD);
    return ESP_OK;
}

void uart_bridge_raw_end(void)
{
    // 마지막 부트로더 패킷(RESET 등)이 다 나간 뒤 앱 속도로 되돌림 — TX 버퍼 0 이라 write 는 FIFO 적재까지만 기다림
    uart_wait_tx_done(MAIN_UART, pdMS_TO_TICKS(RAW_TX_DONE_MS));
    esp_err_t err = uart_set_baudrate(MAIN_UART, g_cfg.main_baud);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "restore baud %lu failed: %s", (unsigned long)g_cfg.main_baud, esp_err_to_name(err));
    }
    uart_flush_input(MAIN_UART);
    xQueueReset(s_port[PORT_MAIN].evq);
    s_raw = false;
    ESP_LOGI(TAG, "main UART raw mode off (%lu bps)", (unsigned long)g_cfg.main_baud);
}

int uart_bridge_raw_read(uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    int r = uart_read_bytes(MAIN_UART, buf, n, pdMS_TO_TICKS(timeout_ms));
    if (r > 0) {
        s_port[PORT_MAIN].st.rx_bytes += r;
    }
    return r;
}

void uart_bridge_raw_write(const uint8_t *buf, size_t n)
{
    uart_write_bytes(MAIN_UART, buf, n);
    s_port[PORT_MAIN].st.tx_bytes += n;
}

bool uart_bridge_raw_active(void)
{
    return s_raw;
}
