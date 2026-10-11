// 브리지 메인보드 OTA 검사 도우미 호스트 시험 (main/dsp_app_proto.c)
//   ./run.sh  (gcc + python3, tools/dsp_pack.py 로 만든 .dspfw 와도 대조)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp_app_proto.h"
#include "dsp_image.h"

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail++; } } while (0)

// ───────── 메모리 이미지 ─────────

typedef struct { uint8_t *b; uint32_t n; } buf_t;

static bool mem_read(void *ctx, uint32_t off, void *dst, size_t n)
{
    buf_t *m = ctx;
    if (off > m->n || n > m->n - off) return false;
    memcpy(dst, m->b + off, n);
    return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

typedef struct { uint32_t addr; uint16_t nw; } blk_t;

// flat[app_len] 코드에서 blocks 대로 파일 생성 (dsp_pack.py pack 과 같은 배치). app_crc 는 flat 기준
static uint32_t build(uint8_t *out, const uint16_t *flat, uint32_t app_len, const blk_t *bl, int nb, uint32_t *app_crc)
{
    dspimg_hdr_t h = { .app_len = app_len, .build_time = 1700000000 };
    uint16_t hdr[DSPIMG_HDR_WORDS];
    h.app_crc = dspimg_crc32_final(dspimg_crc32_words(dspimg_crc32_init(), flat, app_len));
    const char *v = "1.2.3";
    for (int i = 0; v[i]; i++) h.ver[i] = (uint8_t)v[i];
    dspimg_hdr_build(&h, hdr);
    uint32_t pos = 32;
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++, pos += 2) put16(out + pos, hdr[i]);
    for (int b = 0; b < nb; b++) {
        put32(out + pos, bl[b].addr);
        put16(out + pos + 4, bl[b].nw);
        pos += 6;
        for (uint32_t i = 0; i < bl[b].nw; i++, pos += 2) {
            uint32_t a = bl[b].addr + i - DSPIMG_APP_CODE_ADDR;
            put16(out + pos, a < app_len ? flat[a] : 0xFFFF);
        }
    }
    memcpy(out, "AGVDSP1", 8);
    put32(out + 8, pos);
    memcpy(out + 16, "1.2.3", 6);
    *app_crc = h.app_crc;
    return pos;
}

static int check(uint8_t *img, uint32_t len, uint32_t app_len, uint32_t app_crc)
{
    buf_t m = { img, len };
    return dap_image_check_blocks(mem_read, &m, len, app_len, app_crc);
}

static void test_blocks(void)
{
    enum { APP = 300 };
    static uint16_t flat[APP];
    static uint8_t img[4096];
    uint32_t crc, len;
    for (int i = 0; i < APP; i++) flat[i] = (uint16_t)(i * 7 + 1);
    // 128 워드 단위, 마지막 44 워드
    blk_t ok[] = { { 0x3E8040, 128 }, { 0x3E80C0, 128 }, { 0x3E8140, 44 } };
    len = build(img, flat, APP, ok, 3, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_OK);
    CHECK(check(img, len, APP, crc ^ 1) == DAP_IMG_E_CRC);
    // 빈 블록(전부 0xFFFF) 생략 — 빈 구간은 0xFFFF 로 반영되어야 함
    for (int i = 128; i < 256; i++) flat[i] = 0xFFFF;
    blk_t gap[] = { { 0x3E8040, 128 }, { 0x3E8140, 44 } };
    len = build(img, flat, APP, gap, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_OK);
    // 끝쪽 빈 구간 (app_len 은 그대로, 마지막 블록 생략 — 코드 끝 워드가 0xFFFF 인 경우)
    for (int i = 256; i < APP; i++) flat[i] = 0xFFFF;
    blk_t tail[] = { { 0x3E8040, 128 } };
    len = build(img, flat, APP, tail, 1, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_OK);
    flat[APP - 1] = 0x1234;             // 마지막 워드에 데이터가 있는데 블록이 없음 → CRC 불일치
    len = build(img, flat, APP, tail, 1, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_CRC);
    for (int i = 0; i < APP; i++) flat[i] = (uint16_t)(i * 7 + 1);
    // 블록 크기 256 워드 (다른 패커) → 거부
    blk_t big[] = { { 0x3E8040, 256 }, { 0x3E8140, 44 } };
    len = build(img, flat, APP, big, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_BLOCK);
    // nw = 0
    blk_t zero[] = { { 0x3E8040, 128 }, { 0x3E80C0, 0 } };
    len = build(img, flat, APP, zero, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_BLOCK);
    // 앱 영역 밖 (헤더 자리 / 영역 끝 넘음)
    blk_t lo[] = { { 0x3E8000, 64 }, { 0x3E8040, 128 } };
    len = build(img, flat, APP, lo, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_ADDR);
    blk_t hi[] = { { 0x3E8040, 128 }, { DSPIMG_APP_END - 10, 20 } };
    len = build(img, flat, APP, hi, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_ADDR);
    // 순서 어긋남·겹침
    blk_t ovl[] = { { 0x3E8040, 128 }, { 0x3E8080, 128 } };
    len = build(img, flat, APP, ovl, 2, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_ADDR);
    // 파일 끝 불일치: 꼬리 쓰레기 / 잘림
    len = build(img, flat, APP, ok, 3, &crc);
    img[len] = 0; img[len + 1] = 0; img[len + 2] = 0;
    CHECK(check(img, len + 3, APP, crc) == DAP_IMG_E_BLOCK);
    CHECK(check(img, len - 2, APP, crc) == DAP_IMG_E_BLOCK);
    // 블록 데이터 한 워드 변조 → CRC 불일치
    img[32 + 128 + 6 + 10] ^= 0x5A;
    CHECK(check(img, len, APP, crc) == DAP_IMG_E_CRC);
    // app_len 밖 워드는 CRC 에서 제외 (dsp_pack unpack 규칙)
    blk_t over[] = { { 0x3E8040, 128 }, { 0x3E80C0, 128 }, { 0x3E8140, 60 } };
    len = build(img, flat, APP, over, 3, &crc);
    CHECK(check(img, len, APP, crc) == DAP_IMG_OK);
}

// dsp_pack.py 로 만든 파일 (인자) 검사: argv[i] = 파일, 기대 결과는 파일명이 bad 로 시작하면 실패
static void test_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL open %s\n", path); s_fail++; return; }
    static uint8_t b[400000];
    uint32_t n = (uint32_t)fread(b, 1, sizeof(b), f);
    fclose(f);
    uint16_t hw[DSPIMG_HDR_WORDS];
    for (int i = 0; i < DSPIMG_HDR_WORDS; i++) hw[i] = b[32 + i * 2] | (b[33 + i * 2] << 8);
    dspimg_hdr_t h;
    CHECK(dspimg_hdr_parse(hw, &h) == DSPIMG_OK);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int rc = check(b, n, h.app_len, h.app_crc);
    bool want_ok = strncmp(base, "bad", 3) != 0;
    if ((rc == DAP_IMG_OK) != want_ok) { printf("FAIL %s: rc %d\n", base, rc); s_fail++; }
    else printf("ok   %s (rc %d)\n", base, rc);
}

static void test_ver(void)
{
    CHECK(dap_ver_valid("1.2.3"));
    CHECK(dap_ver_valid("2026.10."));
    CHECK(dap_ver_valid("a_B-9"));
    CHECK(!dap_ver_valid(""));
    CHECK(!dap_ver_valid(NULL));
    CHECK(!dap_ver_valid("2026.10.11"));      // 10자
    CHECK(!dap_ver_valid("1.2 3"));
    CHECK(!dap_ver_valid("1\"2"));
    CHECK(!dap_ver_valid("v1/2"));
}

static void test_escape(void)
{
    char o[32];
    CHECK(dap_json_escape(o, sizeof(o), "1.2.3") == 5 && strcmp(o, "1.2.3") == 0);
    dap_json_escape(o, sizeof(o), "a\"b\\c");
    CHECK(strcmp(o, "a\\\"b\\\\c") == 0);
    dap_json_escape(o, sizeof(o), "x\x01y");
    CHECK(strcmp(o, "x\\u0001y") == 0);
    // 자를 때 이스케이프 중간에서 끊지 않음
    char s[6];
    dap_json_escape(s, sizeof(s), "abcd\"");
    CHECK(strcmp(s, "abcd") == 0);
    dap_json_escape(s, sizeof(s), NULL);
    CHECK(s[0] == 0);
}

static void test_json(void)
{
    char u[16];
    const char *j = "{\"job\":12,\"url\":\"http://1.2.3.4/a\",\"recover\":1,\"apply\":0}";
    size_t n = strlen(j);
    CHECK(dap_json_u32(j, n, "job") == 12);
    CHECK(dap_json_u32(j, n, "recover") == 1);
    CHECK(dap_json_u32(j, n, "missing") == 0);
    CHECK(dap_json_str(j, n, "url", u, sizeof(u)) == DAP_JSON_TOO_LONG && u[0] == 0);
    char big[32];
    CHECK(dap_json_str(j, n, "url", big, sizeof(big)) == DAP_JSON_OK && strcmp(big, "http://1.2.3.4/a") == 0);
    // 정확히 cap-1 자는 통과, cap 자는 너무 김
    char e[17];
    const char *j2 = "{\"url\":\"0123456789abcdef\"}";
    CHECK(dap_json_str(j2, strlen(j2), "url", e, sizeof(e)) == DAP_JSON_OK);
    CHECK(dap_json_str(j2, strlen(j2), "url", e, 16) == DAP_JSON_TOO_LONG);
    CHECK(dap_json_str(j, n, "job", big, sizeof(big)) == DAP_JSON_MISSING);          // 숫자
    const char *j3 = "{\"url\":\"abc";                                                  // 닫는 따옴표 없음
    CHECK(dap_json_str(j3, strlen(j3), "url", big, sizeof(big)) == DAP_JSON_MISSING);
    CHECK(dap_json_str(j3, strlen(j3), "url", e, 3) == DAP_JSON_MISSING);
}

static void test_url(void)
{
    const uint8_t ip[4] = { 192, 168, 0, 10 };
    CHECK(dap_url_host_is("http://192.168.0.10:8080/fw/x/a.dspfw", ip));
    CHECK(dap_url_host_is("http://192.168.0.10/fw", ip));
    CHECK(dap_url_host_is("HTTP://192.168.0.10", ip));
    CHECK(dap_url_host_is("https://192.168.0.10:443?x", ip));
    CHECK(!dap_url_host_is("http://192.168.0.11:8080/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.100/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.1/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.10.evil.com/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.10@evil.com/fw", ip));
    CHECK(!dap_url_host_is("http://user@192.168.0.10/fw", ip));
    CHECK(!dap_url_host_is("http://gateway.local:8080/fw", ip));
    CHECK(!dap_url_host_is("ftp://192.168.0.10/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.000.0010/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.010/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.10:/fw", ip));
    CHECK(!dap_url_host_is("http://192.168.0.10:80x/fw", ip));
    CHECK(!dap_url_host_is(NULL, ip));
}

static void test_reply(void)
{
    // V 응답 버전 칸 8자 (공백 채움 제거)
    uint8_t f[48];
    const char *body = "R11" "07" "V" "1.2.3   " "B1      " "20261011";
    size_t n = 0;
    f[n++] = DAP_STX;
    memcpy(f + n, body, strlen(body)); n += strlen(body);
    f[n++] = ' ';
    f[n] = (uint8_t)dap_bcc(f + 1, n - 1); n++;
    f[n++] = DAP_ETX;
    dap_reply_t r;
    CHECK(dap_parse_reply(f, n, 7, '1', &r) == 0 && r.cmd == 'V' && strcmp(r.app_ver, "1.2.3") == 0);
    CHECK(dap_parse_reply(f, n, 8, '1', &r) == -2);
}

int main(int argc, char **argv)
{
    test_ver();
    test_escape();
    test_json();
    test_url();
    test_blocks();
    test_reply();
    for (int i = 1; i < argc; i++) test_file(argv[i]);
    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
