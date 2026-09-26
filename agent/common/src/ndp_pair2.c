#include <string.h>

#include "ndp/ndp_auth.h"
#include "ndp/ndp_pair2.h"
#include "ndp/ndp_sha256.h"

void ndp_pair2_commit(const uint8_t pub_b[32], const uint8_t nonce_b[NDP_PAIR2_NONCE], uint8_t out[32]) {
  ndp_sha256_ctx c;
  ndp_sha256_init(&c);
  ndp_sha256_update(&c, "NDP-PAIR2-COMMIT", 16);
  ndp_sha256_update(&c, pub_b, 32);
  ndp_sha256_update(&c, nonce_b, NDP_PAIR2_NONCE);
  ndp_sha256_final(&c, out);
}

void ndp_pair2_derive(const uint8_t shared[32], const uint8_t pub_b[32], const uint8_t pub_c[32],
                      const uint8_t nonce_b[NDP_PAIR2_NONCE], const uint8_t nonce_c[NDP_PAIR2_NONCE],
                      const uint8_t *label, size_t label_len, ndp_pair2_keys *out) {
  ndp_sha256_ctx c;
  uint8_t th[32], k[32], m[32], ll = (uint8_t)label_len;
  ndp_sha256_init(&c);
  ndp_sha256_update(&c, "NDP-PAIR2-TH", 12);
  ndp_sha256_update(&c, pub_b, 32);
  ndp_sha256_update(&c, pub_c, 32);
  ndp_sha256_update(&c, nonce_b, NDP_PAIR2_NONCE);
  ndp_sha256_update(&c, nonce_c, NDP_PAIR2_NONCE);
  ndp_sha256_update(&c, &ll, 1);
  ndp_sha256_update(&c, label, label_len);
  ndp_sha256_final(&c, th);
  ndp_hmac_sha256(shared, 32, th, 32, k);
  ndp_hmac_sha256(k, 32, "sas", 3, m);
  out->sas = (((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3]) % 1000000u;
  ndp_hmac_sha256(k, 32, "psk", 3, out->psk);
  ndp_key_id(out->psk, out->key_id);
  memset(th, 0, sizeof th);
  memset(k, 0, sizeof k);
}

void ndp_pair2_ok_proof(const uint8_t psk[32], const uint8_t pub_b[32], const uint8_t pub_c[32], uint8_t out[32]) {
  uint8_t msg[12 + 64];
  memcpy(msg, "NDP-PAIR2-OK", 12);
  memcpy(msg + 12, pub_b, 32);
  memcpy(msg + 44, pub_c, 32);
  ndp_hmac_sha256(psk, 32, msg, sizeof msg, out);
}
