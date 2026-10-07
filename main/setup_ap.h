#pragma once
#include <stdbool.h>
#include "esp_err.h"

// 설정 모드의 SoftAP + 웹 페이지 전송 계층 (CONFIG_BR_SETUP_SOFTAP). 관리는 setup.c
void      sap_prepare(const char *ssid);    // esp_wifi_start 전: AP 설정 미리 등록
esp_err_t sap_open(void);                   // APSTA 전환 + http://192.168.4.1
void      sap_close(void);
int       sap_clients(void);                // 접속 단말 수
