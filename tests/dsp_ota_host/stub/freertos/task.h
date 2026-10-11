#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
void vTaskDelay(TickType_t t);
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t f, const char *n, uint32_t st, void *a, int prio, void *h, int core);
