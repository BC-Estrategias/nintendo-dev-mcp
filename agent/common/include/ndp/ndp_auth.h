/* Pairing and authentication primitives (spec §4): pairing code, key derivation, HMAC proofs,
 * session key, and the persistent key store. No I/O: the platform reads/writes the store bytes. */
#ifndef NDP_AUTH_H
#define NDP_AUTH_H

#include "ndp/ndp_defs.h"

#define NDP_CODE_BYTES 10
#define NDP_CODE_TEXT 20 /* "XXXX-XXXX-XXXX-XXXX" + NUL */
#define NDP_MAX_KEYS 8
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

/* Stages of pairing by number comparison (spec §4.8). */
typedef enum {
  NDP_P2_IDLE = 0,
  NDP_P2_COMMITTED = 1, /* the client committed; the console sent its key and waits for the client's */
  NDP_P2_WAITING = 2,   /* the number is on the console's screen: waiting for the person (A = yes, B = no) */
  NDP_P2_APPROVED = 3,  /* the person said yes: the key is stored, the client collects it with PAIR_POLL */
  NDP_P2_DENIED = 4,    /* the person said no, or the request expired */
  NDP_P2_FULL = 5       /* the person said yes but the key store is full */
} ndp_p2_stage;
#define NDP_P2_MAX_ATTEMPTS 5

/* Pairing window, owned by the platform (it outlives connections). */
typedef struct {
  int active;
  uint8_t code[NDP_CODE_BYTES];
  uint64_t expires_ms;
  /* number comparison: one request at a time */
  int p2_stage;
  int p2_owner;                /* which connection made the request (a small id given by the server) */
  int p2_attempts;             /* failed or denied requests in this window */
  uint8_t p2_commit[32];
  uint8_t p2_secret[32];       /* the console's ephemeral X25519 secret: wiped as soon as the shared secret exists */
  uint8_t p2_pub_c[32], p2_pub_b[32], p2_nonce_c[16], p2_nonce_b[16];
  uint8_t p2_psk[32];
  uint32_t p2_sas;             /* the number on the screen, 0..999999 */
  char p2_label[NDP_LABEL_MAX + 1];
  char p2_peer[24];
  uint64_t p2_deadline_ms;     /* 0 = the server sets it on its next step */
} ndp_pairing;

/* Adds a key; it never fails for lack of room: an identical key is a no-op, a new pairing with the SAME label replaces that
 * label's earlier key (the same browser pairing again), and when the store is full the OLDEST key is dropped. Returns NDP_OK. */
int ndp_keystore_add(ndp_keystore *ks, const uint8_t psk[32], const char *label);
/* What ndp_keystore_add would drop for a new pairing named `label`: the index of the entry with that label, or (when full) 0,
 * the oldest; -1 when nothing is replaced. The console tells the person before they press A. */
int ndp_keystore_replace_target(const ndp_keystore *ks, const char *label);
void ndp_keystore_clear(ndp_keystore *ks);

/* File format: "NDPK" version(1) count(1) rsvd(2) device_id[16] count x {key_id[4] psk[32] label[16]} sha256(all before)[32]. */
#define NDP_KEYSTORE_MAX_BYTES (24 + NDP_MAX_KEYS * 52 + 32)
/* Returns the length written, or 0 when `cap` is too small. */
size_t ndp_keystore_serialize(const ndp_keystore *ks, uint8_t *out, size_t cap);
/* Returns 0, or -1 for a bad magic/version/checksum/size (the store is left empty). */
int ndp_keystore_parse(ndp_keystore *ks, const uint8_t *in, size_t len);

#endif
