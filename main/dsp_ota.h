#pragma once
// 메인보드(F2808) 펌웨어 OTA — 서버가 보낸 .dspfw 를 dspfw 파티션에 스테이징하고,
// 적용 명령을 받으면 메인보드를 부트로더로 보내 UART 로 직접 프로그래밍 (docs/DSP_OTA_DESIGN.md)
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t dsp_ota_init(void);                          // uart_bridge·net_bridge 시작 후
// 서버 패킷 처리 (JSON 텍스트)
// DSP_OTA_START {url, ver, crc32, size, job, apply, recover}
//  url: 199자 이하(넘으면 url_too_long), 호스트 = 설정된 서버 IP(아니면 url_host, 리다이렉트도 같은 규칙)
void      dsp_ota_start(const char *json, size_t len);
// DSP_OTA_APPLY {job, recover}
//  recover:1 = 복구 적용 — 앱이 U 에 응답하지 않아도 부트로더로 직행, HELLO 가 안 되면 60초 동안 전원 재투입 대기(wait_power).
//  recover 가 없으면 U 무응답은 app_no_reply (앱 버전을 모를 때 '?' 만 예외), U 송신 실패는 uart_busy.
//  최종 보고(staged·done·fail)는 같은 내용을 3회 보낸다
void      dsp_ota_apply(const char *json, size_t len);
void      dsp_ota_query_version(void);                  // 앱에 버전 조회 (부팅 후·업데이트 후)
// 메인보드 수신 프레임 관찰: PAN ID 학습, R 응답 소비. true = 소비함(서버로 올리지 않음)
bool      dsp_ota_observe(const uint8_t *frame, size_t len);
bool      dsp_ota_busy(void);                           // 프로그래밍 중 (ESP OTA 거부용)
bool      dsp_ota_staging(void);                        // 스테이징(다운로드) 중 (ESP OTA 거부용)
// 상태 JSON 조각: "dsp":"..","dspb":"..","dsp_build":"..","dsp_st":"..","staged":{..}  (앞뒤 중괄호·쉼표 없음)
size_t    dsp_ota_status_json(char *buf, size_t cap);
size_t    dsp_ota_format(char *buf, size_t cap);        // 콘솔 stat 용 한 줄
