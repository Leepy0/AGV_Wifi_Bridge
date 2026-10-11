#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
#include <stdio.h>
extern int g_verbose;
#define ESP_LOGI(tag, fmt, ...) do { (void)(tag); if (g_verbose) printf("  [log] " fmt "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { (void)(tag); if (g_verbose) printf("  [log] " fmt "\n", ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) do { (void)(tag); if (g_verbose) printf("  [log] " fmt "\n", ##__VA_ARGS__); } while (0)
