#include "ndp/ndp_sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void transform(ndp_sha256_ctx *c, const uint8_t block[64]) {
  uint32_t w[64];
  uint32_t a, b, cc, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void ndp_sha256_init(ndp_sha256_ctx *c) {
  c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
  c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c; c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
  c->total = 0;
  c->buflen = 0;
}

void ndp_sha256_update(ndp_sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->total += len;
  if (c->buflen) {
    size_t take = 64 - c->buflen;
    if (take > len) take = len;
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take; p += take; len -= take;
    if (c->buflen == 64) { transform(c, c->buf); c->buflen = 0; }
  }
  while (len >= 64) { transform(c, p); p += 64; len -= 64; }
  if (len) { memcpy(c->buf, p, len); c->buflen = len; }
}

void ndp_sha256_final(ndp_sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->total * 8;
  uint8_t pad[72];
  size_t padlen = (c->buflen < 56) ? (56 - c->buflen) : (120 - c->buflen);
  int i;
  memset(pad, 0, sizeof pad);
  pad[0] = 0x80;
  for (i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
  ndp_sha256_update(c, pad, padlen + 8);
  for (i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)c->h[i];
  }
  memset(c, 0, sizeof *c);
}

void ndp_sha256(const void *data, size_t len, uint8_t out[32]) {
  ndp_sha256_ctx c;
  ndp_sha256_init(&c);
  ndp_sha256_update(&c, data, len);
  ndp_sha256_final(&c, out);
}

void ndp_hmac_init(ndp_hmac_ctx *c, const void *key, size_t key_len) {
  uint8_t k[64], pad[64];
  size_t i;
  memset(k, 0, sizeof k);
  if (key_len > 64) ndp_sha256(key, key_len, k);
  else if (key_len) memcpy(k, key, key_len);
  for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  ndp_sha256_init(&c->inner);
  ndp_sha256_update(&c->inner, pad, 64);
  for (i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  ndp_sha256_init(&c->outer);
  ndp_sha256_update(&c->outer, pad, 64);
  memset(k, 0, sizeof k);
  memset(pad, 0, sizeof pad);
}

void ndp_hmac_update(ndp_hmac_ctx *c, const void *data, size_t len) {
  ndp_sha256_update(&c->inner, data, len);
}

void ndp_hmac_final(ndp_hmac_ctx *c, uint8_t out[32]) {
  uint8_t ih[32];
  ndp_sha256_final(&c->inner, ih);
  ndp_sha256_update(&c->outer, ih, 32);
  ndp_sha256_final(&c->outer, out);
  memset(ih, 0, sizeof ih);
}

void ndp_hmac_sha256(const void *key, size_t key_len, const void *msg, size_t msg_len, uint8_t out[32]) {
  ndp_hmac_ctx c;
  ndp_hmac_init(&c, key, key_len);
  ndp_hmac_update(&c, msg, msg_len);
  ndp_hmac_final(&c, out);
}

int ndp_ct_equal(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  uint8_t diff = 0;
  size_t i;
  for (i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
  return diff == 0;
}
