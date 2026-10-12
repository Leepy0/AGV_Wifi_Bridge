#pragma once
// 부트로더 설정 — 메인보드 앱과 같은 클럭 값을 쓴다 (표준: 20MHz 크리스탈, PLL ×10/2 = 100MHz)

#define BOOT_VER_CHARS      '1', '.', '1', '.', '0'     // 부트로더 버전 (정보 블록·HELLO 응답). 1.1.0 = 0.5.2 소스(HELLO 창·60초 시한·수신 오류 복구), 1.0.0 = 0.5.1 이전

#define OSC_HZ              20000000UL
#define PLLCR_DIV           10          // PLLCR.DIV: SYSCLK = OSC × DIV / 2 (DIVSEL = 2)
#define PLL_DIVSEL          2           // PLLSTS.DIVSEL: 2 = /2
#define SYSCLK_HZ           (OSC_HZ * PLLCR_DIV / 2)      // 100,000,000
#define LOSPCP_VAL          2           // LSPCLK = SYSCLK / (2 × 2) = 25MHz (리셋 기본값과 동일)
#define LSPCLK_HZ           (SYSCLK_HZ / (2UL * LOSPCP_VAL))

#define SCI_BAUD            115200UL
#define SCI_BRR             ((LSPCLK_HZ + 4UL * SCI_BAUD) / (8UL * SCI_BAUD) - 1UL)   // 반올림: 26 → 115740bps

// 100MHz 기준 플래시 대기 상태 (F280x 데이터시트 최소 대기 상태 표: 100MHz → RANDWAIT 3, PAGEWAIT 3, OTPWAIT 5.
// OTP 접근 60ns ÷ 10ns − 1 = 5, TI InitFlash 와 같음 — 앱이 InitFlash 를 다시 부르지 않아도 OTP 를 바르게 읽게)
#define FLASH_RANDWAIT      3
#define FLASH_PAGEWAIT      3
#define FLASH_OTPWAIT       5

// 업데이트 모드에서 유효한 프로토콜 패킷 없이 이 시간이 지나고 앱이 유효하면 앱으로 재부팅
// (브리지가 죽었거나 앱용 ASCII 프레임만 보내는 경우 대비 — 패킷이 아닌 바이트는 이 시계를 되돌리지 않는다)
#define IDLE_TIMEOUT_S      600
// 부트플래그(앱의 F…U000 승인)로 들어왔는데 첫 유효 패킷이 오지 않을 때의 시한. 앱이 유효하면 앱으로 복귀한다
// (UOK 직후 브리지 재부팅·HELLO 3회 모두 깨짐 → 정지 호기가 10분 동안 서 있지 않게). 첫 패킷 뒤에는 IDLE_TIMEOUT_S
#define FIRST_PKT_TIMEOUT_S 60
// 패킷 수신 도중 바이트가 이 시간 넘게 끊기면 파서를 처음부터 (잘린 패킷이 다음 HELLO 를 삼키지 않도록)
#define RX_GAP_MS           100

// 앱 점프 전 HELLO 수신 창 — 멈추거나 J4 통신이 안 되는 앱도 OTA 로 살리기 위한 구조 경로.
// 브리지가 HELLO 를 50ms 간격으로 계속 보내는 동안 메인보드 전원을 다시 넣으면, 이 창에서 CRC 가 맞는 HELLO 를 받아
// 업데이트 모드로 들어간다. 창은 고정 길이라 노이즈가 와도 늘어나지 않는다. 대가: 매 부팅이 약 0.2초 늦어진다.
#define HELLO_WINDOW_MS     200
// 창 안에서 쓰는 짧은 재동기 간격 — 노이즈로 시작된 가짜 패킷이 50ms 간격의 다음 HELLO 를 삼키지 않게
#define HELLO_GAP_MS        10
