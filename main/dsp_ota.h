#pragma once
// 메인보드(F2808) 펌웨어 OTA — 서버가 보낸 .dspfw 를 dspfw 파티션에 스테이징하고,
// 적용 명령을 받으면 메인보드를 부트로더로 보내 UART 로 직접 프로그래밍 (docs/DSP_OTA_DESIGN.md)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t dsp_ota_init(void);                          // uart_bridge·net_bridge 시작 후
// 서버 패킷 처리 (JSON 텍스트)
void      dsp_ota_start(const char *json, size_t len);  // DSP_OTA_START {url, ver, crc32, size, job, apply}
void      dsp_ota_apply(const char *json, size_t len);  // DSP_OTA_APPLY {job}
void      dsp_ota_query_version(void);                  // 앱에 버전 조회 (부팅 후·업데이트 후)
// 메인보드 수신 프레임 관찰: PAN ID 학습, R 응답 소비. true = 소비함(서버로 올리지 않음)
bool      dsp_ota_observe(const uint8_t *frame, size_t len);
bool      dsp_ota_busy(void);                           // 프로그래밍 중 (ESP OTA 거부용)
// 상태 JSON 조각: "dsp":"..","dspb":"..","dsp_build":"..","dsp_st":"..","staged":{..}  (앞뒤 중괄호·쉼표 없음)
size_t    dsp_ota_status_json(char *buf, size_t cap);
size_t    dsp_ota_format(char *buf, size_t cap);        // 콘솔 stat 용 한 줄
