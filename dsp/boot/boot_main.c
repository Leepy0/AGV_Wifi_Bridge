// AGV 메인보드(TMS320F2808) 상주 부트로더 — 플래시 섹터 A
//  리셋 → 부트플래그 또는 앱 무효 → 업데이트 모드(SCI-A 115200, bootproto)
//       → 앱 유효: 기동 실패 3회 초과면 업데이트 모드, 아니면 약 200ms HELLO 수신 창 → HELLO 오면 업데이트 모드 / 아니면 앱(0x3E8040)으로 점프
#include "boot_cfg.h"
#include "f2808_regs.h"
#include "dsp_image.h"
#include "bootproto.h"
#include "boot_hw.h"

// 고정 주소 접근 (플래시 읽기·부트플래그·부트 카운터). 호스트 시뮬레이션 시험은 가짜 메모리로 바꿔 끼운다 (tests/host)
#ifndef BOOT_MEM
#define BOOT_MEM(a)     ((volatile uint16_t *)(a))
#endif

// 부트로더 정보 블록 (0x3F7F00) — 앱이 읽어 버전 보고
#pragma DATA_SECTION(g_bootinfo, ".bootinfo")
const uint16_t g_bootinfo[64] = {
    DSPIMG_BOOT_MAGIC0, DSPIMG_BOOT_MAGIC1, BP_PROTO_VER, 0,
    BOOT_VER_CHARS, 0,
};

static const uint16_t BOOT_VER[BP_BOOTVER_LEN] = { BOOT_VER_CHARS, 0 };

static void flash_read(uint32_t addr, uint16_t *w, uint16_t n)
{
    const volatile uint16_t *src = BOOT_MEM(addr);
    while (n--) {
        *w++ = *src++;
    }
}

static const bp_ops_t OPS = { hw_flash_erase, hw_flash_program, flash_read, BOOT_VER };
static bp_ctx_t s_bp;

// 앱이 기록한 진입 요청 (워치독 리셋을 거쳐도 SARAM 은 유지됨)
static int take_bootflag(void)
{
    volatile uint16_t *f = BOOT_MEM(DSPIMG_BOOTFLAG_ADDR);
    int set = f[0] == DSPIMG_BOOTFLAG_0 && f[1] == DSPIMG_BOOTFLAG_1 &&
              f[2] == DSPIMG_BOOTFLAG_2 && f[3] == DSPIMG_BOOTFLAG_3;
    f[0] = f[1] = f[2] = f[3] = 0;
    return set;
}

// 앱 기동 실패 횟수 — CRC 는 맞지만 fwup_boot_ok() 전에 리셋되는 앱(기동 직후 워치독 리셋·예외 리셋)도 OTA 로 다시 고칠 수 있게.
// 리셋 없이 멈추는 앱(워치독을 끈 채 무한 대기 등)이나 메인 루프까지 가지만 J4 SCI 가 안 되는 앱은 이 카운터로 못 잡는다
// → 앱 워치독 필수(app_fwupdate.h) + 아래 HELLO 수신 창(전원 재투입 시 브리지의 HELLO 로 진입)으로 복구한다.
// 전원을 다시 넣으면 RAM 이 지워져 0 부터 다시 센다.
static uint16_t bootcnt_bump(void)
{
    volatile uint16_t *c = BOOT_MEM(DSPIMG_BOOTCNT_ADDR);
    uint16_t n = c[0] == DSPIMG_BOOTCNT_MAGIC ? c[1] : 0;   // 전원 투입 직후 쓰레기 값은 0
    if (n < 0xFFFF) {
        n++;
    }
    c[0] = DSPIMG_BOOTCNT_MAGIC;
    c[1] = n;
    return n;
}

static void bootcnt_clear(void)
{
    volatile uint16_t *c = BOOT_MEM(DSPIMG_BOOTCNT_ADDR);
    c[0] = DSPIMG_BOOTCNT_MAGIC;
    c[1] = 0;
}

// 업데이트 모드 진입 사유
enum {
    ENTRY_BOOTFLAG,         // 앱이 F…U000 을 승인하고 부트플래그를 남김 (앱은 보통 유효)
    ENTRY_APP_INVALID,      // 앱 헤더·CRC 무효
    ENTRY_BOOTCNT,          // fwup_boot_ok() 없이 연속 리셋 (기동 불능 앱)
    ENTRY_HELLO             // 부팅 직후 HELLO 창에서 HELLO 를 받음 (SCI·파서는 이미 초기화, 응답도 보냄)
};

static void send_tx(void)
{
    uint16_t i;
    for (i = 0; i < s_bp.tx_len; i++) {
        hw_sci_putc(s_bp.tx[i]);
    }
}

// 앱 점프 전 HELLO 수신 창 (boot_cfg.h HELLO_WINDOW_MS). CRC 가 맞는 HELLO 를 받으면 응답을 보내고 1.
// 창은 고정 길이 — 노이즈·앱용 ASCII 프레임이 와도 늘어나지 않는다. HELLO 외의 명령은 무응답으로 버린다.
static int hello_window(void)
{
    const uint32_t window_us = (uint32_t)HELLO_WINDOW_MS * 1000UL;
    const uint32_t gap_us = (uint32_t)HELLO_GAP_MS * 1000UL;
    uint32_t last_rx = 0;

    hw_sci_init();
    bp_init(&s_bp, &OPS);
    s_bp.accept_cmd = BP_CMD_HELLO;
    hw_timer_start();
    while (hw_timer_us() < window_us) {
        int b = hw_sci_getc();
        if (b == HW_SCI_RXERR) {
            bp_rx_reset(&s_bp);
            continue;
        }
        if (b < 0) {
            if (bp_rx_busy(&s_bp) && hw_timer_us() - last_rx > gap_us) {
                bp_rx_reset(&s_bp);
            }
            continue;
        }
        last_rx = hw_timer_us();
        if (bp_rx_byte(&s_bp, (uint16_t)b)) {
            send_tx();
            s_bp.accept_cmd = 0;            // 이제부터 모든 명령 (재전송 판정용 last_* 는 그대로 이어 씀)
            return 1;
        }
    }
    hw_sci_deinit();
    hw_timer_deinit();
    return 0;
}

static void update_mode(uint16_t entry)
{
    // 부트플래그로 들어온 경우만 첫 유효 패킷 전까지 짧은 시한 (그 뒤나 다른 사유는 IDLE_TIMEOUT_S).
    // 시한이 지나도 앱이 무효면 리셋하지 않고 계속 기다린다 (부트플래그 + 앱 무효도 지금 규칙과 같음)
    uint32_t limit_us = (uint32_t)(entry == ENTRY_BOOTFLAG ? FIRST_PKT_TIMEOUT_S : IDLE_TIMEOUT_S) * 1000000UL;
    const uint32_t gap_us = (uint32_t)RX_GAP_MS * 1000UL;
    uint32_t last_rx = 0;

    if (entry != ENTRY_HELLO) {
        hw_sci_init();
        bp_init(&s_bp, &OPS);
    }
    hw_timer_start();
    for (;;) {
        int b = hw_sci_getc();
        if (b == HW_SCI_RXERR) {
            bp_rx_reset(&s_bp);     // 브레이크 등으로 SCI 를 리셋함 — 받던 패킷은 버리고 다음 SOF 부터
            continue;
        }
        if (b >= 0) {
            last_rx = hw_timer_us();
            if (bp_rx_byte(&s_bp, (uint16_t)b)) {
                // 유효한 패킷만 유휴 시계를 되돌린다 — 앱용 ASCII 프레임(V 조회·ACS·RFID)이나 노이즈는 아님
                limit_us = (uint32_t)IDLE_TIMEOUT_S * 1000000UL;
                send_tx();
                if (s_bp.reset_pending) {
                    hw_sci_flush();
                    hw_reset();
                }
                hw_timer_start();
                last_rx = 0;
            }
            continue;
        }
        if (bp_rx_busy(&s_bp) && hw_timer_us() - last_rx > gap_us) {
            bp_rx_reset(&s_bp);     // 잘린 패킷(브리지 재부팅·전원 흔들림) 뒤 재동기
        }
        if (hw_timer_us() >= limit_us) {
            // 브리지가 사라짐 — 앱이 유효하면 복귀, 아니면 계속 대기
            if (bp_app_check(&OPS, 0, 0) == DSPIMG_OK) {
                hw_sci_flush();
                hw_reset();
            }
            limit_us = (uint32_t)IDLE_TIMEOUT_S * 1000000UL;
            hw_timer_start();
            last_rx = 0;
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
        update_mode(ENTRY_BOOTFLAG);
    }
    if (bp_app_check(&OPS, 0, 0) == DSPIMG_OK) {
        if (bootcnt_bump() > DSPIMG_BOOTCNT_MAX) {
            // 앱이 fwup_boot_ok() 를 부르지 못한 채 연속 리셋 → 기동 불능 앱으로 보고 새 이미지를 기다린다
            bootcnt_clear();
            update_mode(ENTRY_BOOTCNT);
        }
        if (hello_window()) {
            // 브리지 구조 모드(전원 재투입 + HELLO 연속 송신): 앱으로 가지 않고 업데이트 모드.
            // 이번 부팅은 앱을 실행하지 않으므로 기동 실패로 세지 않는다
            bootcnt_clear();
            update_mode(ENTRY_HELLO);
        }
        hw_jump_app();
    }
    update_mode(ENTRY_APP_INVALID);
}
