#include "ndp/ndp_frame.h"

#include <string.h>

#include "ndp/ndp_sha256.h"

static const uint8_t MAGIC[4] = {'N', 'D', 'P', '1'};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void ndp_header_encode(const ndp_header *h, uint8_t out[NDP_HEADER_SIZE]) {
  memcpy(out, MAGIC, 4);
  out[4] = h->version;
  out[5] = h->kind;
  put16(out + 6, h->flags);
  put32(out + 8, h->request_id);
  put16(out + 12, h->command);
  put16(out + 14, h->status);
  put32(out + 16, h->payload_len);
}

int ndp_header_decode(const uint8_t in[NDP_HEADER_SIZE], ndp_header *h) {
  if (memcmp(in, MAGIC, 4) != 0) return NDP_ST_BAD_FRAME;
  h->version = in[4];
  h->kind = in[5];
  h->flags = get16(in + 6);
  h->request_id = get32(in + 8);
  h->command = get16(in + 12);
  h->status = get16(in + 14);
  h->payload_len = get32(in + 16);
  if (h->flags & ~NDP_FLAGS_KNOWN) return NDP_ST_BAD_FRAME;
  return NDP_OK;
}

size_t ndp_frame_encode(uint8_t *out, size_t cap, const ndp_header *h, const uint8_t *payload,
                        size_t payload_len, const uint8_t *mac) {
  ndp_header hh = *h;
  size_t total = NDP_HEADER_SIZE + payload_len;
  int with_mac = (h->flags & NDP_FLAG_MAC) && mac != NULL;
  if (with_mac) total += NDP_MAC_SIZE;
  if (total > cap || payload_len > 0xFFFFFFFFu) return 0;
  hh.payload_len = (uint32_t)payload_len;
  ndp_header_encode(&hh, out);
  if (payload_len) memmove(out + NDP_HEADER_SIZE, payload, payload_len);
  if (with_mac) memcpy(out + NDP_HEADER_SIZE + payload_len, mac, NDP_MAC_SIZE);
  return total;
}

void ndp_decoder_init(ndp_decoder *d, uint8_t *buf, size_t cap, uint32_t max_payload) {
  size_t room = cap > (size_t)(NDP_HEADER_SIZE + NDP_MAC_SIZE) ? cap - (NDP_HEADER_SIZE + NDP_MAC_SIZE) : 0;
  d->buf = buf;
  d->cap = cap;
  d->max_payload = (room < max_payload) ? (uint32_t)room : max_payload;
  d->len = 0;
  d->need = 0;
  d->state = 0;
  d->error = NDP_OK;
  memset(&d->hdr, 0, sizeof d->hdr);
}

int ndp_decoder_feed(ndp_decoder *d, const uint8_t *data, size_t len, size_t *consumed) {
  size_t used = 0;
  if (d->state == 3) { *consumed = 0; return NDP_DEC_ERROR; }
  if (d->state == 2) { *consumed = 0; return NDP_DEC_FRAME; }
  while (len - used > 0 || d->state == 1) {
    size_t want, take;
    if (d->state == 0) {
      want = NDP_HEADER_SIZE - d->len;
    } else {
      want = d->need - d->len;
    }
    take = len - used < want ? len - used : want;
    if (take) {
      memcpy(d->buf + d->len, data + used, take);
      d->len += take;
      used += take;
    }
    if (d->state == 0) {
      int rc;
      if (d->len < NDP_HEADER_SIZE) break;
      rc = ndp_header_decode(d->buf, &d->hdr);
      if (rc != NDP_OK) { d->state = 3; d->error = rc; *consumed = used; return NDP_DEC_ERROR; }
      if (d->hdr.payload_len > d->max_payload) {
        d->state = 3; d->error = NDP_ST_TOO_LARGE; *consumed = used; return NDP_DEC_ERROR;
      }
      d->need = NDP_HEADER_SIZE + (size_t)d->hdr.payload_len + ((d->hdr.flags & NDP_FLAG_MAC) ? NDP_MAC_SIZE : 0);
      d->state = 1;
    }
    if (d->state == 1) {
      if (d->len == d->need) { d->state = 2; *consumed = used; return NDP_DEC_FRAME; }
      if (len - used == 0) break;
    }
  }
  *consumed = used;
  return NDP_DEC_NEED_MORE;
}

const ndp_header *ndp_decoder_header(const ndp_decoder *d) { return &d->hdr; }
const uint8_t *ndp_decoder_payload(const ndp_decoder *d) { return d->buf + NDP_HEADER_SIZE; }
const uint8_t *ndp_decoder_mac(const ndp_decoder *d) {
  return (d->hdr.flags & NDP_FLAG_MAC) ? d->buf + NDP_HEADER_SIZE + d->hdr.payload_len : NULL;
}

void ndp_decoder_release(ndp_decoder *d) {
  d->len = 0;
  d->need = 0;
  d->state = 0;
}

void ndp_frame_mac(const uint8_t *key, size_t key_len, uint64_t counter, const uint8_t header[NDP_HEADER_SIZE],
                   const uint8_t *payload, size_t payload_len, uint8_t out[NDP_MAC_SIZE]) {
  ndp_hmac_ctx c;
  uint8_t ctr[8], full[32];
  int i;
  for (i = 0; i < 8; i++) ctr[i] = (uint8_t)(counter >> (8 * i));
  ndp_hmac_init(&c, key, key_len);
  ndp_hmac_update(&c, ctr, 8);
  ndp_hmac_update(&c, header, NDP_HEADER_SIZE);
  if (payload_len) ndp_hmac_update(&c, payload, payload_len);
  ndp_hmac_final(&c, full);
  memcpy(out, full, NDP_MAC_SIZE);
}
