#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "app_config.h"
#include "net_bridge.h"
#include "dsp_ota.h"
#include "bridge_util.h"
#include "ota.h"

static const char *TAG = "ota";

#define CONFIRM_TIMEOUT_US  (180LL * 1000 * 1000)   // 새 펌웨어가 이 시간 안에 서버 통신을 못 하면 롤백
#define URL_MAX             256
#define OTA_TASK_STACK      8192
#define OTA_TASK_PRIO       2                       // 브리지(UART/net) 태스크보다 낮게 — 중계가 우선
#define REPORT_MAX          400                     // OTA_RESULT JSON (BR_PAYLOAD_MAX 512 이하)
#define FINAL_REPEAT        3                       // 최종 결과(done/fail/busy)는 같은 내용으로 3회 — UDP 유실 대비
#define FINAL_GAP_US        (300 * 1000)

static bool                 s_busy;                 // __atomic 으로 접근 (net_rx·콘솔·ota 태스크)
static bool                 s_pending;              // 롤백 대기(미확정) 상태
static esp_timer_handle_t   s_confirm_timer;
// 최종 결과 재송신 (esp_timer 로 300ms 간격, 호출 태스크를 막지 않음)
static esp_timer_handle_t   s_final_timer;
static portMUX_TYPE         s_final_mux = portMUX_INITIALIZER_UNLOCKED;
static char                 s_final[REPORT_MAX];
static size_t               s_final_len;
static int                  s_final_left;

// 진행 상황을 서버로 보고 (JSON)
static void report(const char *fmt, ...)
{
    char js[REPORT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(js, sizeof(js), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(js)) {
        net_send_ota_result((const uint8_t *)js, (size_t)n);
    }
}

static void final_resend(void *arg)
{
    static char js[REPORT_MAX];         // esp_timer 태스크 전용 (스택 절약)
    size_t n = 0;
    bool more;
    taskENTER_CRITICAL(&s_final_mux);
    if (s_final_left > 0) {
        s_final_left--;
        n = s_final_len;
        memcpy(js, s_final, n);
    }
    more = s_final_left > 0;
    taskEXIT_CRITICAL(&s_final_mux);
    if (n > 0) {
        net_send_ota_result((const uint8_t *)js, n);
    }
    if (more) {
        esp_timer_start_once(s_final_timer, FINAL_GAP_US);
    }
}

// 최종 결과 보고: 지금 1회 + 300ms 간격으로 2회 더 (서버는 중복에 안전)
static void report_final(const char *fmt, ...)
{
    char js[REPORT_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(js, sizeof(js), fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(js)) {
        return;
    }
    net_send_ota_result((const uint8_t *)js, (size_t)n);
    if (!s_final_timer) {
        return;
    }
    esp_timer_stop(s_final_timer);          // 앞선 결과의 재송신이 남아 있으면 새 결과로 대체
    taskENTER_CRITICAL(&s_final_mux);
    memcpy(s_final, js, (size_t)n);
    s_final_len  = (size_t)n;
    s_final_left = FINAL_REPEAT - 1;
    taskEXIT_CRITICAL(&s_final_mux);
    esp_timer_start_once(s_final_timer, FINAL_GAP_US);
}

// JSON 문자열에 넣을 사본: 따옴표·역슬래시·제어 문자는 '_' 로
static void json_safe(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        char c = src[i];
        dst[i] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '_' : c;
    }
    dst[i] = '\0';
}

static void confirm_timeout(void *arg)
{
    ESP_LOGE(TAG, "no server contact within %llds - rolling back", CONFIRM_TIMEOUT_US / 1000000);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_init(void)
{
    const esp_timer_create_args_t fa = { .callback = final_resend, .name = "ota_final" };
    if (esp_timer_create(&fa, &s_final_timer) != ESP_OK) {
        s_final_timer = NULL;               // 재송신 없이 1회만 보냄
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        const esp_timer_create_args_t ta = { .callback = confirm_timeout, .name = "ota_confirm" };
        if (esp_timer_create(&ta, &s_confirm_timer) == ESP_OK) {
            esp_timer_start_once(s_confirm_timer, CONFIRM_TIMEOUT_US);
        }
        ESP_LOGW(TAG, "new firmware %s on %s - pending verify (confirm on first server packet)",
                 esp_app_get_description()->version, run->label);
    }
}

void ota_confirm(void)
{
    if (!s_pending) {
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending = false;
        if (s_confirm_timer) {
            esp_timer_stop(s_confirm_timer);
        }
        ESP_LOGI(TAG, "firmware confirmed");
    }
}

bool ota_busy(void)
{
    return __atomic_load_n(&s_busy, __ATOMIC_SEQ_CST);
}

const char *ota_state_str(void)
{
    if (ota_busy()) return "busy";
    if (s_pending)  return "pending";
    return "valid";
}

const char *ota_running_partition(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    return run ? run->label : "?";
}

static void ota_task(void *arg)
{
    char *url = arg;
    esp_https_ota_handle_t h = NULL;
    esp_http_client_config_t hc = {
        .url               = url,
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
        .buffer_size       = 4096,
    };
    esp_https_ota_config_t oc = { .http_config = &hc };

    char safe[URL_MAX];
    json_safe(safe, sizeof(safe), url);
    ESP_LOGI(TAG, "start %s", url);
    report("{\"stage\":\"start\",\"url\":\"%s\"}", safe);

    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) {
        report_final("{\"stage\":\"fail\",\"err\":\"begin %s\"}", esp_err_to_name(err));
        goto out;
    }

    // 같은 프로젝트의 이미지인지 확인 (다른 보드용 bin 방지)
    esp_app_desc_t nd = {0};
    if (esp_https_ota_get_img_desc(h, &nd) == ESP_OK) {
        if (strcmp(nd.project_name, esp_app_get_description()->project_name) != 0) {
            esp_https_ota_abort(h);
            char pn[sizeof(nd.project_name)];
            json_safe(pn, sizeof(pn), nd.project_name);
            report_final("{\"stage\":\"fail\",\"err\":\"project mismatch (%s)\"}", pn);
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        ESP_LOGI(TAG, "image %s (%s)", nd.version, nd.project_name);
    }

    int total = esp_https_ota_get_image_size(h);
    int last  = -1;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(h);
        int pct  = total > 0 ? read * 100 / total : 0;
        if (pct / 10 != last / 10) {    // 10% 단위 보고
            last = pct;
            report("{\"stage\":\"progress\",\"pct\":%d,\"bytes\":%d}", pct, read);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        report_final("{\"stage\":\"fail\",\"err\":\"perform %s\"}", esp_err_to_name(err));
        goto out;
    }
    err = esp_https_ota_finish(h);      // 이미지 검증 + 부팅 슬롯 전환
    if (err != ESP_OK) {
        report_final("{\"stage\":\"fail\",\"err\":\"finish %s\"}", esp_err_to_name(err));
        goto out;
    }

    char ver[sizeof(nd.version)];
    json_safe(ver, sizeof(ver), nd.version);
    report_final("{\"stage\":\"done\",\"ver\":\"%s\",\"bytes\":%d}", ver, esp_https_ota_get_image_len_read(h));
    ESP_LOGI(TAG, "done, rebooting");
    // 결과 3회 송신(300ms 간격) 여유 뒤 재부팅
    vTaskDelay(pdMS_TO_TICKS((FINAL_REPEAT - 1) * FINAL_GAP_US / 1000 + 200));
    esp_restart();

out:
    ESP_LOGE(TAG, "failed: %s", esp_err_to_name(err));
    free(url);
    __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url)
{
    size_t n = url ? strlen(url) : 0;
    if (n < 8 || n >= URL_MAX || strncmp(url, "http", 4) != 0) {
        report_final("{\"stage\":\"fail\",\"err\":\"bad_url\"}");
        return ESP_ERR_INVALID_ARG;
    }
    // 결정 11: 서명 검증 대신 출처 제한 — URL 호스트가 설정된 서버 IP 일 때만 받음 (출발지 위조 패킷으로 임의 서버 지정 방지)
    if (!bu_url_host_is(url, g_cfg.server_ip)) {
        ESP_LOGW(TAG, "rejected: url host is not the configured server - %s", url);
        report_final("{\"stage\":\"fail\",\"err\":\"url_host\"}");
        return ESP_ERR_INVALID_ARG;
    }
    bool idle = false;
    if (!__atomic_compare_exchange_n(&s_busy, &idle, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        report_final("{\"stage\":\"busy\"}");
        return ESP_ERR_INVALID_STATE;
    }
    // s_busy 를 먼저 세운 뒤 메인보드 상태를 본다 — 메인보드 적용 쪽도 상태를 먼저 세운 뒤 ota_busy() 를 보면
    // 동시에 시작해도 둘 중 하나는 반드시 상대를 보고 물러난다.
    // 스테이징(다운로드) 중에도 거부: OTA 완료 후 재부팅이 스테이징을 끊어 staged 보고 없이 사라짐
    const char *why = dsp_ota_busy() ? "mainboard_programming" : dsp_ota_staging() ? "mainboard_staging" : NULL;
    if (why) {
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        ESP_LOGW(TAG, "rejected: %s", why);
        report_final("{\"stage\":\"fail\",\"err\":\"%s\"}", why);
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(url);
    if (!copy) {
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        report_final("{\"stage\":\"fail\",\"err\":\"no_mem\"}");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, copy, OTA_TASK_PRIO, NULL) != pdPASS) {
        free(copy);
        __atomic_store_n(&s_busy, false, __ATOMIC_SEQ_CST);
        report_final("{\"stage\":\"fail\",\"err\":\"no_mem\"}");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
