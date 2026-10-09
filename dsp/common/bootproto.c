#include "bootproto.h"

enum { S_SOF0, S_SOF1, S_LEN0, S_LEN1, S_BODY, S_CRC0, S_CRC1 };

#define RD_CHUNK 64

uint16_t bp_crc16(const uint16_t *b, uint16_t n)
{
    return dspimg_crc16_bytes(b, n);
}

uint16_t bp_build(uint16_t *out, uint16_t cmd, uint16_t seq, const uint16_t *payload, uint16_t n)
{
    uint16_t i, crc;
    out[0] = BP_SOF0;
    out[1] = BP_SOF1;
    out[2] = (uint16_t)((n + 2) & 0xFFU);
    out[3] = (uint16_t)((n + 2) >> 8);
    out[4] = cmd & 0xFFU;
    out[5] = seq & 0xFFU;
    for (i = 0; i < n; i++) {
        out[6 + i] = payload[i] & 0xFFU;
    }
    crc = bp_crc16(&out[4], (uint16_t)(n + 2));
    out[6 + n] = crc & 0xFFU;
    out[7 + n] = (uint16_t)(crc >> 8);
    return (uint16_t)(8 + n);
}

void bp_init(bp_ctx_t *c, const bp_ops_t *ops)
{
    c->ops = ops;
    c->state = S_SOF0;
    c->len = c->idx = 0;
    c->tx_len = 0;
    c->reset_pending = 0;
    c->reset_mode = 0;
    c->last_seq = 0xFFFF;
    c->last_cmd = 0;
    c->last_tx_len = 0;
}

static uint32_t get_u32(const uint16_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void put_u32(uint16_t *b, uint32_t v)
{
    b[0] = v & 0xFFU;
    b[1] = (v >> 8) & 0xFFU;
    b[2] = (v >> 16) & 0xFFU;
    b[3] = (v >> 24) & 0xFFU;
}

int bp_app_check(const bp_ops_t *ops, dspimg_hdr_t *hdr, uint32_t *crc_out)
{
    uint16_t buf[RD_CHUNK];
    dspimg_hdr_t h;
    uint32_t crc, addr, left;
    int rc;

    ops->read(DSPIMG_APP_HDR_ADDR, buf, DSPIMG_HDR_WORDS);
    rc = dspimg_hdr_parse(buf, &h);
    if (hdr) {
        *hdr = h;
    }
    if (crc_out) {
        *crc_out = 0;
    }
    if (rc != DSPIMG_OK) {
        return rc;
    }
    crc  = dspimg_crc32_init();
    addr = DSPIMG_APP_CODE_ADDR;
    left = h.app_len;
    while (left > 0) {
        uint16_t n = left < RD_CHUNK ? (uint16_t)left : RD_CHUNK;
        ops->read(addr, buf, n);
        crc = dspimg_crc32_words(crc, buf, n);
        addr += n;
        left -= n;
    }
    crc = dspimg_crc32_final(crc);
    if (crc_out) {
        *crc_out = crc;
    }
    return crc == h.app_crc ? DSPIMG_OK : DSPIMG_E_APPCRC;
}

// 앱 영역 안의 유효한 [addr, addr+n) 인지
static int in_app(uint32_t addr, uint32_t n)
{
    return addr >= DSPIMG_APP_HDR_ADDR && n > 0 && addr + n <= DSPIMG_APP_END && addr + n > addr;
}

// 요청 처리 → 응답 payload 를 resp 에, 길이 반환
static uint16_t handle(bp_ctx_t *c, uint16_t cmd, const uint16_t *pl, uint16_t n, uint16_t *resp)
{
    const bp_ops_t *ops = c->ops;
    uint16_t i;

    switch (cmd) {
    case BP_CMD_HELLO: {
        dspimg_hdr_t h;
        int rc = bp_app_check(ops, &h, 0);
        resp[0] = BP_ST_OK;
        resp[1] = BP_PROTO_VER;
        resp[2] = BP_BLOCK_WORDS & 0xFFU;
        resp[3] = BP_BLOCK_WORDS >> 8;
        resp[4] = rc == DSPIMG_OK ? 1 : 0;
        for (i = 0; i < BP_BOOTVER_LEN; i++) {
            resp[5 + i] = ops->boot_ver[i] & 0xFFU;
        }
        for (i = 0; i < DSPIMG_VER_WORDS; i++) {
            resp[21 + i] = rc == DSPIMG_E_MAGIC || rc == DSPIMG_E_HDRCRC ? 0 : h.ver[i];
        }
        put_u32(&resp[53], rc == DSPIMG_OK ? h.app_crc : 0);
        put_u32(&resp[57], rc == DSPIMG_OK ? h.app_len : 0);
        return BP_HELLO_RESP_LEN;
    }
    case BP_CMD_ERASE:
        if (n != 1 || pl[0] == 0 || (pl[0] & (uint16_t)~BP_ERASE_APP) != 0) {
            resp[0] = BP_ST_BAD_PAYLOAD;
            return 1;
        }
        resp[0] = ops->erase(pl[0]);
        return 1;
    case BP_CMD_WRITE: {
        uint32_t addr;
        uint16_t nw;
        if (n < 6) {
            resp[0] = BP_ST_BAD_PAYLOAD;
            return 1;
        }
        addr = get_u32(pl);
        nw   = (uint16_t)(pl[4] | (pl[5] << 8));
        put_u32(&resp[1], addr);
        if (nw == 0 || nw > BP_BLOCK_WORDS || n != 6 + nw * 2) {
            resp[0] = BP_ST_BAD_LEN;
            return 5;
        }
        if (!in_app(addr, nw)) {
            resp[0] = BP_ST_BAD_ADDR;
            return 5;
        }
        {
            // 데이터 바이트쌍 → 워드 (rx 버퍼의 payload 영역을 제자리 변환하지 않고 별도 버퍼 사용)
            static uint16_t words[BP_BLOCK_WORDS];
            for (i = 0; i < nw; i++) {
                words[i] = (uint16_t)(pl[6 + i * 2] | (pl[7 + i * 2] << 8));
            }
            resp[0] = ops->program(addr, words, nw);
        }
        return 5;
    }
    case BP_CMD_VERIFY: {
        dspimg_hdr_t h;
        uint32_t crc;
        int rc = bp_app_check(ops, &h, &crc);
        resp[0] = BP_ST_OK;
        resp[1] = rc == DSPIMG_OK ? 1 : 0;
        put_u32(&resp[2], crc);
        put_u32(&resp[6], rc == DSPIMG_OK || rc == DSPIMG_E_APPCRC ? h.app_len : 0);
        resp[10] = (uint16_t)rc;
        return 11;
    }
    case BP_CMD_RESET:
        resp[0] = BP_ST_OK;
        c->reset_pending = 1;
        c->reset_mode = n >= 1 ? pl[0] : 0;
        return 1;
    case BP_CMD_READ: {
        uint32_t addr;
        uint16_t nw;
        if (n != 6) {
            resp[0] = BP_ST_BAD_PAYLOAD;
            return 1;
        }
        addr = get_u32(pl);
        nw   = (uint16_t)(pl[4] | (pl[5] << 8));
        if (nw == 0 || nw > BP_BLOCK_WORDS) {
            resp[0] = BP_ST_BAD_LEN;
            return 1;
        }
        {
            static uint16_t words[BP_BLOCK_WORDS];
            ops->read(addr, words, nw);
            resp[0] = BP_ST_OK;
            for (i = 0; i < nw; i++) {
                resp[1 + i * 2] = words[i] & 0xFFU;
                resp[2 + i * 2] = words[i] >> 8;
            }
        }
        return (uint16_t)(1 + nw * 2);
    }
    default:
        resp[0] = BP_ST_BAD_CMD;
        return 1;
    }
}

int bp_rx_byte(bp_ctx_t *c, uint16_t b)
{
    b &= 0xFFU;
    switch (c->state) {
    case S_SOF0:
        if (b == BP_SOF0) c->state = S_SOF1;
        return 0;
    case S_SOF1:
        c->state = b == BP_SOF1 ? S_LEN0 : (b == BP_SOF0 ? S_SOF1 : S_SOF0);
        return 0;
    case S_LEN0:
        c->len = b;
        c->state = S_LEN1;
        return 0;
    case S_LEN1:
        c->len |= (uint16_t)(b << 8);
        if (c->len < 2 || c->len > 2 + BP_MAX_PAYLOAD) {
            c->state = S_SOF0;      // 길이 이상 → 재동기
            return 0;
        }
        c->idx = 0;
        c->state = S_BODY;
        return 0;
    case S_BODY:
        c->rx[c->idx++] = b;
        if (c->idx == c->len) c->state = S_CRC0;
        return 0;
    case S_CRC0:
        c->rx[c->idx++] = b;
        c->state = S_CRC1;
        return 0;
    case S_CRC1: {
        uint16_t crc, cmd, seq;
        c->rx[c->idx++] = b;
        c->state = S_SOF0;
        crc = (uint16_t)(c->rx[c->len] | (c->rx[c->len + 1] << 8));
        if (crc != bp_crc16(c->rx, c->len)) {
            return 0;               // CRC 오류: 무응답 → 브리지가 재전송
        }
        cmd = c->rx[0];
        seq = c->rx[1];
        if (seq == c->last_seq && cmd == c->last_cmd && c->last_tx_len > 0) {
            // 응답 유실로 인한 재전송 → 같은 응답 반복 (플래시를 두 번 쓰지 않음)
            uint16_t i;
            for (i = 0; i < c->last_tx_len; i++) c->tx[i] = c->last_tx[i];
            c->tx_len = c->last_tx_len;
            return 1;
        }
        {
            static uint16_t resp[BP_MAX_PAYLOAD];
            uint16_t rn = handle(c, cmd, &c->rx[2], (uint16_t)(c->len - 2), resp);
            uint16_t i;
            c->tx_len = bp_build(c->tx, cmd | BP_RESP, seq, resp, rn);
            c->last_seq = seq;
            c->last_cmd = cmd;
            for (i = 0; i < c->tx_len; i++) c->last_tx[i] = c->tx[i];
            c->last_tx_len = c->tx_len;
        }
        return 1;
    }
    default:
        c->state = S_SOF0;
        return 0;
    }
}
