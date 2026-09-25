#include "ndp/ndp_auth.h"

#include <string.h>

#include "ndp/ndp_sha256.h"

static const char CROCKFORD[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

void ndp_code_encode(const uint8_t code[NDP_CODE_BYTES], char out[NDP_CODE_TEXT]) {
  int i, o = 0;
  for (i = 0; i < 16; i++) {
    int bit = i * 5, byte = bit / 8, off = bit % 8;
    unsigned v = (unsigned)code[byte] << 8;
    if (byte + 1 < NDP_CODE_BYTES) v |= code[byte + 1];
    out[o++] = CROCKFORD[(v >> (11 - off)) & 31];
    if (i % 4 == 3 && i != 15) out[o++] = '-';
  }
  out[o] = '\0';
}

int ndp_code_decode(const char *text, uint8_t code[NDP_CODE_BYTES]) {
  unsigned bits = 0;
  int nbits = 0, chars = 0, o = 0;
  const char *p;
  memset(code, 0, NDP_CODE_BYTES);
  for (p = text; *p; p++) {
    char c = *p;
    const char *hit;
    if (c == '-' || c == ' ') continue;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c == 'O') c = '0';
    else if (c == 'I' || c == 'L') c = '1';
    hit = strchr(CROCKFORD, c);
    if (!hit || c == '\0') return -1;
    if (++chars > 16) return -1;
    bits = (bits << 5) | (unsigned)(hit - CROCKFORD);
    nbits += 5;
    while (nbits >= 8) {
      nbits -= 8;
      code[o++] = (uint8_t)(bits >> nbits);
      bits &= (1u << nbits) - 1u;
    }
  }
  return chars == 16 ? 0 : -1;
}

void ndp_derive_psk(const uint8_t code[NDP_CODE_BYTES], uint8_t psk[32]) {
  ndp_sha256_ctx c;
  ndp_sha256_init(&c);
  ndp_sha256_update(&c, "NDP-PSK-v1", 10);
  ndp_sha256_update(&c, code, NDP_CODE_BYTES);
  ndp_sha256_final(&c, psk);
}

void ndp_key_id(const uint8_t psk[32], uint8_t id[4]) {
  uint8_t h[32];
  ndp_sha256(psk, 32, h);
  memcpy(id, h, 4);
}

void ndp_proof(const uint8_t psk[32], const char *label, const uint8_t cn[16], const uint8_t dn[16],
               const uint8_t *extra, size_t extra_len, uint8_t out[32]) {
  ndp_hmac_ctx c;
  ndp_hmac_init(&c, psk, 32);
  ndp_hmac_update(&c, label, strlen(label));
  ndp_hmac_update(&c, cn, 16);
  ndp_hmac_update(&c, dn, 16);
  if (extra_len) ndp_hmac_update(&c, extra, extra_len);
  ndp_hmac_final(&c, out);
}

void ndp_session_key(const uint8_t psk[32], const uint8_t cn[16], const uint8_t dn[16], uint8_t out[32]) {
  ndp_proof(psk, "session", cn, dn, NULL, 0, out);
}

void ndp_keystore_clear(ndp_keystore *ks) { memset(ks, 0, sizeof *ks); }

int ndp_keystore_add(ndp_keystore *ks, const uint8_t psk[32], const char *label) {
  uint8_t id[4];
  int i;
  ndp_key_id(psk, id);
  for (i = 0; i < ks->count; i++)
    if (memcmp(ks->keys[i].key_id, id, 4) == 0 && ndp_ct_equal(ks->keys[i].psk, psk, 32)) return NDP_OK;
  if (ks->count >= NDP_MAX_KEYS) return NDP_ST_NO_SPACE;
  memcpy(ks->keys[ks->count].key_id, id, 4);
  memcpy(ks->keys[ks->count].psk, psk, 32);
  memset(ks->keys[ks->count].label, 0, sizeof ks->keys[ks->count].label);
  strncpy(ks->keys[ks->count].label, label, NDP_LABEL_MAX);
  ks->count++;
  return NDP_OK;
}

size_t ndp_keystore_serialize(const ndp_keystore *ks, uint8_t *out, size_t cap) {
  size_t n = 24 + (size_t)ks->count * 52 + 32, o = 0;
  int i;
  if (cap < n) return 0;
  memcpy(out, "NDPK", 4);
  out[4] = 1;
  out[5] = (uint8_t)ks->count;
  out[6] = out[7] = 0;
  memcpy(out + 8, ks->device_id, 16);
  o = 24;
  for (i = 0; i < ks->count; i++) {
    memcpy(out + o, ks->keys[i].key_id, 4);
    memcpy(out + o + 4, ks->keys[i].psk, 32);
    memset(out + o + 36, 0, 16);
    memcpy(out + o + 36, ks->keys[i].label, strlen(ks->keys[i].label));
    o += 52;
  }
  ndp_sha256(out, o, out + o);
  return o + 32;
}

int ndp_keystore_parse(ndp_keystore *ks, const uint8_t *in, size_t len) {
  int count, i;
  size_t o, body;
  uint8_t h[32];
  ndp_keystore_clear(ks);
  if (len < 24 + 32 || memcmp(in, "NDPK", 4) != 0 || in[4] != 1) return -1;
  count = in[5];
  if (count > NDP_MAX_KEYS) return -1;
  body = 24 + (size_t)count * 52;
  if (len != body + 32) return -1;
  ndp_sha256(in, body, h);
  if (!ndp_ct_equal(h, in + body, 32)) return -1;
  memcpy(ks->device_id, in + 8, 16);
  ks->has_device_id = 1;
  o = 24;
  for (i = 0; i < count; i++) {
    memcpy(ks->keys[i].key_id, in + o, 4);
    memcpy(ks->keys[i].psk, in + o + 4, 32);
    memcpy(ks->keys[i].label, in + o + 36, NDP_LABEL_MAX);
    ks->keys[i].label[NDP_LABEL_MAX] = '\0';
    o += 52;
  }
  ks->count = count;
  return 0;
}
