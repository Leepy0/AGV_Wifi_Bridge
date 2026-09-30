#include <string.h>
#include "proto.h"

// 엔디언과 무관하게 리틀 엔디언으로 기록/해석
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline uint16_t get_u16(const uint8_t *p)  { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t br_build(uint8_t *out, size_t cap, uint8_t type, uint8_t agv_id,
                uint16_t seq, const uint8_t *payload, size_t len)
{
    if (len > BR_PAYLOAD_MAX || cap < BR_HDR_LEN + len) {
        return 0;
    }
    out[0] = BR_MAGIC0;
    out[1] = BR_MAGIC1;
    out[2] = BR_VER;
    out[3] = type;
    out[4] = agv_id;
    out[5] = 0;                 // flags (예약)
    put_u16(&out[6], seq);
    put_u16(&out[8], (uint16_t)len);
    if (len > 0) {
        memcpy(&out[BR_HDR_LEN], payload, len);
    }
    return BR_HDR_LEN + len;
}

bool br_parse(const uint8_t *in, size_t n, br_hdr_t *hdr, const uint8_t **payload)
{
    if (n < BR_HDR_LEN || in[0] != BR_MAGIC0 || in[1] != BR_MAGIC1 || in[2] != BR_VER) {
        return false;
    }
    uint16_t len = get_u16(&in[8]);
    if (len > BR_PAYLOAD_MAX || (size_t)len != n - BR_HDR_LEN) {
        return false;           // 길이 불일치 = 손상 패킷
    }
    hdr->type   = in[3];
    hdr->agv_id = in[4];
    hdr->flags  = in[5];
    hdr->seq    = get_u16(&in[6]);
    hdr->len    = len;
    *payload    = &in[BR_HDR_LEN];
    return true;
}
