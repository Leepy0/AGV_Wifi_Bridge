// AGV 메인보드(TMS320F2808) 상주 부트로더 — 플래시 섹터 A
//  리셋 → 부트플래그 또는 앱 무효 → 업데이트 모드(SCI-A 115200, bootproto) / 아니면 앱(0x3E8040)으로 점프
#include "boot_cfg.h"
#include "f2808_regs.h"
#include "dsp_image.h"
#include "bootproto.h"
#include "boot_hw.h"

// 부트로더 정보 블록 (0x3F7F00) — 앱이 읽어 버전 보고
#pragma DATA_SECTION(g_bootinfo, ".bootinfo")
const uint16_t g_bootinfo[64] = {
    DSPIMG_BOOT_MAGIC0, DSPIMG_BOOT_MAGIC1, BP_PROTO_VER, 0,
    BOOT_VER_CHARS, 0,
};

static const uint16_t BOOT_VER[BP_BOOTVER_LEN] = { BOOT_VER_CHARS, 0 };

static void flash_read(uint32_t addr, uint16_t *w, uint16_t n)
{
    const uint16_t *src = (const uint16_t *)addr;
    while (n--) {
        *w++ = *src++;
    }
}

static const bp_ops_t OPS = { hw_flash_erase, hw_flash_program, flash_read, BOOT_VER };
static bp_ctx_t s_bp;

// 앱이 기록한 진입 요청 (워치독 리셋을 거쳐도 SARAM 은 유지됨)
static int take_bootflag(void)
{
    volatile uint16_t *f = (volatile uint16_t *)DSPIMG_BOOTFLAG_ADDR;
    int set = f[0] == DSPIMG_BOOTFLAG_0 && f[1] == DSPIMG_BOOTFLAG_1 &&
              f[2] == DSPIMG_BOOTFLAG_2 && f[3] == DSPIMG_BOOTFLAG_3;
    f[0] = f[1] = f[2] = f[3] = 0;
    return set;
}

static void update_mode(void)
{
    uint16_t wraps = 0;
    const uint16_t idle_wraps = (uint16_t)((uint32_t)IDLE_TIMEOUT_S * (SYSCLK_HZ / 1000UL) / 4294967UL) + 1;   // 랩 ≈ 2^32 / SYSCLK 초

    hw_sci_init();
    hw_timer_start();
    bp_init(&s_bp, &OPS);
    for (;;) {
        int b = hw_sci_getc();
        if (b >= 0) {
            wraps = 0;
            hw_timer_start();
            if (bp_rx_byte(&s_bp, (uint16_t)b)) {
                uint16_t i;
                for (i = 0; i < s_bp.tx_len; i++) {
                    hw_sci_putc(s_bp.tx[i]);
                }
                if (s_bp.reset_pending) {
                    hw_sci_flush();
                    hw_reset();
                }
            }
            continue;
        }
        if (hw_timer_wrapped() && ++wraps >= idle_wraps) {
            // 브리지가 사라짐 — 앱이 유효하면 복귀, 아니면 계속 대기
            wraps = 0;
            if (bp_app_check(&OPS, 0, 0) == DSPIMG_OK) {
                hw_reset();
            }
        }
    }
}

void main(void)
{
    DINT();
    hw_init_clocks();
    hw_copy_ramfuncs();
    hw_init_flash();

    if (take_bootflag()) {
        update_mode();
    }
    if (bp_app_check(&OPS, 0, 0) == DSPIMG_OK) {
        hw_jump_app();
    }
    update_mode();
}
