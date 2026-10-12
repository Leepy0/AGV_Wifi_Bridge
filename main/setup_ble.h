#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 설정 모드의 BLE 전송 계층 (CONFIG_BR_SETUP_BLE). 관리는 setup.c
//  Nordic UART Service(NUS) — 휴대폰 BLE 터미널 앱(Serial Bluetooth Terminal, nRF Toolbox, Bluefruit Connect 등)
//  열 때 NimBLE·BT 컨트롤러를 초기화하고 닫을 때 해제 (평상시 Wi-Fi 에 영향 없음)
esp_err_t sble_open(const char *name);
void      sble_close(void);
bool      sble_connected(void);
uint32_t  sble_session(void);               // 연결마다 증가 — 로그인 상태 초기화용
bool      sble_take_subscribed(void);       // 앱이 알림을 구독함 (안내 문구 송신 시점)
bool      sble_take_line(char *out, size_t cap);   // 받은 명령 한 줄 (없으면 false)
void      sble_send(const char *s, size_t len);    // 결과 송신 (\n → \r\n, MTU 단위 분할)
void      sble_disconnect(void);            // 지금 연결을 끊음 (광고는 계속 — 로그인 연속 실패 등)
