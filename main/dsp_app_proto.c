#include <string.h>
#include <ctype.h>
#include "dsp_app_proto.h"
#include "bridge_util.h"
#include "dsp_image.h"
#include "bootproto.h"

char dap_bcc(const uint8_t *payload, size_t len)
{
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += payload[i];
    }
    unsigned n = sum & 0x0F;
    return (char)(n < 10 ? '0' + n : 'A' + n - 10);
}

static bool hex1(char c)
{
    return (c >= '1' && c <= '9') || (c >= 'A' && c <= 'F');
}

// STX..ETX 프레임 검사: 길이·헤더 문자·BCC. 성공 시 payload 시작(STX 다음)과 길이(BCC 포함)
static bool unwrap(const uint8_t *frame, size_t len, const uint8_t **pl, size_t *pn)
{
    if (len < 9 || frame[0] != DAP_STX || frame[len - 1] != DAP_ETX) {
        return false;
    }
    const uint8_t *p = frame + 1;
    size_t n = len - 2;
    if (!hex1((char)p[1]) || !hex1((char)p[2]) || !isdigit(p[3]) || !isdigit(p[4]) || p[n - 2] != ' ') {
        return false;
    }
    if ((char)p[n - 1] != dap_bcc(p, n - 1)) {
        return false;
    }
    *pl = p;
    *pn = n;
    return true;
}

size_t dap_build_query(uint8_t *out, size_t cap, char pan, char msg, uint8_t agv, char cmd)
{
    if (cap < 12 || agv > 99) {
        return 0;
    }
    size_t n = 0;
    out[n++] = DAP_STX;
    out[n++] = 'F';
    out[n++] = (uint8_t)pan;
    out[n++] = (uint8_t)msg;
    out[n++] = (uint8_t)('0' + agv / 10);
    out[n++] = (uint8_t)('0' + agv % 10);
    out[n++] = (uint8_t)cmd;
    out[n++] = '0';
    out[n++] = '0';
    out[n++] = '0';
    out[n++] = ' ';
    out[n] = (uint8_t)dap_bcc(out + 1, n - 1);
    n++;
    out[n++] = DAP_ETX;
    return n;
}

static void copy_trim(char *dst, const uint8_t *src, size_t n)
{
    while (n > 0 && src[n - 1] == ' ') {
        n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int dap_parse_reply(const uint8_t *frame, size_t len, uint8_t agv, char expect_msg, dap_reply_t *r)
{
    const uint8_t *p;
    size_t n;
    if (len < 2 || frame[0] != DAP_STX || frame[1] != 'R') {
        return -1;
    }
    if (!unwrap(frame, len, &p, &n)) {
        return -2;
    }
    if ((p[3] - '0') * 10 + (p[4] - '0') != agv || (char)p[2] != expect_msg) {
        return -2;
    }
    memset(r, 0, sizeof(*r));
    r->cmd = (char)p[5];
    size_t body = n - 2 - 5;            // 본문 길이 (헤더 5, 꼬리 2 제외)
    if (r->cmd == 'V' && body == 25) {
        copy_trim(r->app_ver,  &p[6],  8);
        copy_trim(r->boot_ver, &p[14], 8);
        copy_trim(r->build,    &p[22], 8);
        return 0;
    }
    if (r->cmd == 'U' && body == 5) {
        r->ok = p[6] == 'O' && p[7] == 'K';
        r->reason = (uint8_t)((p[8] - '0') * 10 + (p[9] - '0'));
        return 0;
    }
    return -2;
}

char dap_learn_pan(const uint8_t *frame, size_t len, uint8_t agv)
{
    const uint8_t *p;
    size_t n;
    if (len < 2 || frame[0] != DAP_STX || (frame[1] != 'E' && frame[1] != 'A')) {
        return 0;
    }
    if (!unwrap(frame, len, &p, &n) || (p[3] - '0') * 10 + (p[4] - '0') != agv) {
        return 0;
    }
    return (char)p[1];
}

// ───────── 메인보드 OTA 검사 도우미 ─────────

bool dap_ver_valid(const char *v)
{
    size_t n = 0;
    if (!v) {
        return false;
    }
    for (; v[n]; n++) {
        char c = v[n];
        if (n >= DAP_VER_MAX || !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                  c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return n > 0;
}

size_t dap_json_escape(char *dst, size_t cap, const char *src)
{
    static const char HEX[] = "0123456789abcdef";
    size_t n = 0;
    if (cap == 0) {
        return 0;
    }
    for (; src && *src; src++) {
        unsigned char c = (unsigned char)*src;
        char e[7];
        size_t k;
        if (c == '"' || c == '\\') {
            e[0] = '\\'; e[1] = (char)c; k = 2;
        } else if (c < 0x20 || c == 0x7F) {
            e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0'; e[4] = HEX[c >> 4]; e[5] = HEX[c & 15]; k = 6;
        } else {
            e[0] = (char)c; k = 1;
        }
        if (n + k + 1 > cap) {
            break;
        }
        memcpy(dst + n, e, k);
        n += k;
    }
    dst[n] = '\0';
    return n;
}

static const char *json_find(const char *json, size_t len, const char *key)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl + 2 < len; i++) {
        if (json[i] == '"' && memcmp(json + i + 1, key, kl) == 0 && json[i + 1 + kl] == '"') {
            const char *p = json + i + 2 + kl;
            const char *end = json + len;
            while (p < end && (*p == ' ' || *p == ':')) {
                if (*p == ':') {
                    p++;
                    while (p < end && *p == ' ') p++;
                    return p;
                }
                p++;
            }
            return NULL;
        }
    }
    return NULL;
}

int dap_json_str(const char *json, size_t len, const char *key, char *dst, size_t cap)
{
    const char *p = json_find(json, len, key);
    const char *end = json + len;
    size_t n = 0;
    if (cap == 0) {
        return DAP_JSON_MISSING;
    }
    dst[0] = '\0';
    if (!p || p >= end || *p != '"') {
        return DAP_JSON_MISSING;
    }
    for (p++; p < end && *p != '"'; p++) {
        if (n + 1 >= cap) {
            dst[0] = '\0';
            // 닫는 따옴표가 있어야 '너무 김', 없으면 형식 오류
            while (p < end && *p != '"') p++;
            return p < end ? DAP_JSON_TOO_LONG : DAP_JSON_MISSING;
        }
        dst[n++] = *p;
    }
    dst[n] = '\0';
    if (p >= end) {
        dst[0] = '\0';
        return DAP_JSON_MISSING;
    }
    return DAP_JSON_OK;
}

uint32_t dap_json_u32(const char *json, size_t len, const char *key)
{
    const char *p = json_find(json, len, key);
    const char *end = json + len;
    if (!p || p >= end || *p < '0' || *p > '9') {
        return 0;
    }
    uint32_t v = 0;
    for (; p < end && *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (uint32_t)(*p - '0');
    }
    return v;
}

// 브리지 자체 OTA(ota.c)와 같은 검사기를 쓴다 — 호스트는 10진 IPv4 만, 사용자 정보·호스트명·IPv6 거부
bool dap_url_host_is(const char *url, const uint8_t ip[4])
{
    uint32_t be;
    memcpy(&be, ip, 4);
    return bu_url_host_is(url, be);
}

#define IMG_FILE_HDR_LEN    32
#define IMG_BLOCKS_OFF      (IMG_FILE_HDR_LEN + DSPIMG_HDR_WORDS * 2)

// 0xFFFF 워드 n 개를 CRC 에 반영 (지워진 플래시)
static uint32_t crc_erased(uint32_t crc, uint32_t n)
{
    static const uint16_t FF[64] = {
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
        0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    };
    uint32_t bytes = n * 2;
    while (bytes) {
        uint32_t k = bytes < 64 ? bytes : 64;
        crc = dspimg_crc32_bytes(crc, FF, k);
        bytes -= k;
    }
    return crc;
}

int dap_image_check_blocks(dap_read_fn rd, void *ctx, uint32_t file_len, uint32_t app_len, uint32_t app_crc)
{
    uint32_t pos = IMG_BLOCKS_OFF;
    uint32_t next = DSPIMG_APP_CODE_ADDR;         // CRC 에 반영한 다음 워드 주소 (= 직전 블록 끝 이상)
    const uint32_t crc_end = DSPIMG_APP_CODE_ADDR + app_len;
    uint32_t crc = dspimg_crc32_init();
    uint8_t buf[BP_BLOCK_WORDS * 2];
    uint16_t w[BP_BLOCK_WORDS * 2];

    if (file_len < IMG_BLOCKS_OFF || app_len == 0 || app_len > DSPIMG_APP_MAX_WORDS) {
        return DAP_IMG_E_BLOCK;
    }
    while (pos < file_len) {
        uint8_t bh[6];
        if (file_len - pos < 6) {
            return DAP_IMG_E_BLOCK;
        }
        if (!rd(ctx, pos, bh, 6)) {
            return DAP_IMG_E_READ;
        }
        uint32_t addr = bh[0] | ((uint32_t)bh[1] << 8) | ((uint32_t)bh[2] << 16) | ((uint32_t)bh[3] << 24);
        uint32_t nw = bh[4] | ((uint32_t)bh[5] << 8);
        pos += 6;
        if (nw == 0 || nw > BP_BLOCK_WORDS || file_len - pos < nw * 2) {
            return DAP_IMG_E_BLOCK;
        }
        if (addr < DSPIMG_APP_CODE_ADDR || addr > DSPIMG_APP_END || nw > DSPIMG_APP_END - addr || addr < next) {
            return DAP_IMG_E_ADDR;
        }
        if (!rd(ctx, pos, buf, nw * 2)) {
            return DAP_IMG_E_READ;
        }
        pos += nw * 2;
        // 빈 구간 0xFFFF → 블록 데이터 (app_len 안쪽만)
        if (next < crc_end) {
            uint32_t gap_end = addr < crc_end ? addr : crc_end;
            crc = crc_erased(crc, gap_end - next);
        }
        if (addr < crc_end) {
            uint32_t use = crc_end - addr < nw ? crc_end - addr : nw;
            for (uint32_t i = 0; i < use * 2; i++) w[i] = buf[i];   // 파일은 워드마다 하위 바이트 먼저 = CRC 순서
            crc = dspimg_crc32_bytes(crc, w, use * 2);
        }
        next = addr + nw;
    }
    if (next < crc_end) {
        crc = crc_erased(crc, crc_end - next);
    }
    return dspimg_crc32_final(crc) == app_crc ? DAP_IMG_OK : DAP_IMG_E_CRC;
}
