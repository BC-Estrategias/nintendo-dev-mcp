/* Pairing by number comparison (spec §4.8): X25519 key agreement, a commitment so nobody can grind the number, and the
 * derivations both sides compute. No I/O and no state: the agent (console) and the clients (page, CLI) call these. */
#ifndef NDP_PAIR2_H
#define NDP_PAIR2_H

#include "ndp/ndp_defs.h"

#define NDP_X25519_BYTES 32
#define NDP_PAIR2_NONCE 16

/* RFC 7748 X25519: out = scalar * point (clamping is done inside). Returns 0, or -1 when the result is all zero
 * (a low-order input point: the exchange MUST be abandoned). */
int ndp_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* pub = scalar * 9 (the base point). */
void ndp_x25519_public(uint8_t pub[32], const uint8_t scalar[32]);

/* commit = SHA-256("NDP-PAIR2-COMMIT" || pubB || nonceB): what the browser sends BEFORE it has seen the console's key. */
void ndp_pair2_commit(const uint8_t pub_b[32], const uint8_t nonce_b[NDP_PAIR2_NONCE], uint8_t out[32]);

typedef struct {
  uint8_t psk[32];   /* the pairing key both sides store */
  uint8_t key_id[4];
  uint32_t sas;      /* the number the person compares, 0..999999 */
} ndp_pair2_keys;

/* th = SHA-256("NDP-PAIR2-TH" || pubB || pubC || nonceB || nonceC || len(label) || label); k = HMAC(shared, th);
 * sas = be32(HMAC(k, "sas")[0..4]) mod 10^6; psk = HMAC(k, "psk"); key_id = SHA-256(psk)[0..4]. */
void ndp_pair2_derive(const uint8_t shared[32], const uint8_t pub_b[32], const uint8_t pub_c[32],
                      const uint8_t nonce_b[NDP_PAIR2_NONCE], const uint8_t nonce_c[NDP_PAIR2_NONCE],
                      const uint8_t *label, size_t label_len, ndp_pair2_keys *out);
/* The console's proof that it derived the same key: HMAC(psk, "NDP-PAIR2-OK" || pubB || pubC). */
void ndp_pair2_ok_proof(const uint8_t psk[32], const uint8_t pub_b[32], const uint8_t pub_c[32], uint8_t out[32]);

#endif
