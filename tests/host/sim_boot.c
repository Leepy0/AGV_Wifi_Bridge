// 부트로더 진입 판정 시뮬레이션 — dsp/boot/boot_main.c 를 가짜 하드웨어(boot_hw.h 구현)와 함께 호스트에서 돌린다
//  D-01 부팅 직후 HELLO 창(고정 200ms, HELLO 만 받음), D-06 부트플래그 진입 시 첫 패킷 시한 60초,
//  D-03 수신 오류(HW_SCI_RXERR) 뒤 파서 재동기, 기존 규칙(앱 무효 무기한 대기·부트 카운터·600초 유휴)
#include <setjmp.h>
#include <string.h>
#include "boot_cfg.h"
#include "bootproto.h"
#include "boot_hw.h"
#include "test_util.h"

void boot_main(void);

// ── 가짜 메모리 ──
#define FL_BASE 0x3E8000UL
#define FL_END  0x3F8000UL
static uint16_t g_flash[FL_END - FL_BASE];
static uint16_t g_ram[0x800];

volatile uint16_t *host_boot_mem(uint32_t a)
{
    if (a >= FL_BASE && a < FL_END) return &g_flash[a - FL_BASE];
    if (a < 0x800) return &g_ram[a];
    fprintf(stderr, "bad address 0x%lX\n", (unsigned long)a);
    CHECK(0 && "알 수 없는 주소");
    return &g_ram[0];
}

// ── 가짜 하드웨어: 가상 시계(µs), 예정된 수신 바이트, 송신 기록 ──
enum { EXIT_NONE, EXIT_RESET, EXIT_APP, EXIT_LIMIT };
#define BYTE_US 87                  // 115200bps 한 바이트
#define POLL_US 20                  // 수신 없을 때 한 번 도는 시간

typedef struct { uint64_t t; int v; } rx_ev_t;
static rx_ev_t g_ev[4096];
static int g_ev_n, g_ev_i;
static uint64_t g_now, g_t0, g_limit;
static uint16_t g_tx[8192];
static int g_tx_n;
static int g_sci_init_n, g_sci_deinit_n, g_timer_deinit_n, g_erase_n;
static jmp_buf g_env;

static void sim_reset(uint64_t limit_us)
{
    g_ev_n = g_ev_i = 0;
    g_now = g_t0 = 0;
    g_limit = limit_us;
    g_tx_n = 0;
    g_sci_init_n = g_sci_deinit_n = g_timer_deinit_n = g_erase_n = 0;
}

static void ev_add(uint64_t t, int v)
{
    // 시간순 삽입
    int i = g_ev_n++;
    while (i > 0 && g_ev[i - 1].t > t) { g_ev[i] = g_ev[i - 1]; i--; }
    g_ev[i].t = t;
    g_ev[i].v = v;
}

static uint64_t pkt_add(uint64_t t, uint16_t cmd, uint16_t seq, const uint16_t *pl, uint16_t n)
{
    uint16_t pkt[BP_MAX_PACKET];
    uint16_t len = bp_build(pkt, cmd, seq, pl, n);
    for (uint16_t i = 0; i < len; i++) ev_add(t + (uint64_t)i * BYTE_US, pkt[i]);
    return t + (uint64_t)len * BYTE_US;
}

void hw_init_clocks(void) {}
void hw_copy_ramfuncs(void) {}
void hw_init_flash(void) {}
void hw_sci_init(void) { g_sci_init_n++; }
void hw_sci_deinit(void) { g_sci_deinit_n++; }
void hw_sci_flush(void) {}
void hw_timer_start(void) { g_t0 = g_now; }
void hw_timer_deinit(void) { g_timer_deinit_n++; }
void hw_reset(void) { longjmp(g_env, EXIT_RESET); }
void hw_jump_app(void) { longjmp(g_env, EXIT_APP); }

uint32_t hw_timer_us(void)
{
    uint64_t e = g_now - g_t0;
    return e > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)e;
}

int hw_sci_getc(void)
{
    if (g_now > g_limit) longjmp(g_env, EXIT_LIMIT);
    if (g_ev_i < g_ev_n && g_ev[g_ev_i].t <= g_now) {
        int v = g_ev[g_ev_i++].v;
        g_now += v < 0 ? 1 : BYTE_US;
        return v;
    }
    g_now += POLL_US;
    return HW_SCI_NONE;
}

void hw_sci_putc(uint16_t b)
{
    if (g_tx_n < (int)(sizeof(g_tx) / sizeof(g_tx[0]))) g_tx[g_tx_n++] = b & 0xFF;
}

uint16_t hw_flash_erase(uint16_t mask)
{
    g_erase_n++;
    if (mask == 0 || (mask & (uint16_t)~BP_ERASE_APP)) return BP_ST_BAD_PAYLOAD;
    if (mask & BP_ERASE_D) for (uint32_t a = 0x3E8000; a < 0x3EC000; a++) g_flash[a - FL_BASE] = 0xFFFF;
    if (mask & BP_ERASE_C) for (uint32_t a = 0x3EC000; a < 0x3F0000; a++) g_flash[a - FL_BASE] = 0xFFFF;
    if (mask & BP_ERASE_B) for (uint32_t a = 0x3F0000; a < 0x3F4000; a++) g_flash[a - FL_BASE] = 0xFFFF;
    return BP_ST_OK;
}

uint16_t hw_flash_program(uint32_t addr, const uint16_t *w, uint16_t n)
{
    if (addr < DSPIMG_APP_HDR_ADDR || addr + n > DSPIMG_APP_END) return BP_ST_BAD_ADDR;
    for (uint16_t i = 0; i < n; i++) g_flash[addr + i - FL_BASE] &= w[i];
    return BP_ST_OK;
}

// ── 시나리오 도우미 ──
static void flash_blank(void)
{
    for (size_t i = 0; i < sizeof(g_flash) / 2; i++) g_flash[i] = 0xFFFF;
}

static void flash_valid_app(void)
{
    flash_blank();
    uint16_t *code = &g_flash[DSPIMG_APP_CODE_ADDR - FL_BASE];
    for (int i = 0; i < 300; i++) code[i] = (uint16_t)(i * 7 + 1);
    dspimg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.app_len = 300;
    h.app_crc = dspimg_crc32_final(dspimg_crc32_words(dspimg_crc32_init(), code, 300));
    h.ver[0] = '2';
    dspimg_hdr_build(&h, &g_flash[0]);
}

static void ram_power_on(void)
{
    for (size_t i = 0; i < sizeof(g_ram) / 2; i++) g_ram[i] = (uint16_t)(0x1234 + i * 31);   // 전원 투입 쓰레기
}

static void set_bootflag(void)
{
    uint16_t *f = &g_ram[DSPIMG_BOOTFLAG_ADDR];
    f[0] = DSPIMG_BOOTFLAG_0; f[1] = DSPIMG_BOOTFLAG_1; f[2] = DSPIMG_BOOTFLAG_2; f[3] = DSPIMG_BOOTFLAG_3;
}

static void set_bootcnt(uint16_t n)
{
    g_ram[DSPIMG_BOOTCNT_ADDR] = DSPIMG_BOOTCNT_MAGIC;
    g_ram[DSPIMG_BOOTCNT_ADDR + 1] = n;
}

static uint16_t bootcnt(void)
{
    return g_ram[DSPIMG_BOOTCNT_ADDR] == DSPIMG_BOOTCNT_MAGIC ? g_ram[DSPIMG_BOOTCNT_ADDR + 1] : 0xFFFF;
}

static int run_boot(void)
{
    int r = setjmp(g_env);
    if (r == 0) {
        boot_main();
        return EXIT_NONE;
    }
    return r;
}

// 송신 기록에서 cmd 응답 개수 (CRC 맞는 것만)
static int count_resp(uint16_t cmd)
{
    int cnt = 0;
    for (int i = 0; i + 8 <= g_tx_n; ) {
        if (g_tx[i] != BP_SOF0 || g_tx[i + 1] != BP_SOF1) { i++; continue; }
        uint16_t len = (uint16_t)(g_tx[i + 2] | (g_tx[i + 3] << 8));
        if (i + 4 + len + 2 > g_tx_n) break;
        uint16_t crc = (uint16_t)(g_tx[i + 4 + len] | (g_tx[i + 5 + len] << 8));
        if (crc == bp_crc16(&g_tx[i + 4], len) && g_tx[i + 4] == (cmd | BP_RESP)) cnt++;
        i += 4 + len + 2;
    }
    return cnt;
}

#define MS(x)  ((uint64_t)(x) * 1000ULL)
#define SEC(x) ((uint64_t)(x) * 1000000ULL)

// 앱 유효, 아무것도 안 옴 → 약 200ms 뒤 앱으로 (SCI·타이머 원상 복구)
static void test_window_quiet(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    CHECK_EQ(run_boot(), EXIT_APP);
    CHECK(g_now >= MS(HELLO_WINDOW_MS) && g_now < MS(HELLO_WINDOW_MS) + 200);
    CHECK_EQ(g_sci_init_n, 1);
    CHECK_EQ(g_sci_deinit_n, 1);
    CHECK_EQ(g_timer_deinit_n, 1);
    CHECK_EQ(g_tx_n, 0);
    CHECK_EQ(bootcnt(), 1);                                  // 전원 투입 쓰레기 → 0 → +1
}

// 창 안의 노이즈·ASCII 프레임·가짜 SOF 는 창을 늘리지 않는다 (고정 200ms)
static void test_window_noise_fixed(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    for (uint64_t t = 0; t < MS(400); t += 300) {
        static const int pat[] = { 0x02, 'W', '1', 0x03, 0xC5, 0x3A, 0x55, 0xC5 };
        ev_add(t, pat[(t / 300) % 8]);
    }
    CHECK_EQ(run_boot(), EXIT_APP);
    CHECK(g_now >= MS(HELLO_WINDOW_MS) && g_now < MS(HELLO_WINDOW_MS) + 200);
    CHECK_EQ(g_tx_n, 0);
}

// 창 안의 HELLO → 응답하고 업데이트 모드, 이후 ERASE·RESET 처리
static void test_window_hello(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    uint64_t t = pkt_add(MS(120), BP_CMD_HELLO, 1, NULL, 0);
    uint16_t m = BP_ERASE_APP, mode = 0;
    t = pkt_add(t + MS(300), BP_CMD_ERASE, 2, &m, 1);
    pkt_add(t + MS(50), BP_CMD_RESET, 3, &mode, 1);
    CHECK_EQ(run_boot(), EXIT_RESET);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
    CHECK_EQ(count_resp(BP_CMD_ERASE), 1);
    CHECK_EQ(count_resp(BP_CMD_RESET), 1);
    CHECK_EQ(g_erase_n, 1);
    CHECK_EQ(g_sci_deinit_n, 0);                             // 앱으로 가지 않음
    CHECK_EQ(g_sci_init_n, 1);                               // 업데이트 모드가 SCI 를 다시 초기화하지 않음
    CHECK_EQ(bootcnt(), 0);                                  // 이번 부팅은 기동 실패로 세지 않음
}

// 브리지 구조 모드: 50ms 간격 HELLO 연속 송신 중 전원 투입 → 첫 HELLO 에서 진입
static void test_window_hello_stream(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(3));
    for (int i = 0; i < 40; i++) pkt_add(MS(33) + MS(50) * (uint64_t)i, BP_CMD_HELLO, (uint16_t)(i + 1), NULL, 0);
    CHECK_EQ(run_boot(), EXIT_LIMIT);                        // 업데이트 모드에서 계속 대기 (600초 전)
    CHECK(count_resp(BP_CMD_HELLO) >= 30);
    CHECK_EQ(g_sci_deinit_n, 0);
}

// 창 안의 ERASE·RESET 은 무시 (HELLO 만) → 앱으로
static void test_window_other_cmds(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    uint16_t m = BP_ERASE_APP, mode = 0;
    uint64_t t = pkt_add(MS(20), BP_CMD_ERASE, 1, &m, 1);
    pkt_add(t + MS(30), BP_CMD_RESET, 2, &mode, 1);
    CHECK_EQ(run_boot(), EXIT_APP);
    CHECK_EQ(g_erase_n, 0);
    CHECK_EQ(g_tx_n, 0);
}

// 노이즈로 시작된 가짜 패킷(긴 길이) 뒤에도 다음 HELLO(50ms 뒤)를 받는다 (창 안 재동기 간격 HELLO_GAP_MS)
static void test_window_resync(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    ev_add(MS(5), 0xC5); ev_add(MS(5) + 87, 0x3A); ev_add(MS(5) + 174, 0x00); ev_add(MS(5) + 261, 0x01);   // len 256
    pkt_add(MS(55), BP_CMD_HELLO, 1, NULL, 0);
    CHECK_EQ(run_boot(), EXIT_LIMIT);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
}

// 부트플래그 + 앱 유효, 브리지가 안 옴 → 60초 뒤 앱으로 (D-06)
static void test_bootflag_first_timeout(void)
{
    flash_valid_app(); ram_power_on(); set_bootflag(); sim_reset(SEC(700));
    CHECK_EQ(run_boot(), EXIT_RESET);
    CHECK(g_now >= SEC(FIRST_PKT_TIMEOUT_S) && g_now < SEC(FIRST_PKT_TIMEOUT_S) + MS(1));
    CHECK_EQ(g_ram[DSPIMG_BOOTFLAG_ADDR], 0);                // 부트플래그는 읽고 지움
    // 다음 부팅: 부트플래그 없음 → 창 → 앱
    CHECK_EQ(run_boot(), EXIT_APP);
}

// 부트플래그 + 첫 유효 패킷 뒤에는 600초 유휴 기준
static void test_bootflag_after_packet(void)
{
    flash_valid_app(); ram_power_on(); set_bootflag(); sim_reset(SEC(1300));
    pkt_add(SEC(30), BP_CMD_HELLO, 1, NULL, 0);
    CHECK_EQ(run_boot(), EXIT_RESET);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
    CHECK(g_now >= SEC(30) + SEC(IDLE_TIMEOUT_S) && g_now < SEC(30) + SEC(IDLE_TIMEOUT_S) + MS(10));
}

// 앱용 ASCII 프레임·노이즈는 시계를 되돌리지 않음 (부트플래그 60초 시한 그대로)
static void test_bootflag_noise_no_extend(void)
{
    flash_valid_app(); ram_power_on(); set_bootflag(); sim_reset(SEC(700));
    for (uint64_t t = SEC(1); t < SEC(100); t += MS(500)) ev_add(t, 0x02);
    CHECK_EQ(run_boot(), EXIT_RESET);
    CHECK(g_now < SEC(FIRST_PKT_TIMEOUT_S) + MS(1));
}

// 부트플래그 + 앱 무효 → 시한이 지나도 리셋하지 않고 계속 대기 (지금 규칙)
static void test_bootflag_app_invalid(void)
{
    flash_blank(); ram_power_on(); set_bootflag(); sim_reset(SEC(1300));
    CHECK_EQ(run_boot(), EXIT_LIMIT);
}

// 앱 무효 → 무기한 대기 (창 없이 바로 업데이트 모드, HELLO 응답)
static void test_app_invalid(void)
{
    flash_blank(); ram_power_on(); sim_reset(SEC(1300));
    pkt_add(SEC(900), BP_CMD_HELLO, 1, NULL, 0);
    CHECK_EQ(run_boot(), EXIT_LIMIT);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
    CHECK_EQ(g_sci_deinit_n, 0);
}

// 부트 카운터 초과 → 업데이트 모드, 앱이 유효하면 600초 뒤 복귀
static void test_bootcnt(void)
{
    flash_valid_app(); ram_power_on(); set_bootcnt(DSPIMG_BOOTCNT_MAX); sim_reset(SEC(700));
    CHECK_EQ(run_boot(), EXIT_RESET);
    CHECK(g_now >= SEC(IDLE_TIMEOUT_S) && g_now < SEC(IDLE_TIMEOUT_S) + MS(1));
    CHECK_EQ(bootcnt(), 0);
    // 카운터가 MAX 이하면 창을 거쳐 앱으로
    set_bootcnt(DSPIMG_BOOTCNT_MAX - 1); sim_reset(SEC(5));
    CHECK_EQ(run_boot(), EXIT_APP);
    CHECK_EQ(bootcnt(), DSPIMG_BOOTCNT_MAX);
}

// 수신 오류(브레이크) 뒤 파서 재동기 (D-03): 잘린 패킷 → RXERR → 곧바로 HELLO 가 와도 받는다
static void test_rxerr_resync(void)
{
    flash_blank(); ram_power_on(); sim_reset(SEC(10));
    uint64_t t = SEC(1);
    const int part[] = { 0xC5, 0x3A, 0x10, 0x00, 0x03, 0x01 };     // len 16 의 머리만
    for (int i = 0; i < 6; i++) ev_add(t + (uint64_t)i * BYTE_US, part[i]);
    t += 6 * BYTE_US;
    ev_add(t + 10, HW_SCI_RXERR);
    pkt_add(t + 200, BP_CMD_HELLO, 1, NULL, 0);                    // 바이트 공백(100ms)보다 훨씬 빨리
    CHECK_EQ(run_boot(), EXIT_LIMIT);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
}

// 창 안의 수신 오류도 재동기
static void test_window_rxerr(void)
{
    flash_valid_app(); ram_power_on(); sim_reset(SEC(5));
    const int part[] = { 0xC5, 0x3A, 0x10, 0x00 };
    for (int i = 0; i < 4; i++) ev_add(MS(10) + (uint64_t)i * BYTE_US, part[i]);
    ev_add(MS(10) + 400, HW_SCI_RXERR);
    pkt_add(MS(10) + 500, BP_CMD_HELLO, 1, NULL, 0);
    CHECK_EQ(run_boot(), EXIT_LIMIT);
    CHECK_EQ(count_resp(BP_CMD_HELLO), 1);
}

int main(void)
{
    test_window_quiet();
    test_window_noise_fixed();
    test_window_hello();
    test_window_hello_stream();
    test_window_other_cmds();
    test_window_resync();
    test_bootflag_first_timeout();
    test_bootflag_after_packet();
    test_bootflag_noise_no_extend();
    test_bootflag_app_invalid();
    test_app_invalid();
    test_bootcnt();
    test_rxerr_resync();
    test_window_rxerr();
    return test_summary("boot_main sim");
}
