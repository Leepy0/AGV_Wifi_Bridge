#pragma once
// 부트로더 설정 — 메인보드 앱과 같은 클럭 값을 쓴다 (표준: 20MHz 크리스탈, PLL ×10/2 = 100MHz)

#define BOOT_VER_CHARS      '1', '.', '0', '.', '0'     // 부트로더 버전 (정보 블록·HELLO 응답)

#define OSC_HZ              20000000UL
#define PLLCR_DIV           10          // PLLCR.DIV: SYSCLK = OSC × DIV / 2 (DIVSEL = 2)
#define PLL_DIVSEL          2           // PLLSTS.DIVSEL: 2 = /2
#define SYSCLK_HZ           (OSC_HZ * PLLCR_DIV / 2)      // 100,000,000
#define LOSPCP_VAL          2           // LSPCLK = SYSCLK / (2 × 2) = 25MHz (리셋 기본값과 동일)
#define LSPCLK_HZ           (SYSCLK_HZ / (2UL * LOSPCP_VAL))

#define SCI_BAUD            115200UL
#define SCI_BRR             ((LSPCLK_HZ + 4UL * SCI_BAUD) / (8UL * SCI_BAUD) - 1UL)   // 반올림: 26 → 115740bps

// 100MHz 기준 플래시 대기 상태 (F2808 데이터시트: SYSCLK ≤ 100MHz → RANDWAIT 3, PAGEWAIT 3, OTPWAIT 3)
#define FLASH_RANDWAIT      3
#define FLASH_PAGEWAIT      3
#define FLASH_OTPWAIT       3

// 업데이트 모드에서 수신 없이 이 시간이 지나고 앱이 유효하면 앱으로 재부팅 (브리지가 죽은 경우 대비)
#define IDLE_TIMEOUT_S      600
