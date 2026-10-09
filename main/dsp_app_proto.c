#include <string.h>
#include <ctype.h>
#include "dsp_app_proto.h"

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
