#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

// 무선 설정 모드 — 휴대폰으로 호기 설정 (USB 콘솔과 같은 명령)
//  - BLE (기본): 휴대폰 BLE 터미널 앱으로 장치 이름 AGVBR-<호기>-<MAC 끝 4자리> 에 접속
//  - SoftAP + 웹 페이지 (선택, 기본 꺼짐): 사내 무선 보안(WIPS)이 있는 현장에서는 차단됨
//  열림: 저장된 설정 없음(처음 설치) / 설정 버튼 3초 / 콘솔·서버 'setup on' / BLE 에서 reboot 한 뒤
//  닫힘: 'setup off' / 저장 후 재부팅 / 접속 단말 없이 Wi-Fi(STA) 연결 상태가 timeout 동안 지속
//        (STA 미연결이면 유일한 접근 경로이므로 열어 둠)
//  BLE 는 열려 있는 동안만 BT 컨트롤러를 켬 — Wi-Fi 와 무선부를 나눠 쓰므로 평상시에는 완전히 끔
void      setup_prepare_wifi(void);     // net_bridge 의 Wi-Fi 초기화 중 esp_wifi_start 전에 호출
esp_err_t setup_init(void);             // net_bridge_start 성공 후 호출 (관리 태스크·버튼)
void      setup_request(bool open);
bool      setup_active(void);
size_t    setup_format(char *buf, size_t cap);   // 상태 한 줄
// 응답 송신 여유를 두고 재부팅 (reopen = 재부팅 후 설정 모드 다시 열기). 메인보드 프로그래밍 중이면 끝난 뒤 재부팅
void      setup_reboot(bool reopen);
void      setup_wake(void);             // 전송 계층 → 관리 태스크 깨움 (BLE 수신 등)
