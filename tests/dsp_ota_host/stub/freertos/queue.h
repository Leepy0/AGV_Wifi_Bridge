#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(int n, int sz);
BaseType_t xQueueSend(QueueHandle_t q, const void *p, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *p, TickType_t t);
