#pragma once
// TMS320F2808 레지스터 — 부트로더가 쓰는 것만 (DSP280x 헤더 없이 빌드되게). 주소·비트는 SPRU712(F280x System Control)·
// SPRU051(SCI)·SPRS230(데이터시트) 기준. DSP280x_SysCtrl.h / DSP280x_Sci.h / DSP280x_Gpio.h 와 대조할 것.
#include <stdint.h>

#define REG16(a)            (*(volatile uint16_t *)(a))
#define REG32(a)            (*(volatile uint32_t *)(a))

#define EALLOW()            asm(" EALLOW")
#define EDIS()              asm(" EDIS")
#define DINT()              asm(" SETC INTM")
#define NOP()               asm(" NOP")

// ── 시스템 제어 (EALLOW) ──
#define PLLSTS              REG16(0x7011)
#define PLLSTS_PLLLOCKS     0x0001
#define PLLSTS_MCLKSTS      0x0008
#define PLLSTS_MCLKCLR      0x0010
#define PLLSTS_MCLKOFF      0x0040
#define PLLSTS_DIVSEL_MASK  0x0180
#define PLLSTS_DIVSEL_SHIFT 7
#define LOSPCP              REG16(0x701B)
#define PCLKCR0             REG16(0x701C)
#define PCLKCR0_SCIAENCLK   0x0400
#define PLLCR               REG16(0x7021)
#define PLLCR_DIV_MASK      0x000F
#define SCSR                REG16(0x7022)
#define WDKEY               REG16(0x7025)
#define WDCR                REG16(0x7029)
#define WDCR_DISABLE        0x0068      // WDDIS | WDCHK=101
#define WDCR_RESET_NOW      0x0000      // WDCHK ≠ 101 → 즉시 리셋

// ── 플래시 제어 (EALLOW) ──
#define FOPT                REG16(0x0A80)
#define FOPT_ENPIPE         0x0001
#define FPWR                REG16(0x0A82)
#define FPWR_ACTIVE         0x0003
#define FSTATUS             REG16(0x0A83)
#define FSTATUS_V3STAT      0x0008      // 1 쓰면 클리어
#define FSTDBYWAIT          REG16(0x0A84)
#define FACTIVEWAIT         REG16(0x0A85)
#define FBANKWAIT           REG16(0x0A86)   // PAGEWAIT 11:8, RANDWAIT 3:0
#define FOTPWAIT            REG16(0x0A87)   // OTPWAIT 4:0

// ── GPIO (EALLOW) — 32비트 레지스터의 상위 워드(GPIO24~31) ──
#define GPAQSEL2_H          REG16(0x6F85)   // GPIO28: bits 9:8 (3 = 비동기 입력)
#define GPAMUX2_H           REG16(0x6F89)   // GPIO28: bits 9:8 = 1 (SCIRXDA), GPIO29: bits 11:10 = 1 (SCITXDA)
#define GPAPUD_H            REG16(0x6F8D)   // bit12 GPIO28, bit13 GPIO29 (0 = 풀업 켬)

// ── SCI-A ──
#define SCICCR              REG16(0x7050)
#define SCICTL1             REG16(0x7051)
#define SCIHBAUD            REG16(0x7052)
#define SCILBAUD            REG16(0x7053)
#define SCICTL2             REG16(0x7054)
#define SCICTL2_TXEMPTY     0x0040
#define SCIRXST             REG16(0x7055)
#define SCIRXBUF            REG16(0x7057)
#define SCITXBUF            REG16(0x7059)
#define SCIFFTX             REG16(0x705A)
#define SCIFFTX_TXFFST_MASK 0x1F00      // 12:8
#define SCIFFRX             REG16(0x705B)
#define SCIFFRX_RXFFST_MASK 0x1F00      // 12:8
#define SCIFFRX_RXFFOVF     0x8000
#define SCIFFRX_RXFFOVRCLR  0x4000
#define SCIFFCT             REG16(0x705C)

// ── CPU 타이머 0 ──
#define TIMER0TIM           REG32(0x0C00)
#define TIMER0PRD           REG32(0x0C02)
#define TIMER0TCR           REG16(0x0C04)
#define TIMER0TCR_TIF       0x8000      // 1 쓰면 클리어
#define TIMER0TCR_TRB       0x0020
#define TIMER0TCR_TSS       0x0010
#define TIMER0TPR           REG16(0x0C06)
#define TIMER0TPRH          REG16(0x0C07)
