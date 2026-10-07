#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// 무선 설정용 SoftAP + 웹 페이지 (휴대폰으로 접속 → http://192.168.4.1)
//  - 열림: 저장된 설정 없음(처음 설치) / 설정 버튼 3초 / 콘솔·서버 'setup on'
//  - 닫힘: 'setup off' / 웹에서 저장(재부팅) / 접속 단말 없이 Wi-Fi(STA) 연결 상태가 timeout 동안 지속
//          (STA 미연결이면 유일한 접근 경로이므로 열어 둠)
//  - STA 중계는 그대로 유지 (APSTA). AP 채널은 STA 채널을 따라감
void      setup_ap_prepare(void);       // net_bridge 의 Wi-Fi 초기화 중 esp_wifi_start 전에 호출 (AP 설정 등록)
esp_err_t setup_ap_init(void);          // net_bridge_start 성공 후 호출 (관리 태스크·버튼)
void      setup_ap_request(bool open);
bool      setup_ap_active(void);
size_t    setup_ap_format(char *buf, size_t cap);   // 상태 한 줄
