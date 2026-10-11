#pragma once
// 호스트 시험용 최소 ESP-IDF/FreeRTOS 선언 (tests/bridge_util 전용 — 동작은 시험 파일이 정의)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK                         0
#define ESP_FAIL                       -1
#define ESP_ERR_NO_MEM                 0x101
#define ESP_ERR_INVALID_ARG            0x102
#define ESP_ERR_INVALID_STATE          0x103
#define ESP_ERR_HTTPS_OTA_IN_PROGRESS  0x9002
const char *esp_err_to_name(esp_err_t err);

#define ESP_LOGE(tag, fmt, ...) ((void)(tag))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag))
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef void    *TaskHandle_t;
typedef void   (*TaskFunction_t)(void *);
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdMS_TO_TICKS(x) ((TickType_t)(x))
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m)  ((void)(m))
void       vTaskDelay(TickType_t t);
void       vTaskDelete(TaskHandle_t t);
BaseType_t xTaskCreate(TaskFunction_t f, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h);

typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);
typedef struct { esp_timer_cb_t callback; void *arg; const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
int64_t   esp_timer_get_time(void);

void esp_restart(void);

typedef struct { char version[32]; char project_name[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);

typedef struct { const char *label; } esp_partition_t;
typedef enum { ESP_OTA_IMG_VALID, ESP_OTA_IMG_PENDING_VERIFY } esp_ota_img_states_t;
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *st);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

typedef struct { const char *url; int timeout_ms; bool keep_alive_enable; int buffer_size; } esp_http_client_config_t;
typedef struct { const esp_http_client_config_t *http_config; } esp_https_ota_config_t;
typedef void *esp_https_ota_handle_t;
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c, esp_https_ota_handle_t *h);
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h, esp_app_desc_t *d);
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h);
int       esp_https_ota_get_image_size(esp_https_ota_handle_t h);
int       esp_https_ota_get_image_len_read(esp_https_ota_handle_t h);
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h);
bool      esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h);
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h);
