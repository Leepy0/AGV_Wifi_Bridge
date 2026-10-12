#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_http_client *esp_http_client_handle_t;
typedef struct { const char *url; int timeout_ms; int buffer_size; } esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c);
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h);
int esp_http_client_get_status_code(esp_http_client_handle_t h);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t h);
esp_err_t esp_http_client_close(esp_http_client_handle_t h);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h);
int esp_http_client_read(esp_http_client_handle_t h, char *buf, int len);
esp_err_t esp_http_client_get_url(esp_http_client_handle_t client, char *url, const int len);
