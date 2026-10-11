#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
#include <stdint.h>
int64_t esp_timer_get_time(void);
