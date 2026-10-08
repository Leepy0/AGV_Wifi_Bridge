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
#include "net_bridge.h"
#include "dsp_ota.h"
#include "ota.h"

static const char *TAG = "ota";

#define CONFIRM_TIMEOUT_US  (180LL * 1000 * 1000)   // 새 펌웨어가 이 시간 안에 서버 통신을 못 하면 롤백
#define URL_MAX             256
#define OTA_TASK_STACK      8192
#define OTA_TASK_PRIO       2                       // 브리지(UART/net) 태스크보다 낮게 — 중계가 우선

static volatile bool        s_busy;
static bool                 s_pending;              // 롤백 대기(미확정) 상태
static esp_timer_handle_t   s_confirm_timer;

// 진행 상황을 서버로 보고 (JSON)
static void report(const char *fmt, ...)
{
    char js[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(js, sizeof(js), fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < sizeof(js)) {
        net_send_ota_result((const uint8_t *)js, (size_t)n);
    }
}

static void confirm_timeout(void *arg)
{
    ESP_LOGE(TAG, "no server contact within %llds - rolling back", CONFIRM_TIMEOUT_US / 1000000);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_init(void)
{
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
    return s_busy;
}

const char *ota_state_str(void)
{
    if (s_busy)    return "busy";
    if (s_pending) return "pending";
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

    ESP_LOGI(TAG, "start %s", url);
    report("{\"stage\":\"start\",\"url\":\"%s\"}", url);

    esp_err_t err = esp_https_ota_begin(&oc, &h);
    if (err != ESP_OK) {
        report("{\"stage\":\"fail\",\"err\":\"begin %s\"}", esp_err_to_name(err));
        goto out;
    }

    // 같은 프로젝트의 이미지인지 확인 (다른 보드용 bin 방지)
    esp_app_desc_t nd = {0};
    if (esp_https_ota_get_img_desc(h, &nd) == ESP_OK) {
        if (strcmp(nd.project_name, esp_app_get_description()->project_name) != 0) {
            esp_https_ota_abort(h);
            report("{\"stage\":\"fail\",\"err\":\"project mismatch (%s)\"}", nd.project_name);
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
        report("{\"stage\":\"fail\",\"err\":\"perform %s\"}", esp_err_to_name(err));
        goto out;
    }
    err = esp_https_ota_finish(h);      // 이미지 검증 + 부팅 슬롯 전환
    if (err != ESP_OK) {
        report("{\"stage\":\"fail\",\"err\":\"finish %s\"}", esp_err_to_name(err));
        goto out;
    }

    report("{\"stage\":\"done\",\"ver\":\"%s\",\"bytes\":%d}", nd.version, esp_https_ota_get_image_len_read(h));
    ESP_LOGI(TAG, "done, rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));     // 결과 패킷 송신 여유
    esp_restart();

out:
    ESP_LOGE(TAG, "failed: %s", esp_err_to_name(err));
    free(url);
    s_busy = false;
    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url)
{
    size_t n = url ? strlen(url) : 0;
    if (n < 8 || n >= URL_MAX || strncmp(url, "http", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_busy || dsp_ota_busy()) {
        report("{\"stage\":\"busy\"}");
        return ESP_ERR_INVALID_STATE;
    }
    char *copy = strdup(url);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    s_busy = true;
    if (xTaskCreate(ota_task, "ota", OTA_TASK_STACK, copy, OTA_TASK_PRIO, NULL) != pdPASS) {
        free(copy);
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
