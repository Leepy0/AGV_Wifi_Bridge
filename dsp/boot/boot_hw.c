// F2808 하드웨어 계층. 플래시 API 호출부는 RAM(ramfuncs)에서 실행 — 지우기·쓰기 중에는 플래시를 읽을 수 없다.
#include <string.h>
#include "boot_cfg.h"
#include "f2808_regs.h"
#include "bootproto.h"
#include "boot_hw.h"
#include "Flash2808_API_Library.h"      // SPRC193 Flash API (SCALE_FACTOR 는 Flash2808_API_Config.h 의 CPU_RATE 로 결정)

// 링커가 만드는 복사 범위 심볼 (F2808_boot.cmd)
extern uint16_t RamfuncsLoadStart, RamfuncsLoadEnd, RamfuncsRunStart;
extern uint16_t Flash28_API_LoadStart, Flash28_API_LoadEnd, Flash28_API_RunStart;

static void delay_loop(uint32_t n)
{
    while (n--) {
        NOP();
    }
}

void hw_init_clocks(void)
{
    EALLOW();
    WDCR = WDCR_DISABLE;
    // PLL (DSP280x_SysCtrl.c InitPll 과 같은 순서)
    if (PLLSTS & PLLSTS_MCLKSTS) {
        PLLSTS |= PLLSTS_MCLKCLR;       // 클럭 감지 오류 플래그 정리 후 계속 (크리스탈 확인 필요)
    }
    if (PLLSTS & PLLSTS_DIVSEL_MASK) {
        PLLSTS &= (uint16_t)~PLLSTS_DIVSEL_MASK;    // DIVSEL = 0 (/4) 로 내린 뒤 PLL 변경
    }
    if ((PLLCR & PLLCR_DIV_MASK) != PLLCR_DIV) {
        PLLSTS |= PLLSTS_MCLKOFF;       // PLL 변경 중 missing-clock 감지 끔
        PLLCR = PLLCR_DIV;
        while ((PLLSTS & PLLSTS_PLLLOCKS) == 0) {
        }
        PLLSTS &= (uint16_t)~PLLSTS_MCLKOFF;
    }
    PLLSTS = (uint16_t)((PLLSTS & ~PLLSTS_DIVSEL_MASK) | (PLL_DIVSEL << PLLSTS_DIVSEL_SHIFT));
    LOSPCP = LOSPCP_VAL;
    PCLKCR0 |= PCLKCR0_SCIAENCLK;
    EDIS();
}

void hw_copy_ramfuncs(void)
{
    memcpy(&RamfuncsRunStart, &RamfuncsLoadStart, (size_t)(&RamfuncsLoadEnd - &RamfuncsLoadStart));
    memcpy(&Flash28_API_RunStart, &Flash28_API_LoadStart, (size_t)(&Flash28_API_LoadEnd - &Flash28_API_LoadStart));
}

#pragma CODE_SECTION(hw_init_flash, "ramfuncs")
void hw_init_flash(void)
{
    EALLOW();
    FPWR        = FPWR_ACTIVE;
    FSTATUS     = FSTATUS_V3STAT;
    FSTDBYWAIT  = 0x01FF;
    FACTIVEWAIT = 0x01FF;
    FBANKWAIT   = (uint16_t)((FLASH_PAGEWAIT << 8) | FLASH_RANDWAIT);
    FOTPWAIT    = FLASH_OTPWAIT;
    FOPT        = FOPT_ENPIPE;
    EDIS();
    asm(" RPT #7 || NOP");              // 파이프라인 정리
}

void hw_sci_init(void)
{
    EALLOW();
    GPAPUD_H   &= (uint16_t)~0x3000;    // GPIO28/29 풀업
    GPAQSEL2_H |= 0x0300;               // GPIO28 비동기 입력
    GPAMUX2_H   = (uint16_t)((GPAMUX2_H & ~0x0F00) | 0x0500);   // GPIO28 = SCIRXDA, GPIO29 = SCITXDA
    EDIS();

    SCICCR   = 0x0007;                  // 8비트, 1 스톱, 패리티 없음
    SCICTL1  = 0x0003;                  // RX/TX 켬, 리셋 상태
    SCICTL2  = 0x0000;
    SCIHBAUD = (uint16_t)(SCI_BRR >> 8);
    SCILBAUD = (uint16_t)(SCI_BRR & 0xFF);
    SCIFFTX  = 0xE040;                  // FIFO 켬, TX FIFO 리셋 해제
    SCIFFRX  = 0x2040 | SCIFFRX_RXFFOVRCLR;   // RX FIFO 리셋 해제, 오버플로 클리어
    SCIFFCT  = 0x0000;
    SCICTL1  = 0x0023;                  // SWRESET 해제
}

int hw_sci_getc(void)
{
    if (SCIFFRX & SCIFFRX_RXFFOVF) {
        SCIFFRX |= SCIFFRX_RXFFOVRCLR;  // 넘침: 이후 패킷은 CRC 로 걸러짐
    }
    if ((SCIFFRX & SCIFFRX_RXFFST_MASK) == 0) {
        return -1;
    }
    return (int)(SCIRXBUF & 0x00FF);
}

void hw_sci_putc(uint16_t b)
{
    while ((SCIFFTX & SCIFFTX_TXFFST_MASK) >= (16 << 8)) {
    }
    SCITXBUF = b & 0x00FF;
}

void hw_sci_flush(void)
{
    while ((SCIFFTX & SCIFFTX_TXFFST_MASK) != 0) {
    }
    while ((SCICTL2 & SCICTL2_TXEMPTY) == 0) {
    }
    delay_loop(2000);                   // 마지막 스톱 비트 여유
}

void hw_timer_start(void)
{
    TIMER0TCR  = TIMER0TCR_TSS;         // 정지
    TIMER0TPR  = 0;
    TIMER0TPRH = 0;
    TIMER0PRD  = 0xFFFFFFFFUL;
    TIMER0TCR  = TIMER0TCR_TRB | TIMER0TCR_TIF;   // 리로드, 플래그 클리어
    TIMER0TCR &= (uint16_t)~TIMER0TCR_TSS;        // 시작
}

uint32_t hw_timer_now(void)
{
    return TIMER0TIM;
}

int hw_timer_wrapped(void)
{
    if (TIMER0TCR & TIMER0TCR_TIF) {
        TIMER0TCR |= TIMER0TCR_TIF;
        return 1;
    }
    return 0;
}

void hw_reset(void)
{
    DINT();
    EALLOW();
    SCSR = 0x0000;                      // 워치독 → 리셋 (인터럽트 아님)
    WDCR = WDCR_RESET_NOW;              // WDCHK 오류 → 즉시 리셋
    EDIS();
    for (;;) {
    }
}

void hw_jump_app(void)
{
    void (*app)(void) = (void (*)(void))DSPIMG_APP_CODE_ADDR;   // 0x3E8040: 앱 codestart (LB _c_int00)
    DINT();
    app();
    for (;;) {
    }
}

// ── 플래시 API 래퍼 (RAM) ──

static FLASH_ST s_st;

#pragma CODE_SECTION(flash_prepare, "ramfuncs")
static void flash_prepare(void)
{
    Flash_CPUScaleFactor = SCALE_FACTOR;    // Flash2808_API_Config.h: CPU_RATE = 10.000L (100MHz)
    Flash_CallbackPtr    = NULL;            // 워치독은 꺼져 있음
}

#pragma CODE_SECTION(hw_flash_erase, "ramfuncs")
uint16_t hw_flash_erase(uint16_t bp_mask)
{
    uint16_t mask = 0;
    if (bp_mask & BP_ERASE_B) mask |= SECTORB;
    if (bp_mask & BP_ERASE_C) mask |= SECTORC;
    if (bp_mask & BP_ERASE_D) mask |= SECTORD;
    if (mask == 0 || (bp_mask & (uint16_t)~BP_ERASE_APP)) {
        return BP_ST_BAD_PAYLOAD;       // 섹터 A 는 절대 지우지 않음
    }
    flash_prepare();
    return Flash_Erase(mask, &s_st) == STATUS_SUCCESS ? BP_ST_OK : BP_ST_ERASE_FAIL;
}

#pragma CODE_SECTION(hw_flash_program, "ramfuncs")
uint16_t hw_flash_program(uint32_t addr, const uint16_t *w, uint16_t n)
{
    Uint16 *dst = (Uint16 *)addr;
    if (addr < DSPIMG_APP_HDR_ADDR || addr + n > DSPIMG_APP_END) {
        return BP_ST_BAD_ADDR;
    }
    flash_prepare();
    if (Flash_Program(dst, (Uint16 *)w, (Uint32)n, &s_st) != STATUS_SUCCESS) {
        return BP_ST_PROG_FAIL;
    }
    if (Flash_Verify(dst, (Uint16 *)w, (Uint32)n, &s_st) != STATUS_SUCCESS) {
        return BP_ST_VERIFY_FAIL;
    }
    return BP_ST_OK;
}
