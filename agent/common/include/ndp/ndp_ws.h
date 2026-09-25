/* HTTP/1.1 request parsing and WebSocket (RFC 6455) framing for the console's web page. No sockets here: the server
 * feeds bytes in and sends the bytes that come out. Only what the page needs is supported, and everything else is
 * refused (spec §15):
 *   - GET/HEAD with no body; the request head must fit HTTP_HEAD_MAX bytes;
 *   - one WebSocket endpoint; only BINARY messages (each carries a piece of the NDP byte stream). */
#ifndef NDP_WS_H
#define NDP_WS_H

#include <stddef.h>
#include <stdint.h>

/* ---- SHA-1 and base64 (only for the WebSocket handshake; not used for anything security-relevant) ---- */
void ndp_sha1(const void *data, size_t len, uint8_t out[20]);
/* Writes ceil(n/3)*4 characters and a NUL; `cap` must be at least that + 1. Returns the length, or 0 if too small. */
size_t ndp_base64_encode(const uint8_t *in, size_t n, char *out, size_t cap);

/* ---- HTTP request head ---- */
#define NDP_HTTP_HEAD_MAX 2048
#define NDP_HTTP_PATH_MAX 96
#define NDP_HTTP_HOST_MAX 64
#define NDP_HTTP_ORIGIN_MAX 96

typedef enum { NDP_HTTP_GET = 1, NDP_HTTP_HEAD = 2, NDP_HTTP_OTHER = 3 } ndp_http_method;

typedef struct {
  ndp_http_method method;
  char path[NDP_HTTP_PATH_MAX]; /* without the query string */
  char host[NDP_HTTP_HOST_MAX]; /* the Host header value ("" if absent) */
  char origin[NDP_HTTP_ORIGIN_MAX];
  int has_origin;
  int upgrade_websocket; /* Upgrade: websocket AND Connection contains "upgrade" */
  char ws_key[32];       /* Sec-WebSocket-Key ("" if absent) */
  int ws_version;        /* Sec-WebSocket-Version, 0 if absent */
  size_t head_len;       /* bytes of the request head, including the blank line */
} ndp_http_req;

#define NDP_HTTP_NEED_MORE 0
#define NDP_HTTP_OK 1
#define NDP_HTTP_BAD -400        /* malformed */
#define NDP_HTTP_TOO_LARGE -431  /* head bigger than NDP_HTTP_HEAD_MAX, or a field too long */
#define NDP_HTTP_BODY -413       /* the request announces a body (Content-Length > 0 / Transfer-Encoding) */
#define NDP_HTTP_VERSION -505

/* Parses the request head in buf[0..len). Returns NDP_HTTP_NEED_MORE until the blank line arrives. */
int ndp_http_parse(const char *buf, size_t len, ndp_http_req *out);

/* DNS-rebinding / cross-site defence for the WebSocket upgrade (spec §15.2): the Host must be the address the client
 * really connected to (`local_ip`, dotted decimal), optionally with ":port", or "localhost" when `local_ip` is loopback;
 * an Origin, when present, must be exactly "http://<Host>". Non-browser clients send no Origin. */
int ndp_http_host_ok(const ndp_http_req *r, const char *local_ip);
int ndp_http_origin_ok(const ndp_http_req *r);

/* ---- WebSocket ---- */
/* Sec-WebSocket-Accept for `key` (base64 text, at most 24 chars). `out` needs 29 bytes. Returns 0 or -1 for a bad key. */
int ndp_ws_accept_key(const char *key, char out[29]);

#define NDP_WS_MAX_FRAME 70000u /* larger frames close the connection with 1009 (an NDP frame is at most ~65.6 KB) */
#define NDP_WS_HEADER_MAX 10

/* Server-to-client frame header (FIN set, unmasked). Returns the header length (2..10). */
size_t ndp_ws_frame_header(uint8_t opcode, uint64_t payload_len, uint8_t out[NDP_WS_HEADER_MAX]);

enum { NDP_WS_OP_CONT = 0, NDP_WS_OP_TEXT = 1, NDP_WS_OP_BINARY = 2, NDP_WS_OP_CLOSE = 8, NDP_WS_OP_PING = 9, NDP_WS_OP_PONG = 10 };

typedef enum { NDP_WS_NEED = 0, NDP_WS_DATA = 1, NDP_WS_PING = 2, NDP_WS_CLOSE = 3, NDP_WS_ERROR = 4 } ndp_ws_event_kind;

typedef struct {
  ndp_ws_event_kind kind;
  const uint8_t *data; /* NDP_WS_DATA: a slice of the unmasked message payload (points into the caller's input buffer);
                        * NDP_WS_PING: the ping payload (echo it in a pong) */
  size_t len;
  uint16_t code; /* NDP_WS_CLOSE / NDP_WS_ERROR: the WebSocket close code to send back (1000, 1002, 1003, 1009, ...) */
} ndp_ws_event;

typedef struct {
  int state;
  uint8_t head[14];
  size_t head_len, head_need;
  uint8_t mask[4];
  uint64_t remaining;
  size_t mask_pos;
  uint8_t opcode;   /* of the frame being read */
  int in_message;   /* a fragmented data message is open */
  uint8_t ctl[125]; /* control-frame payload being collected */
  size_t ctl_len;
} ndp_ws_dec;

void ndp_ws_dec_init(ndp_ws_dec *d);
/* Consumes bytes from `in` (unmasked IN PLACE) and reports at most one event; `*used` is how much was consumed.
 * Call again with the rest until it returns NDP_WS_NEED with *used == len. After NDP_WS_ERROR/NDP_WS_CLOSE the
 * connection must be closed. */
ndp_ws_event ndp_ws_dec_feed(ndp_ws_dec *d, uint8_t *in, size_t len, size_t *used);

#endif
