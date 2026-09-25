/* Pairing and authentication primitives (spec §4): pairing code, key derivation, HMAC proofs,
 * session key, and the persistent key store. No I/O: the platform reads/writes the store bytes. */
#ifndef NDP_AUTH_H
#define NDP_AUTH_H

#include "ndp/ndp_defs.h"

#define NDP_CODE_BYTES 10
#define NDP_CODE_TEXT 20 /* "XXXX-XXXX-XXXX-XXXX" + NUL */
#define NDP_MAX_KEYS 4
#define NDP_LABEL_MAX 15
#define NDP_MAX_AUTH_FAILS 5

/* Crockford Base32 of the 80-bit code. */
void ndp_code_encode(const uint8_t code[NDP_CODE_BYTES], char out[NDP_CODE_TEXT]);
/* Case-insensitive; '-' and ' ' ignored; O -> 0, I/L -> 1. Returns 0, or -1 for an invalid code. */
int ndp_code_decode(const char *text, uint8_t code[NDP_CODE_BYTES]);

/* PSK = SHA-256("NDP-PSK-v1" || code); key_id = SHA-256(PSK)[0:4]. */
void ndp_derive_psk(const uint8_t code[NDP_CODE_BYTES], uint8_t psk[32]);
void ndp_key_id(const uint8_t psk[32], uint8_t id[4]);
/* HMAC(psk, label || cn || dn || extra); `label` is "pair" or "auth". */
void ndp_proof(const uint8_t psk[32], const char *label, const uint8_t cn[16], const uint8_t dn[16],
               const uint8_t *extra, size_t extra_len, uint8_t out[32]);
/* HMAC(psk, "session" || cn || dn). */
void ndp_session_key(const uint8_t psk[32], const uint8_t cn[16], const uint8_t dn[16], uint8_t out[32]);

typedef struct {
  uint8_t key_id[4];
  uint8_t psk[32];
  char label[NDP_LABEL_MAX + 1];
} ndp_paired_key;

typedef struct {
  uint8_t device_id[16];
  int has_device_id;
  ndp_paired_key keys[NDP_MAX_KEYS];
  int count;
} ndp_keystore;

/* Pairing window, owned by the platform (it outlives connections). */
typedef struct {
  int active;
  uint8_t code[NDP_CODE_BYTES];
  uint64_t expires_ms;
} ndp_pairing;

/* Adds a key. Returns NDP_OK, or NDP_ST_NO_SPACE when the store is full. An identical key is a no-op. */
int ndp_keystore_add(ndp_keystore *ks, const uint8_t psk[32], const char *label);
void ndp_keystore_clear(ndp_keystore *ks);

/* File format: "NDPK" version(1) count(1) rsvd(2) device_id[16] count x {key_id[4] psk[32] label[16]} sha256(all before)[32]. */
#define NDP_KEYSTORE_MAX_BYTES (24 + NDP_MAX_KEYS * 52 + 32)
/* Returns the length written, or 0 when `cap` is too small. */
size_t ndp_keystore_serialize(const ndp_keystore *ks, uint8_t *out, size_t cap);
/* Returns 0, or -1 for a bad magic/version/checksum/size (the store is left empty). */
int ndp_keystore_parse(ndp_keystore *ks, const uint8_t *in, size_t len);

#endif
