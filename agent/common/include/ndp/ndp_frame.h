/* NDP v1 frame header, encoder and streaming decoder (spec §2, §11). */
#ifndef NDP_FRAME_H
#define NDP_FRAME_H

#include "ndp/ndp_defs.h"

typedef struct {
  uint8_t version;
  uint8_t kind;
  uint16_t flags;
  uint32_t request_id;
  uint16_t command;
  uint16_t status;
  uint32_t payload_len;
} ndp_header;

/* Serializes the 20-byte header. */
void ndp_header_encode(const ndp_header *h, uint8_t out[NDP_HEADER_SIZE]);

/* Parses and validates (magic, flags) a header. Returns NDP_OK or NDP_ST_BAD_FRAME. */
int ndp_header_decode(const uint8_t in[NDP_HEADER_SIZE], ndp_header *h);

/* Writes header + payload (+ mac when h->flags has NDP_FLAG_MAC and mac != NULL).
 * h->payload_len is taken from payload_len. Returns bytes written, or 0 if `cap` is too small. */
size_t ndp_frame_encode(uint8_t *out, size_t cap, const ndp_header *h, const uint8_t *payload,
                        size_t payload_len, const uint8_t *mac);

/* Streaming decoder. The caller provides the buffer; cap must be >= NDP_HEADER_SIZE +
 * max_payload + NDP_MAC_SIZE (max_payload is clamped if it is not). */
typedef struct {
  uint8_t *buf;
  size_t cap;
  uint32_t max_payload;
  size_t len;  /* bytes accumulated in buf */
  size_t need; /* total bytes of the frame once the header is known, else 0 */
  int state;   /* 0 = header, 1 = body, 2 = ready, 3 = error */
  int error;   /* NDP_ST_* when state == 3 */
  ndp_header hdr;
} ndp_decoder;

enum { NDP_DEC_NEED_MORE = 0, NDP_DEC_FRAME = 1, NDP_DEC_ERROR = -1 };

void ndp_decoder_init(ndp_decoder *d, uint8_t *buf, size_t cap, uint32_t max_payload);

/* Consumes bytes from data (sets *consumed). Returns NDP_DEC_FRAME once a complete frame is
 * available (bytes after the frame are NOT consumed), NDP_DEC_NEED_MORE, or NDP_DEC_ERROR (see
 * d->error; the stream is desynchronized and the connection must be closed). While a frame is
 * ready, further calls return NDP_DEC_FRAME with *consumed = 0 until ndp_decoder_release. */
int ndp_decoder_feed(ndp_decoder *d, const uint8_t *data, size_t len, size_t *consumed);

const ndp_header *ndp_decoder_header(const ndp_decoder *d);
const uint8_t *ndp_decoder_payload(const ndp_decoder *d);
/* NULL when the frame carries no MAC. */
const uint8_t *ndp_decoder_mac(const ndp_decoder *d);
/* Discards the ready frame and prepares for the next one. */
void ndp_decoder_release(ndp_decoder *d);

/* mac = HMAC-SHA256(key, u64le(counter) || header || payload)[0:16]  (spec §4). */
void ndp_frame_mac(const uint8_t *key, size_t key_len, uint64_t counter, const uint8_t header[NDP_HEADER_SIZE],
                   const uint8_t *payload, size_t payload_len, uint8_t out[NDP_MAC_SIZE]);

#endif
