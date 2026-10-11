// dsp/app/app_fwupdate.c 호스트 시험 — 업데이트 진입 판정 (D-07)
//  safe 콜백 NULL → 거부(UNG09), 요청 프레임 형식(길이 11·16진 PAN/MSG·숫자 호기·"000"·공백·BCC) 검사, V 응답,
//  브리지 main/dsp_app_proto.c 와의 프레임 계약 (브리지가 만든 F 를 앱이 받고, 앱의 R 을 브리지가 해석)
#include <string.h>
#include "app_fwupdate.h"
#include "dsp_app_proto.h"
#include "dsp_image.h"
#include "test_util.h"

// 가짜 메모리: 앱 헤더(0x3E8000), 부트로더 정보(0x3F7F00), 부트 카운터·플래그(0x7FA~0x7FF)
static uint16_t g_apphdr[DSPIMG_HDR_WORDS], g_bootinfo[64], g_ram[8], g_dummy[4];

volatile uint16_t *host_fwup_mem(uint32_t a)
{
    if (a == DSPIMG_APP_HDR_ADDR) return g_apphdr;
    if (a == DSPIMG_BOOTINFO_ADDR) return g_bootinfo;
    if (a >= 0x7F8 && a < 0x800) return &g_ram[a - 0x7F8];
    CHECK(0 && "알 수 없는 주소");
    return g_dummy;
}

static int g_safe_calls;
static uint16_t safe_ok(void) { g_safe_calls++; return 0; }
static uint16_t safe_moving(void) { g_safe_calls++; return 1; }

// "F1107U000 " + BCC 형태 payload 를 만든다
static uint16_t make(char *p, const char *body10)
{
    memcpy(p, body10, 10);
    p[10] = fwup_bcc(p, 10);
    return 11;
}

static int handle(const char *p, uint16_t n, fwup_safe_fn safe, char *reply, uint16_t *rlen)
{
    *rlen = 0;
    return fwup_handle(p, n, 7, safe, reply, FWUP_REPLY_MAX, rlen);
}

static void setup_mem(void)
{
    dspimg_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.app_len = 100; h.app_crc = 1; h.build_time = 1700000000UL;   // 2023-11-14
    memcpy(h.ver, (const uint16_t[]){ '1', '.', '2', '.', '3', 0 }, 6 * sizeof(uint16_t));
    dspimg_hdr_build(&h, g_apphdr);
    memset(g_bootinfo, 0, sizeof(g_bootinfo));
    g_bootinfo[0] = DSPIMG_BOOT_MAGIC0; g_bootinfo[1] = DSPIMG_BOOT_MAGIC1;
    g_bootinfo[4] = '1'; g_bootinfo[5] = '.'; g_bootinfo[6] = '0';
    memset(g_ram, 0, sizeof(g_ram));
}

static void test_update_entry(void)
{
    char p[32], r[FWUP_REPLY_MAX];
    uint16_t n, rl;
    setup_mem();

    n = make(p, "F1107U000 ");
    g_safe_calls = 0;
    CHECK_EQ(handle(p, n, safe_ok, r, &rl), FWUP_REPLY_RESET);
    CHECK_EQ(g_safe_calls, 1);
    CHECK_EQ(rl, 14);
    CHECK(r[0] == 0x02 && r[rl - 1] == 0x03);
    CHECK(memcmp(&r[1], "R1107UOK00 ", 11) == 0);
    CHECK_EQ(r[12], fwup_bcc(&r[1], 11));
    CHECK_EQ(g_ram[2], DSPIMG_BOOTCNT_MAGIC);                // fwup_boot_ok 보조 호출 (0x7FA)
    CHECK_EQ(g_ram[3], 0);

    // safe 콜백이 없으면 거부 (fail-closed)
    CHECK_EQ(handle(p, n, NULL, r, &rl), FWUP_REPLY);
    CHECK(memcmp(&r[1], "R1107UNG09 ", 11) == 0);

    // 주행 중
    CHECK_EQ(handle(p, n, safe_moving, r, &rl), FWUP_REPLY);
    CHECK(memcmp(&r[1], "R1107UNG01 ", 11) == 0);

    // PAN·MSG 16진 A~F 허용
    n = make(p, "FAF07U000 ");
    CHECK_EQ(handle(p, n, safe_ok, r, &rl), FWUP_REPLY_RESET);
    CHECK(memcmp(&r[1], "RAF07UOK00 ", 11) == 0);
}

static void test_reject_malformed(void)
{
    char p[32], r[FWUP_REPLY_MAX];
    uint16_t rl;
    setup_mem();
    // 자리별로 하나씩 깨뜨린 요청 (BCC 는 맞게 다시 계산) → 모두 무응답, 안전 콜백도 부르지 않음
    const char *bad[] = {
        "F0107U000 ",   // PAN '0'
        "F1G07U000 ",   // MSG 16진 아님
        "F1a07U000 ",   // 소문자
        "F11 7U000 ",   // 호기 숫자 아님
        "F110:U000 ",
        "F1107U001 ",   // 본문 "000" 아님
        "F1107UX00 ",
        "F1107U0000",   // BCC 앞 공백 없음
        "F1108U000 ",   // 다른 호기
        "F1107Z000 ",   // 모르는 종류
        "E1107U000 ",   // F 아님
    };
    g_safe_calls = 0;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint16_t n = make(p, bad[i]);
        int rc = handle(p, n, safe_ok, r, &rl);
        if (rc != FWUP_NONE) fprintf(stderr, "  accepted: %s\n", bad[i]);
        CHECK_EQ(rc, FWUP_NONE);
        CHECK_EQ(rl, 0);
    }
    CHECK_EQ(g_safe_calls, 0);

    // 길이: 10·12자 (BCC 는 맞춤)
    memcpy(p, "F1107U00 ", 9); p[9] = fwup_bcc(p, 9);
    CHECK_EQ(handle(p, 10, safe_ok, r, &rl), FWUP_NONE);
    memcpy(p, "F1107U0000 ", 11); p[11] = fwup_bcc(p, 11);
    CHECK_EQ(handle(p, 12, safe_ok, r, &rl), FWUP_NONE);
    // BCC 오류
    make(p, "F1107U000 ");
    p[10] = p[10] == '0' ? '1' : '0';
    CHECK_EQ(handle(p, 11, safe_ok, r, &rl), FWUP_NONE);
    // reply 버퍼가 작으면 처리 안 함
    make(p, "F1107U000 ");
    CHECK_EQ(fwup_handle(p, 11, 7, safe_ok, r, FWUP_REPLY_MAX - 1, &rl), FWUP_NONE);
    CHECK_EQ(g_safe_calls, 0);
}

static void test_version_query(void)
{
    char p[32], r[FWUP_REPLY_MAX];
    uint16_t n, rl;
    setup_mem();
    n = make(p, "F1307V000 ");
    CHECK_EQ(handle(p, n, NULL, r, &rl), FWUP_REPLY);        // V 는 안전 조건과 무관
    CHECK_EQ(rl, 34);
    CHECK(memcmp(&r[1], "R1307V1.2.3   1.0     20231114 ", 31) == 0);
    CHECK_EQ(r[32], fwup_bcc(&r[1], 31));
    CHECK_EQ(r[33], 0x03);
}

// 브리지 쪽 F 프레임 → 앱 → R 프레임 → 브리지 해석
static void test_bridge_contract(void)
{
    uint8_t f[DAP_FRAME_MAX];
    char r[FWUP_REPLY_MAX];
    uint16_t rl;
    dap_reply_t rep;
    setup_mem();
    // BCC: 하위 니블 ASCII-HEX ('H' = 0x48 → '8')
    CHECK_EQ(dap_bcc((const uint8_t *)"H", 1), '8');
    CHECK_EQ(fwup_bcc("H", 1), '8');
    const char pans[] = "19AF";
    for (int k = 0; k < 4; k++) {
        size_t n = dap_build_query(f, sizeof(f), pans[k], 'C', 7, 'U');
        CHECK_EQ(n, 13);
        CHECK_EQ(fwup_handle((const char *)f + 1, (uint16_t)(n - 2), 7, safe_ok, r, sizeof(r), &rl), FWUP_REPLY_RESET);
        CHECK_EQ(dap_parse_reply((const uint8_t *)r, rl, 7, 'C', &rep), 0);
        CHECK(rep.cmd == 'U' && rep.ok && rep.reason == 0);
        CHECK_EQ(fwup_handle((const char *)f + 1, (uint16_t)(n - 2), 7, NULL, r, sizeof(r), &rl), FWUP_REPLY);
        CHECK_EQ(dap_parse_reply((const uint8_t *)r, rl, 7, 'C', &rep), 0);
        CHECK(rep.cmd == 'U' && !rep.ok && rep.reason == 9);
    }
    size_t n = dap_build_query(f, sizeof(f), '1', '5', 42, 'V');
    CHECK_EQ(fwup_handle((const char *)f + 1, (uint16_t)(n - 2), 42, NULL, r, sizeof(r), &rl), FWUP_REPLY);
    CHECK_EQ(dap_parse_reply((const uint8_t *)r, rl, 42, '5', &rep), 0);
    CHECK(strcmp(rep.app_ver, "1.2.3") == 0);
    CHECK(strcmp(rep.boot_ver, "1.0") == 0);
    CHECK(strcmp(rep.build, "20231114") == 0);
    // 다른 호기 번호의 F 는 앱이 무시
    n = dap_build_query(f, sizeof(f), '1', '5', 41, 'U');
    CHECK_EQ(fwup_handle((const char *)f + 1, (uint16_t)(n - 2), 42, safe_ok, r, sizeof(r), &rl), FWUP_NONE);
}

int main(void)
{
    test_update_entry();
    test_reject_malformed();
    test_version_query();
    test_bridge_contract();
    return test_summary("app_fwupdate");
}
