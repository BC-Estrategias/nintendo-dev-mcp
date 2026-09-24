#include "ndp/ndp_tlv.h"

#include <string.h>

void ndp_tlv_w_init(ndp_tlv_w *w, uint8_t *buf, size_t cap) {
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
  w->overflow = 0;
}

void ndp_tlv_put(ndp_tlv_w *w, uint16_t tag, const void *value, size_t len) {
  if (w->overflow) return;
  if (len > 0xFFFF || w->len + 4 + len > w->cap) { w->overflow = 1; return; }
  w->buf[w->len] = (uint8_t)tag;
  w->buf[w->len + 1] = (uint8_t)(tag >> 8);
  w->buf[w->len + 2] = (uint8_t)len;
  w->buf[w->len + 3] = (uint8_t)(len >> 8);
  if (len) memcpy(w->buf + w->len + 4, value, len);
  w->len += 4 + len;
}

void ndp_tlv_put_u16(ndp_tlv_w *w, uint16_t tag, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  ndp_tlv_put(w, tag, b, 2);
}

void ndp_tlv_put_u32(ndp_tlv_w *w, uint16_t tag, uint32_t v) {
  uint8_t b[4];
  int i;
  for (i = 0; i < 4; i++) b[i] = (uint8_t)(v >> (8 * i));
  ndp_tlv_put(w, tag, b, 4);
}

void ndp_tlv_put_u64(ndp_tlv_w *w, uint16_t tag, uint64_t v) {
  uint8_t b[8];
  int i;
  for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
  ndp_tlv_put(w, tag, b, 8);
}

void ndp_tlv_put_str(ndp_tlv_w *w, uint16_t tag, const char *s) { ndp_tlv_put(w, tag, s, strlen(s)); }

int ndp_tlv_validate(const uint8_t *p, size_t len) {
  size_t i = 0;
  while (i < len) {
    size_t vl;
    if (len - i < 4) return 0;
    vl = (size_t)p[i + 2] | ((size_t)p[i + 3] << 8);
    if (vl > len - i - 4) return 0;
    i += 4 + vl;
  }
  return 1;
}

int ndp_tlv_find(const uint8_t *p, size_t plen, uint16_t tag, const uint8_t **value, size_t *len) {
  size_t i = 0;
  while (plen - i >= 4) {
    uint16_t t = (uint16_t)(p[i] | (p[i + 1] << 8));
    size_t vl = (size_t)p[i + 2] | ((size_t)p[i + 3] << 8);
    if (vl > plen - i - 4) return 0;
    if (t == tag) { *value = p + i + 4; *len = vl; return 1; }
    i += 4 + vl;
  }
  return 0;
}

uint16_t ndp_tlv_get_u16(const uint8_t *v) { return (uint16_t)(v[0] | (v[1] << 8)); }
uint32_t ndp_tlv_get_u32(const uint8_t *v) {
  return (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
}
uint64_t ndp_tlv_get_u64(const uint8_t *v) {
  return (uint64_t)ndp_tlv_get_u32(v) | ((uint64_t)ndp_tlv_get_u32(v + 4) << 32);
}
