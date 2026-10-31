#pragma once
// 호스트 시험용 최소 스텁 (PSA Crypto 해시 선언만 — 구현은 sha256_host.c 의 작은 SHA-256)
#include <stddef.h>
#include <stdint.h>
typedef int32_t psa_status_t;
typedef uint32_t psa_algorithm_t;
#define PSA_SUCCESS             ((psa_status_t)0)
#define PSA_ERROR_BAD_STATE     ((psa_status_t)-137)
#define PSA_ALG_SHA_256         ((psa_algorithm_t)0x02000009)
typedef struct { int active; uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } psa_hash_operation_t;
static inline psa_hash_operation_t psa_hash_operation_init(void) { psa_hash_operation_t o = {0}; return o; }
psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg);
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t n);
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *out, size_t cap, size_t *len);
psa_status_t psa_hash_abort(psa_hash_operation_t *op);
