#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "app_config.h"
#include "net_bridge.h"
#include "cmd.h"
#include "setup.h"
#include "setup_ap.h"
#include "setup_ble.h"
#include "dsp_ota.h"
#include "bridge_util.h"

static const char *TAG = "setup";

#define TASK_PRIO       2           // 중계(UART/net) 태스크보다 낮게
#define TASK_STACK      6144
#define TICK_MS         100
#define BTN_HOLD_MS     3000
#define OPEN_RETRY_US   (1000LL * 1000)
#define OPEN_TRIES      5
#define IDLE_TIMEOUT_US ((int64_t)CONFIG_BR_SETUP_TIMEOUT_S * 1000 * 1000)
#define REOPEN_MAGIC    0x5E7A0B1Eu
#define CMD_LINE_MAX    200
#define BAD_PASS_DELAY_MS 1000
#define REBOOT_DELAY_MS 1000        // 응답 송신 여유
#define BUSY_POLL_MS    500

#if CONFIG_BR_SETUP_BLE
#define BLE_PASS CONFIG_BR_SETUP_BLE_PASS
#endif

static char             s_name[20];         // AGVBR-<호기>-<MAC 끝 4자리> (BLE 이름 / AP SSID)
static TaskHandle_t     s_task;
static volatile bool    s_want;             // 열기 요청 (false = 닫기)
static volatile bool    s_on;               // 설정 모드 동작 중
static volatile bool    s_reboot;
static volatile bool    s_reboot_reopen;
static int64_t          s_idle_since;       // 접속 단말이 없어진 시각
static RTC_NOINIT_ATTR uint32_t s_reopen;   // 소프트웨어 재부팅 후 설정 모드 다시 열기

void setup_prepare_wifi(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_name, sizeof(s_name), "AGVBR-%02u-%02X%02X", g_cfg.agv_id, mac[4], mac[5]);
#if CONFIG_BR_SETUP_SOFTAP
    sap_prepare(s_name);
#endif
}

void setup_wake(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void setup_request(bool open)
{
    if (open && s_on) {
        s_idle_since = esp_timer_get_time();    // 이미 열려 있으면 유휴 시간만 연장
    }
    s_want = open;
    setup_wake();
}

bool setup_active(void)
{
    return s_on;
}

// 응답 송신 여유를 두고 재부팅. 메인보드 프로그래밍 중이면 끝날 때까지 미룸
// (지운 뒤 재부팅하면 메인보드가 앱 없이 부트로더에 남는다)
static void do_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    if (dsp_ota_busy()) {
        ESP_LOGW(TAG, "reboot deferred until mainboard programming ends");
        while (dsp_ota_busy()) {
            vTaskDelay(pdMS_TO_TICKS(BUSY_POLL_MS));
        }
    }
    s_reopen = s_reboot_reopen ? REOPEN_MAGIC : 0;
    esp_restart();
}

static void reboot_task(void *arg)
{
    do_reboot();
}

void setup_reboot(bool reopen)
{
    s_reboot_reopen = reopen;
    if (s_task == NULL) {
        // 설정 모드 없음(무선 설정 방식 꺼짐 등) — 재부팅 전용 태스크로
        if (xTaskCreate(reboot_task, "reboot", 3072, NULL, TASK_PRIO, NULL) != pdPASS) {
            do_reboot();                // 메모리 부족 — 호출 태스크에서 바로
        }
        return;
    }
    s_reboot = true;
    setup_wake();
}

static int clients(void)
{
    int n = 0;
#if CONFIG_BR_SETUP_BLE
    n += sble_connected() ? 1 : 0;
#endif
#if CONFIG_BR_SETUP_SOFTAP
    n += sap_clients();
#endif
    return n;
}

size_t setup_format(char *buf, size_t cap)
{
    int n;
    if (cap == 0) {
        return 0;
    }
    if (s_task == NULL) {
        n = snprintf(buf, cap, "setup: unavailable\n");
    } else if (!s_on) {
        n = snprintf(buf, cap, "setup: %s name=%s\n", s_want ? "opening" : "off", s_name);
    } else {
        n = snprintf(buf, cap, "setup: on name=%s"
#if CONFIG_BR_SETUP_BLE
                     " ble=%s"
#endif
#if CONFIG_BR_SETUP_SOFTAP
                     " ap_clients=%d"
#endif
                     "%s\n", s_name
#if CONFIG_BR_SETUP_BLE
                     , sble_connected() ? "connected" : "advertising"
#endif
#if CONFIG_BR_SETUP_SOFTAP
                     , sap_clients()
#endif
                     , s_want ? "" : " (closing)");
    }
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

static bool open_all(void)
{
    bool ok = false;
#if CONFIG_BR_SETUP_BLE
    ok |= sble_open(s_name) == ESP_OK;
#endif
#if CONFIG_BR_SETUP_SOFTAP
    ok |= sap_open() == ESP_OK;
#endif
    if (ok) {
        s_on = true;
        s_idle_since = esp_timer_get_time();
        ESP_LOGW(TAG, "setup mode on - \"%s\"", s_name);
    }
    return ok;
}

static void close_all(void)
{
#if CONFIG_BR_SETUP_BLE
    sble_close();
#endif
#if CONFIG_BR_SETUP_SOFTAP
    sap_close();
#endif
    s_on = false;
    ESP_LOGI(TAG, "setup mode off");
}

#if CONFIG_BR_SETUP_BLE
// BLE 로 받은 명령 처리 — 콘솔과 같은 문법. 비밀번호가 설정돼 있으면 'login <비밀번호>' 후 사용
static void ble_poll(void)
{
    static char line[CMD_LINE_MAX];
    static char out[600];
    static uint32_t authed_session;     // 로그인한 연결 (0 = 없음)
    static bu_login_guard_t guard;      // 연결별 로그인 연속 실패 계수
    cmd_act_t act;
    size_t n;

    if (sble_take_subscribed()) {
        n = snprintf(out, sizeof(out), "AGV Wi-Fi Bridge %s (fw %s)\n%s\n", s_name, esp_app_get_description()->version,
                     BLE_PASS[0] ? "login <password>" : "type help");
        sble_send(out, n);
    }
    while (sble_take_line(line, sizeof(line))) {
        if (line[0] == '\0') {
            continue;                   // CR/LF 가 따로 온 경우 등 빈 줄
        }
        if (strncmp(line, "login ", 6) == 0) {
            uint32_t session = sble_session();
            if (strcmp(line + 6, BLE_PASS) == 0) {
                authed_session = session;
                bu_login_ok(&guard, session);
                n = snprintf(out, sizeof(out), "ok - type help\n");
            } else {
                vTaskDelay(pdMS_TO_TICKS(BAD_PASS_DELAY_MS));     // 대입 시도 지연
                if (bu_login_fail(&guard, session)) {
                    // 같은 연결에서 연속 5회 실패 → 연결을 끊음 (다시 접속해야 재시도 가능)
                    ESP_LOGW(TAG, "BLE login failed %d times - disconnecting", BU_LOGIN_MAX_FAILS);
                    n = snprintf(out, sizeof(out), "wrong password - too many failures, disconnecting\n");
                    sble_send(out, n);
                    sble_disconnect();
                    break;              // 남은 입력은 버림 (다음 연결에서 s_rx 초기화)
                }
                n = snprintf(out, sizeof(out), "wrong password\n");
            }
            sble_send(out, n);
            continue;
        }
        if (BLE_PASS[0] && authed_session != sble_session()) {
            n = snprintf(out, sizeof(out), "login required: login <password>\n");
            sble_send(out, n);
            continue;
        }
        n = cmd_exec(line, CMD_SRC_LOCAL, out, sizeof(out), &act);
        sble_send(out, n);
        if (act == CMD_ACT_REBOOT) {
            setup_reboot(true);         // 재부팅 후 BLE 다시 열기 → 다시 접속해 'stat' 으로 Wi-Fi 연결 확인
        }
    }
}
#endif

// 설정 모드 열기/닫기·버튼·유휴 종료·명령 처리·재부팅을 한 태스크에서 처리 (Wi-Fi/BT 상태 변경을 한곳으로)
static void setup_task(void *arg)
{
    int64_t next_try = 0;
    int tries = 0;
#if CONFIG_BR_SETUP_BTN_GPIO >= 0
    int64_t btn_down = 0;       // 버튼이 눌린 시각 (0 = 안 눌림)
    bool btn_fired = false;
#endif

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (s_reboot) {
            do_reboot();
        }

#if CONFIG_BR_SETUP_BTN_GPIO >= 0
        // 설정 버튼 3초 → 열기 (태스크가 BLE 수신 등으로 일찍 깨기도 하므로 눌린 시각 기준)
        if (gpio_get_level(CONFIG_BR_SETUP_BTN_GPIO) == 0) {
            if (btn_down == 0) {
                btn_down = now;
            } else if (!btn_fired && now - btn_down >= BTN_HOLD_MS * 1000LL) {
                btn_fired = true;
                ESP_LOGI(TAG, "setup button held");
                setup_request(true);
            }
        } else {
            btn_down  = 0;
            btn_fired = false;
        }
#endif

        if (s_want && !s_on && now >= next_try) {
            if (open_all()) {
                tries = 0;
            } else if (++tries >= OPEN_TRIES) {
                ESP_LOGE(TAG, "setup mode failed to start");
                s_want = false;
                tries  = 0;
            } else {
                next_try = now + OPEN_RETRY_US;
            }
        } else if (!s_want && s_on) {
            close_all();
        }

        if (s_on) {
            // 단말 없이 STA 연결 상태가 timeout 지속 → 닫음. STA 미연결이면 유일한 접근 경로이므로 유지
            if (clients() > 0) {
                s_idle_since = now;
            } else if (net_is_up() && now - s_idle_since >= IDLE_TIMEOUT_US) {
                ESP_LOGI(TAG, "setup mode idle for %ds", CONFIG_BR_SETUP_TIMEOUT_S);
                s_want = false;
                close_all();
            }
        }
#if CONFIG_BR_SETUP_BLE
        if (s_on) {
            ble_poll();
        }
#endif
#if CONFIG_BR_SETUP_SOFTAP
        // 휴대폰이 설정 AP 를 쓰는 동안 STA 재접속(전 채널 스캔)을 멈춰 AP 채널을 유지
        net_sta_hold(s_on && sap_clients() > 0 && !net_is_up());
#endif
    }
}

esp_err_t setup_init(void)
{
#if !CONFIG_BR_SETUP_BLE && !CONFIG_BR_SETUP_SOFTAP
    return ESP_ERR_NOT_SUPPORTED;           // 무선 설정 방식이 하나도 없음 (menuconfig)
#endif
    if (s_name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;       // Wi-Fi 초기화 전
    }
#if CONFIG_BR_SETUP_BTN_GPIO >= 0
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_BR_SETUP_BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "button");
#endif
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(setup_task, "setup", TASK_STACK, NULL, TASK_PRIO, &s_task, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "setup task");

    // BLE 에서 reboot 한 직후 — 다시 접속해 결과를 확인할 수 있게 설정 모드 유지
    if (esp_reset_reason() == ESP_RST_SW && s_reopen == REOPEN_MAGIC) {
        setup_request(true);
    }
    s_reopen = 0;
    return ESP_OK;
}
