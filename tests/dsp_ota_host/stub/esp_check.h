#pragma once
// 호스트 시험용 최소 스텁 (ESP-IDF·FreeRTOS 선언만 — 구현은 test_dsp_ota_flow.c 의 가짜)
#define ESP_RETURN_ON_FALSE(a, err, tag, msg) do { if (!(a)) return err; } while (0)
