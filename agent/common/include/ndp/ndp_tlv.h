/* TLV payload encoding/decoding: [tag u16][len u16][value] (spec §3). */
#ifndef NDP_TLV_H
#define NDP_TLV_H

#include "ndp/ndp_defs.h"

typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
  int overflow; /* set when a put did not fit; len is then meaningless */
} ndp_tlv_w;

void ndp_tlv_w_init(ndp_tlv_w *w, uint8_t *buf, size_t cap);
void ndp_tlv_put(ndp_tlv_w *w, uint16_t tag, const void *value, size_t len);
void ndp_tlv_put_u16(ndp_tlv_w *w, uint16_t tag, uint16_t v);
void ndp_tlv_put_u32(ndp_tlv_w *w, uint16_t tag, uint32_t v);
void ndp_tlv_put_u64(ndp_tlv_w *w, uint16_t tag, uint64_t v);
void ndp_tlv_put_str(ndp_tlv_w *w, uint16_t tag, const char *s);

/* Returns 1 when the payload is a well-formed TLV sequence, else 0. */
int ndp_tlv_validate(const uint8_t *p, size_t len);

/* Finds the FIRST occurrence of a tag in a payload already validated by ndp_tlv_validate.
 * Returns 1 and sets value and len when found, else 0. */
int ndp_tlv_find(const uint8_t *p, size_t plen, uint16_t tag, const uint8_t **value, size_t *len);

uint16_t ndp_tlv_get_u16(const uint8_t *v);
uint32_t ndp_tlv_get_u32(const uint8_t *v);
uint64_t ndp_tlv_get_u64(const uint8_t *v);

#endif
