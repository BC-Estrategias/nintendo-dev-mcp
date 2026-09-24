/* SHA-256 and HMAC-SHA-256 (FIPS 180-4 / RFC 2104). No dynamic allocation. */
#ifndef NDP_SHA256_H
#define NDP_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t h[8];
  uint8_t buf[64];
  uint64_t total; /* bytes hashed so far */
  size_t buflen;
} ndp_sha256_ctx;

void ndp_sha256_init(ndp_sha256_ctx *c);
void ndp_sha256_update(ndp_sha256_ctx *c, const void *data, size_t len);
void ndp_sha256_final(ndp_sha256_ctx *c, uint8_t out[32]);
void ndp_sha256(const void *data, size_t len, uint8_t out[32]);

typedef struct {
  ndp_sha256_ctx inner;
  ndp_sha256_ctx outer;
} ndp_hmac_ctx;

void ndp_hmac_init(ndp_hmac_ctx *c, const void *key, size_t key_len);
void ndp_hmac_update(ndp_hmac_ctx *c, const void *data, size_t len);
void ndp_hmac_final(ndp_hmac_ctx *c, uint8_t out[32]);
void ndp_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]);

/* Constant-time comparison; returns 1 when equal. */
int ndp_ct_equal(const void *a, const void *b, size_t n);

#endif
