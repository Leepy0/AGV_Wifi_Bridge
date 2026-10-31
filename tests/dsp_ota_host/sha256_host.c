// 호스트 시험용 PSA 해시 스텁: 작은 SHA-256 (FIPS 180-4). 열린 연산 수를 세어 abort 누락을 잡는다
#include <string.h>
#include "psa/crypto.h"

int g_psa_open;     // setup 뒤 finish/abort 하지 않은 연산 수

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | p[i * 4 + 1] << 16 | p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

psa_status_t psa_crypto_init(void) { return PSA_SUCCESS; }

psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    static const uint32_t H0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    if (op->active || alg != PSA_ALG_SHA_256) return PSA_ERROR_BAD_STATE;
    memset(op, 0, sizeof(*op));
    memcpy(op->h, H0, sizeof(H0));
    op->active = 1;
    g_psa_open++;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t n)
{
    if (!op->active) return PSA_ERROR_BAD_STATE;
    op->len += n;
    while (n) {
        size_t k = 64 - op->n < n ? 64 - op->n : n;
        memcpy(op->buf + op->n, in, k);
        op->n += k; in += k; n -= k;
        if (op->n == 64) { block(op->h, op->buf); op->n = 0; }
    }
    return PSA_SUCCESS;
}

psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t cap, size_t *len)
{
    if (!op->active || cap < 32) return PSA_ERROR_BAD_STATE;
    uint64_t bits = op->len * 8;
    uint8_t pad = 0x80, z = 0;
    op->len = 0;
    psa_hash_update(op, &pad, 1);
    while (op->n != 56) psa_hash_update(op, &z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i * 8)); psa_hash_update(op, &b, 1); }
    for (int i = 0; i < 8; i++) { out[i * 4] = op->h[i] >> 24; out[i * 4 + 1] = op->h[i] >> 16; out[i * 4 + 2] = op->h[i] >> 8; out[i * 4 + 3] = op->h[i]; }
    *len = 32;
    op->active = 0;
    g_psa_open--;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    if (op->active) g_psa_open--;
    op->active = 0;
    return PSA_SUCCESS;
}
