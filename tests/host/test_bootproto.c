// dsp/common/bootproto.c·dsp_image.c 호스트 시험 — 부트로더(섹터 A, OTA 로 못 바꿈)와 브리지가 같이 쓰는 코드
//  CRC 벡터(패커와 대조), 패킷 왕복, 섹터 A 쓰기 거부, 잘린 패킷 재동기, 재전송 판정(seq·cmd·CRC),
//  READ 주소 범위, HELLO 전용 수신(accept_cmd), 기준 .dspfw(golden) 해석
#include <stdlib.h>
#include <string.h>
#include "bootproto.h"
#include "test_util.h"

// ── 가짜 플래시 (0x3D7800 OTP, 0x3E8000~0x3F7FFF 플래시) ──
#define FL_BASE 0x3E8000UL
#define FL_END  0x3F8000UL
#define OTP_BASE 0x3D7800UL
#define OTP_END  0x3D7C00UL
static uint16_t g_flash[FL_END - FL_BASE];
static uint16_t g_otp[OTP_END - OTP_BASE];
static int g_erase_calls, g_prog_calls, g_bad_reads;

static uint16_t *mem_at(uint32_t a)
{
    if (a >= FL_BASE && a < FL_END) return &g_flash[a - FL_BASE];
    if (a >= OTP_BASE && a < OTP_END) return &g_otp[a - OTP_BASE];
    return NULL;
}

static uint16_t fake_erase(uint16_t mask)
{
    g_erase_calls++;
    if (mask & BP_ERASE_D) for (uint32_t a = 0x3E8000; a < 0x3EC000; a++) *mem_at(a) = 0xFFFF;
    if (mask & BP_ERASE_C) for (uint32_t a = 0x3EC000; a < 0x3F0000; a++) *mem_at(a) = 0xFFFF;
    if (mask & BP_ERASE_B) for (uint32_t a = 0x3F0000; a < 0x3F4000; a++) *mem_at(a) = 0xFFFF;
    return BP_ST_OK;
}

static uint16_t fake_program(uint32_t addr, const uint16_t *w, uint16_t n)
{
    g_prog_calls++;
    for (uint16_t i = 0; i < n; i++) *mem_at(addr + i) &= w[i];
    return BP_ST_OK;
}

static void fake_read(uint32_t addr, uint16_t *w, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint16_t *p = mem_at(addr + i);
        if (!p) { g_bad_reads++; w[i] = 0xDEAD; } else w[i] = *p;
    }
}

static const uint16_t BOOTVER[BP_BOOTVER_LEN] = { '1', '.', '0', '.', '0', 0 };
static const bp_ops_t OPS = { fake_erase, fake_program, fake_read, BOOTVER };
static bp_ctx_t ctx;

static void reset_flash(void)
{
    for (size_t i = 0; i < sizeof(g_flash) / 2; i++) g_flash[i] = 0xFFFF;
    for (size_t i = 0; i < sizeof(g_otp) / 2; i++) g_otp[i] = (uint16_t)(0x5000 + i);
    g_erase_calls = g_prog_calls = g_bad_reads = 0;
}

// 패킷을 바이트 단위로 넣고 응답 payload(상태 포함)를 돌려줌. 응답 없으면 -1
static int xfer_raw(const uint16_t *pkt, uint16_t n, uint16_t *resp, uint16_t *resp_cmd, uint16_t *resp_seq)
{
    int got = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (bp_rx_byte(&ctx, pkt[i])) {
            CHECK(i == n - 1);
            got = 1;
        }
    }
    if (!got) return -1;
    // 응답도 공용 형식: SOF, len, cmd|0x80, seq, payload, crc
    CHECK_EQ(ctx.tx[0], BP_SOF0);
    CHECK_EQ(ctx.tx[1], BP_SOF1);
    uint16_t len = (uint16_t)(ctx.tx[2] | (ctx.tx[3] << 8));
    CHECK_EQ(ctx.tx_len, 4 + len + 2);
    uint16_t crc = (uint16_t)(ctx.tx[4 + len] | (ctx.tx[5 + len] << 8));
    CHECK_EQ(crc, bp_crc16(&ctx.tx[4], len));
    if (resp_cmd) *resp_cmd = ctx.tx[4];
    if (resp_seq) *resp_seq = ctx.tx[5];
    for (uint16_t i = 0; i < len - 2; i++) resp[i] = ctx.tx[6 + i];
    return len - 2;
}

static int xfer(uint16_t cmd, uint16_t seq, const uint16_t *pl, uint16_t n, uint16_t *resp)
{
    uint16_t pkt[BP_MAX_PACKET], rc, rs;
    uint16_t len = bp_build(pkt, cmd, seq, pl, n);
    int r = xfer_raw(pkt, len, resp, &rc, &rs);
    if (r >= 0) {
        CHECK_EQ(rc, cmd | BP_RESP);
        CHECK_EQ(rs, seq & 0xFF);
    }
    return r;
}

static uint16_t write_pl(uint16_t *pl, uint32_t addr, const uint16_t *w, uint16_t nw)
{
    pl[0] = addr & 0xFF; pl[1] = (addr >> 8) & 0xFF; pl[2] = (addr >> 16) & 0xFF; pl[3] = (addr >> 24) & 0xFF;
    pl[4] = nw & 0xFF; pl[5] = nw >> 8;
    for (uint16_t i = 0; i < nw; i++) { pl[6 + i * 2] = w[i] & 0xFF; pl[7 + i * 2] = w[i] >> 8; }
    return (uint16_t)(6 + nw * 2);
}

static uint16_t read_pl(uint16_t *pl, uint32_t addr, uint16_t nw)
{
    pl[0] = addr & 0xFF; pl[1] = (addr >> 8) & 0xFF; pl[2] = (addr >> 16) & 0xFF; pl[3] = (addr >> 24) & 0xFF;
    pl[4] = nw & 0xFF; pl[5] = nw >> 8;
    return 6;
}

static void test_crc_vectors(void)
{
    const char *s = "123456789";
    uint16_t b[9];
    for (int i = 0; i < 9; i++) b[i] = (uint16_t)s[i];
    CHECK_EQ(dspimg_crc32_final(dspimg_crc32_bytes(dspimg_crc32_init(), b, 9)), 0xCBF43926UL);
    CHECK_EQ(dspimg_crc16_bytes(b, 9), 0x29B1);
    // tools/dsp_pack.py selftest 와 같은 값 ("12345678" 을 하위 바이트 먼저 워드로)
    const uint16_t w[4] = { 0x3231, 0x3433, 0x3635, 0x3837 };
    CHECK_EQ(dspimg_crc32_final(dspimg_crc32_words(dspimg_crc32_init(), w, 4)), 0x9AE0DAAFUL);
    CHECK_EQ(dspimg_crc16_words(w, 4), 0xA12B);
    dspimg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.app_len = 0x1234; h.app_crc = 0xDEADBEEFUL; h.build_time = 1700000000UL;
    h.ver[0] = '1'; h.ver[1] = '.'; h.ver[2] = '2'; h.ver[3] = '.'; h.ver[4] = '3';
    uint16_t hdr[DSPIMG_HDR_WORDS];
    dspimg_hdr_build(&h, hdr);
    CHECK_EQ(hdr[DSPIMG_W_HDRCRC], 0xBB14);                // dsp_pack.py selftest 의 hdr[62]
    dspimg_hdr_t back;
    CHECK_EQ(dspimg_hdr_parse(hdr, &back), DSPIMG_OK);
    CHECK_EQ(back.app_len, 0x1234);
    hdr[DSPIMG_W_VER] ^= 1;
    CHECK_EQ(dspimg_hdr_parse(hdr, &back), DSPIMG_E_HDRCRC);
}

static void test_hello_blank(void)
{
    uint16_t resp[BP_MAX_PAYLOAD];
    reset_flash();
    bp_init(&ctx, &OPS);
    int n = xfer(BP_CMD_HELLO, 1, NULL, 0, resp);
    CHECK_EQ(n, BP_HELLO_RESP_LEN);
    CHECK_EQ(resp[0], BP_ST_OK);
    CHECK_EQ(resp[1], BP_PROTO_VER);
    CHECK_EQ(resp[2] | (resp[3] << 8), BP_BLOCK_WORDS);
    CHECK_EQ(resp[4], 0);                                   // 빈 플래시: 앱 무효
    CHECK_EQ(resp[5], '1');
}

static void test_write_ranges(void)
{
    uint16_t resp[BP_MAX_PAYLOAD], pl[BP_MAX_PAYLOAD], w[4] = { 1, 2, 3, 4 };
    reset_flash();
    bp_init(&ctx, &OPS);
    // 섹터 A (부트로더) 는 절대 쓰지 않음
    CHECK_EQ(xfer(BP_CMD_WRITE, 1, pl, write_pl(pl, 0x3F4000, w, 4), resp), 5);
    CHECK_EQ(resp[0], BP_ST_BAD_ADDR);
    // 앱 영역 끝에 걸침
    CHECK_EQ(xfer(BP_CMD_WRITE, 2, pl, write_pl(pl, 0x3F3FFE, w, 4), resp), 5);
    CHECK_EQ(resp[0], BP_ST_BAD_ADDR);
    // 앱 아래
    CHECK_EQ(xfer(BP_CMD_WRITE, 3, pl, write_pl(pl, 0x3E7FFF, w, 4), resp), 5);
    CHECK_EQ(resp[0], BP_ST_BAD_ADDR);
    CHECK_EQ(g_prog_calls, 0);
    // 정상
    CHECK_EQ(xfer(BP_CMD_WRITE, 4, pl, write_pl(pl, 0x3E8040, w, 4), resp), 5);
    CHECK_EQ(resp[0], BP_ST_OK);
    CHECK_EQ(g_flash[0x40], 1);
    CHECK_EQ(g_flash[0x43], 4);
    // 길이 불일치
    uint16_t n = write_pl(pl, 0x3E8040, w, 4);
    CHECK_EQ(xfer(BP_CMD_WRITE, 5, pl, (uint16_t)(n - 1), resp), 5);
    CHECK_EQ(resp[0], BP_ST_BAD_LEN);
    // ERASE: 섹터 A 비트 거부
    uint16_t m = 0x08;
    CHECK_EQ(xfer(BP_CMD_ERASE, 6, &m, 1, resp), 1);
    CHECK_EQ(resp[0], BP_ST_BAD_PAYLOAD);
    CHECK_EQ(g_erase_calls, 0);
}

static void test_resync(void)
{
    uint16_t resp[BP_MAX_PAYLOAD], pkt[BP_MAX_PACKET];
    reset_flash();
    bp_init(&ctx, &OPS);
    // 잡음 + 잘린 패킷 (WRITE 머리만) → 파서가 본문을 기다리는 중
    const uint16_t noise[] = { 0x02, 'A', 0x03, 0xC5, 0xC5, 0x3A, 0x10, 0x00, 0x03, 0x07 };
    for (size_t i = 0; i < sizeof(noise) / sizeof(noise[0]); i++) CHECK(!bp_rx_byte(&ctx, noise[i]));
    CHECK(bp_rx_busy(&ctx));
    // 재동기 없이 HELLO 를 넣으면 본문으로 삼켜져 응답이 없다
    uint16_t len = bp_build(pkt, BP_CMD_HELLO, 9, NULL, 0);
    CHECK_EQ(xfer_raw(pkt, len, resp, NULL, NULL), -1);
    // 호출 측의 바이트 공백 판정 → bp_rx_reset → 다음 HELLO 정상
    bp_rx_reset(&ctx);
    CHECK(!bp_rx_busy(&ctx));
    CHECK_EQ(xfer(BP_CMD_HELLO, 10, NULL, 0, resp), BP_HELLO_RESP_LEN);
    // 길이 이상(최대 초과) → 스스로 재동기
    const uint16_t badlen[] = { 0xC5, 0x3A, 0xFF, 0x7F };
    for (size_t i = 0; i < 4; i++) bp_rx_byte(&ctx, badlen[i]);
    CHECK(!bp_rx_busy(&ctx));
    CHECK_EQ(xfer(BP_CMD_HELLO, 11, NULL, 0, resp), BP_HELLO_RESP_LEN);
    // CRC 오류 → 무응답, 다음 패킷 정상
    len = bp_build(pkt, BP_CMD_HELLO, 12, NULL, 0);
    pkt[len - 1] ^= 0x55;
    CHECK_EQ(xfer_raw(pkt, len, resp, NULL, NULL), -1);
    CHECK_EQ(xfer(BP_CMD_HELLO, 12, NULL, 0, resp), BP_HELLO_RESP_LEN);
}

static void test_retransmit(void)
{
    uint16_t resp[BP_MAX_PAYLOAD], pl[BP_MAX_PAYLOAD], w[2] = { 0x1111, 0x2222 };
    reset_flash();
    bp_init(&ctx, &OPS);
    uint16_t n = write_pl(pl, 0x3E8100, w, 2);
    CHECK_EQ(xfer(BP_CMD_WRITE, 7, pl, n, resp), 5);
    CHECK_EQ(g_prog_calls, 1);
    // 응답 유실 → 같은 패킷 재전송: 다시 쓰지 않고 같은 응답
    CHECK_EQ(xfer(BP_CMD_WRITE, 7, pl, n, resp), 5);
    CHECK_EQ(resp[0], BP_ST_OK);
    CHECK_EQ(g_prog_calls, 1);
    // 같은 seq·cmd 인데 payload 가 다르면 새 요청 (D-10: 연속 실행한 도구가 seq 를 1 부터 다시 씀)
    n = write_pl(pl, 0x3E8200, w, 2);
    CHECK_EQ(xfer(BP_CMD_WRITE, 7, pl, n, resp), 5);
    CHECK_EQ(g_prog_calls, 2);
    CHECK_EQ(g_flash[0x200], 0x1111);

    // READ: 같은 seq 로 다른 주소를 읽으면 그 주소의 값
    g_flash[0x0000] = 0xAAAA;
    g_flash[0x8000] = 0xBBBB;                               // 0x3F0000
    CHECK_EQ(xfer(BP_CMD_READ, 1, pl, read_pl(pl, 0x3E8000, 1), resp), 3);
    CHECK_EQ(resp[1] | (resp[2] << 8), 0xAAAA);
    CHECK_EQ(xfer(BP_CMD_READ, 1, pl, read_pl(pl, 0x3F0000, 1), resp), 3);
    CHECK_EQ(resp[0], BP_ST_OK);
    CHECK_EQ(resp[1] | (resp[2] << 8), 0xBBBB);
}

static void test_read_ranges(void)
{
    uint16_t resp[BP_MAX_PAYLOAD], pl[8];
    reset_flash();
    bp_init(&ctx, &OPS);
    uint16_t seq = 20;
    // 주변장치 레지스터 (SCIRXBUF 0x7057) → 읽지 않음
    CHECK_EQ(xfer(BP_CMD_READ, seq++, pl, read_pl(pl, 0x7057, 1), resp), 1);
    CHECK_EQ(resp[0], BP_ST_BAD_ADDR);
    // RAM, 플래시 앞·뒤에 걸침, OTP 끝에 걸침, 주소 넘침
    const uint32_t bad[][2] = { { 0x000000, 4 }, { 0x3E7FFF, 2 }, { 0x3F7FFF, 2 }, { 0x3D7BFF, 2 },
                                { 0xFFFFFFFFUL, 2 }, { 0x3F8000, 1 } };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK_EQ(xfer(BP_CMD_READ, seq++, pl, read_pl(pl, bad[i][0], (uint16_t)bad[i][1]), resp), 1);
        CHECK_EQ(resp[0], BP_ST_BAD_ADDR);
    }
    CHECK_EQ(g_bad_reads, 0);
    // 허용: 앱, 섹터 A 끝(부트로더 정보·CSM), OTP
    const uint32_t good[][2] = { { 0x3E8000, 128 }, { 0x3F7F00, 64 }, { 0x3F7F80, 128 }, { 0x3D7800, 16 }, { 0x3D7BF0, 16 } };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        int r = xfer(BP_CMD_READ, seq++, pl, read_pl(pl, good[i][0], (uint16_t)good[i][1]), resp);
        CHECK_EQ(r, 1 + good[i][1] * 2);
        CHECK_EQ(resp[0], BP_ST_OK);
    }
    CHECK_EQ(xfer(BP_CMD_READ, seq++, pl, read_pl(pl, 0x3D7800, 1), resp), 3);
    CHECK_EQ(resp[1] | (resp[2] << 8), 0x5000);
    CHECK_EQ(g_bad_reads, 0);
    // 길이 0·초과
    CHECK_EQ(xfer(BP_CMD_READ, seq++, pl, read_pl(pl, 0x3E8000, 0), resp), 1);
    CHECK_EQ(resp[0], BP_ST_BAD_LEN);
    CHECK_EQ(xfer(BP_CMD_READ, seq++, pl, read_pl(pl, 0x3E8000, BP_BLOCK_WORDS + 1), resp), 1);
    CHECK_EQ(resp[0], BP_ST_BAD_LEN);
}

static void test_accept_only_hello(void)
{
    uint16_t resp[BP_MAX_PAYLOAD], m = BP_ERASE_APP;
    reset_flash();
    bp_init(&ctx, &OPS);
    ctx.accept_cmd = BP_CMD_HELLO;
    CHECK_EQ(xfer(BP_CMD_ERASE, 1, &m, 1, resp), -1);       // HELLO 창: 다른 명령은 무응답, 실행 안 함
    CHECK_EQ(g_erase_calls, 0);
    CHECK_EQ(xfer(BP_CMD_RESET, 2, NULL, 0, resp), -1);
    CHECK(!ctx.reset_pending);
    CHECK_EQ(xfer(BP_CMD_HELLO, 3, NULL, 0, resp), BP_HELLO_RESP_LEN);
    ctx.accept_cmd = 0;
    CHECK_EQ(xfer(BP_CMD_ERASE, 4, &m, 1, resp), 1);
    CHECK_EQ(g_erase_calls, 1);
}

// ── 기준 .dspfw (tools/dsp_pack.py 출력) 를 C 쪽 규칙으로 해석: 파일 CRC, 앱 헤더, 블록 → 앱 CRC ──
static uint16_t *g_file;
static long g_file_len;

static uint32_t le32(long off)
{
    return (uint32_t)g_file[off] | ((uint32_t)g_file[off + 1] << 8) | ((uint32_t)g_file[off + 2] << 16) |
           ((uint32_t)g_file[off + 3] << 24);
}

static void test_golden(const char *path)
{
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) return;
    static unsigned char raw[65536];
    g_file_len = (long)fread(raw, 1, sizeof(raw), f);
    fclose(f);
    g_file = malloc(sizeof(uint16_t) * (size_t)g_file_len);
    for (long i = 0; i < g_file_len; i++) g_file[i] = raw[i];

    CHECK(memcmp(raw, "AGVDSP1\0", 8) == 0);
    CHECK_EQ(le32(8), (uint32_t)g_file_len);
    uint32_t fcrc = dspimg_crc32_final(dspimg_crc32_bytes(dspimg_crc32_init(), &g_file[32], (uint32_t)(g_file_len - 32)));
    CHECK_EQ(fcrc, le32(12));
    CHECK(memcmp(&raw[16], "1.2.3\0", 6) == 0);

    uint16_t hdr[DSPIMG_HDR_WORDS];
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++) hdr[i] = (uint16_t)(g_file[32 + i * 2] | (g_file[33 + i * 2] << 8));
    dspimg_hdr_t h;
    CHECK_EQ(dspimg_hdr_parse(hdr, &h), DSPIMG_OK);
    CHECK_EQ(h.app_len, 400);
    CHECK_EQ(h.app_crc, 0xF55BEFB2UL);
    CHECK_EQ(h.build_time, 1700000000UL);
    CHECK(h.ver[0] == '1' && h.ver[1] == '.' && h.ver[4] == '3' && h.ver[5] == 0);

    // 블록을 부트로더 프로토콜(WRITE)로 가짜 플래시에 굽고 VERIFY — 브리지가 하는 순서 그대로
    uint16_t resp[BP_MAX_PAYLOAD], pl[BP_MAX_PAYLOAD], w[BP_BLOCK_WORDS], m = BP_ERASE_APP;
    reset_flash();
    bp_init(&ctx, &OPS);
    uint16_t seq = 1;
    CHECK_EQ(xfer(BP_CMD_ERASE, seq++, &m, 1, resp), 1);
    long pos = 32 + DSPIMG_HDR_WORDS * 2;
    int blocks = 0;
    while (pos < g_file_len) {
        uint32_t addr = le32(pos);
        uint16_t n = (uint16_t)(g_file[pos + 4] | (g_file[pos + 5] << 8));
        pos += 6;
        CHECK(n > 0 && n <= BP_BLOCK_WORDS);
        for (uint16_t i = 0; i < n; i++) w[i] = (uint16_t)(g_file[pos + i * 2] | (g_file[pos + i * 2 + 1] << 8));
        pos += n * 2;
        CHECK_EQ(xfer(BP_CMD_WRITE, seq++, pl, write_pl(pl, addr, w, n), resp), 5);
        CHECK_EQ(resp[0], BP_ST_OK);
        blocks++;
    }
    CHECK_EQ(pos, g_file_len);
    CHECK_EQ(blocks, 3);
    CHECK_EQ(xfer(BP_CMD_VERIFY, seq++, NULL, 0, resp), 11);
    CHECK_EQ(resp[1], 0);                                   // 헤더 전: 무효
    CHECK_EQ(xfer(BP_CMD_WRITE, seq++, pl, write_pl(pl, DSPIMG_APP_HDR_ADDR, hdr, DSPIMG_HDR_WORDS), resp), 5);
    CHECK_EQ(xfer(BP_CMD_VERIFY, seq++, NULL, 0, resp), 11);
    CHECK_EQ(resp[1], 1);
    CHECK_EQ((uint32_t)resp[2] | ((uint32_t)resp[3] << 8) | ((uint32_t)resp[4] << 16) | ((uint32_t)resp[5] << 24),
             0xF55BEFB2UL);
    CHECK_EQ(resp[10], DSPIMG_OK);
    CHECK_EQ(xfer(BP_CMD_HELLO, seq++, NULL, 0, resp), BP_HELLO_RESP_LEN);
    CHECK_EQ(resp[4], 1);
    CHECK(resp[21] == '1' && resp[25] == '3' && resp[26] == 0);
    // RESET 은 응답 뒤 호출 측이 수행
    uint16_t mode = 0;
    CHECK_EQ(xfer(BP_CMD_RESET, seq++, &mode, 1, resp), 1);
    CHECK(ctx.reset_pending);
    free(g_file);
}

int main(int argc, char **argv)
{
    test_crc_vectors();
    test_hello_blank();
    test_write_ranges();
    test_resync();
    test_retransmit();
    test_read_ranges();
    test_accept_only_hello();
    test_golden(argc > 1 ? argv[1] : "golden/app_golden.dspfw");
    return test_summary("bootproto");
}
