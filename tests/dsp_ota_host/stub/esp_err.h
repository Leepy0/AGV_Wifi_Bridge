#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
