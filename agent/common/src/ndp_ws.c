#include "ndp/ndp_ws.h"

#include <string.h>

/* ------------------------------------------------------------------ SHA-1 (FIPS 180-4) */
static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
  uint32_t w[80], a, b, c, d, e, t;
  int i;
  for (i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
  for (i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
    t = rol(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ndp_sha1(const void *data, size_t len, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  const uint8_t *p = (const uint8_t *)data;
  uint8_t tail[128];
  size_t full = len / 64, rem = len % 64, i, tl;
  uint64_t bits = (uint64_t)len * 8u;
  for (i = 0; i < full; i++) sha1_block(h, p + i * 64);
  memset(tail, 0, sizeof tail);
  if (rem) memcpy(tail, p + full * 64, rem);
  tail[rem] = 0x80;
  tl = rem < 56 ? 64 : 128;
  for (i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
  sha1_block(h, tail);
  if (tl == 128) sha1_block(h, tail + 64);
  for (i = 0; i < 5; i++) {
    out[i * 4] = (uint8_t)(h[i] >> 24); out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(h[i] >> 8); out[i * 4 + 3] = (uint8_t)h[i];
  }
}

/* ------------------------------------------------------------------ base64 */
size_t ndp_base64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t need = (n + 2) / 3 * 4, o = 0, i;
  if (cap < need + 1) return 0;
  for (i = 0; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = A[v >> 18]; out[o++] = A[(v >> 12) & 63]; out[o++] = A[(v >> 6) & 63]; out[o++] = A[v & 63];
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = A[v >> 18]; out[o++] = A[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
  } else if (n - i == 2) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;
    out[o++] = A[v >> 18]; out[o++] = A[(v >> 12) & 63]; out[o++] = A[(v >> 6) & 63]; out[o++] = '=';
  }
  out[o] = '\0';
  return o;
}

/* ------------------------------------------------------------------ HTTP request head */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq(const char *a, size_t al, const char *b) { /* case-insensitive, `b` is lower case */
  size_t i;
  if (al != strlen(b)) return 0;
  for (i = 0; i < al; i++)
    if (lower((unsigned char)a[i]) != b[i]) return 0;
  return 1;
}

/* Does the comma-separated header value contain `token` (case-insensitively, as a whole token)? */
static int has_token(const char *v, size_t n, const char *token) {
  size_t i = 0, tl = strlen(token);
  while (i < n) {
    size_t s, e;
    while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
    s = i;
    while (i < n && v[i] != ',') i++;
    e = i;
    while (e > s && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
    if (e - s == tl && ieq(v + s, tl, token)) return 1;
  }
  return 0;
}

static int copy_value(char *dst, size_t cap, const char *v, size_t n) {
  if (n >= cap) return NDP_HTTP_TOO_LARGE;
  memcpy(dst, v, n);
  dst[n] = '\0';
  return 0;
}

static int all_digits(const char *v, size_t n, long *out) {
  size_t i;
  long x = 0;
  if (n == 0 || n > 9) return 0;
  for (i = 0; i < n; i++) {
    if (v[i] < '0' || v[i] > '9') return 0;
    x = x * 10 + (v[i] - '0');
  }
  *out = x;
  return 1;
}

int ndp_http_parse(const char *buf, size_t len, ndp_http_req *out) {
  size_t i, end = 0, line_end, pos;
  int saw_upgrade = 0, saw_conn_upgrade = 0, saw_host = 0, rc;
  memset(out, 0, sizeof *out);
  /* find the blank line */
  for (i = 0; i + 3 < len && i + 4 <= NDP_HTTP_HEAD_MAX; i++)
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = i + 4; break; }
  if (!end) return len >= NDP_HTTP_HEAD_MAX ? NDP_HTTP_TOO_LARGE : NDP_HTTP_NEED_MORE;
  for (i = 0; i < end; i++) { /* only printable ASCII, TAB and CRLF are allowed in a head */
    unsigned char c = (unsigned char)buf[i];
    if (c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f)) continue;
    return NDP_HTTP_BAD;
  }
  out->head_len = end;

  /* request line: METHOD SP target SP HTTP/1.x */
  for (line_end = 0; line_end + 1 < end && !(buf[line_end] == '\r' && buf[line_end + 1] == '\n'); line_end++) {}
  {
    size_t m = 0, t0, t1;
    while (m < line_end && buf[m] != ' ') m++;
    if (m == 0 || m >= line_end) return NDP_HTTP_BAD;
    out->method = m == 3 && !memcmp(buf, "GET", 3) ? NDP_HTTP_GET : m == 4 && !memcmp(buf, "HEAD", 4) ? NDP_HTTP_HEAD : NDP_HTTP_OTHER;
    t0 = m + 1;
    t1 = t0;
    while (t1 < line_end && buf[t1] != ' ') t1++;
    if (t1 == t0 || t1 >= line_end) return NDP_HTTP_BAD;
    if (buf[t0] != '/') return NDP_HTTP_BAD;
    { /* the path, without the query string or fragment */
      size_t pe = t0;
      while (pe < t1 && buf[pe] != '?' && buf[pe] != '#') pe++;
      rc = copy_value(out->path, sizeof out->path, buf + t0, pe - t0);
      if (rc) return rc;
    }
    if (line_end - (t1 + 1) != 8 || memcmp(buf + t1 + 1, "HTTP/1.", 7) != 0) return NDP_HTTP_VERSION;
    if (buf[line_end - 1] != '0' && buf[line_end - 1] != '1') return NDP_HTTP_VERSION;
  }

  /* headers */
  pos = line_end + 2;
  while (pos + 2 <= end) {
    size_t ls = pos, le, colon, vs, ve, nl;
    for (le = ls; le + 1 < end && !(buf[le] == '\r' && buf[le + 1] == '\n'); le++) {}
    if (le == ls) break; /* the blank line */
    pos = le + 2;
    if (buf[ls] == ' ' || buf[ls] == '\t') return NDP_HTTP_BAD; /* obsolete line folding */
    for (colon = ls; colon < le && buf[colon] != ':'; colon++) {}
    if (colon == ls || colon >= le) return NDP_HTTP_BAD;
    nl = colon - ls;
    if (buf[colon - 1] == ' ' || buf[colon - 1] == '\t') return NDP_HTTP_BAD; /* no space before the colon */
    vs = colon + 1;
    while (vs < le && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
    ve = le;
    while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve--;
    if (ieq(buf + ls, nl, "host")) {
      if (saw_host) return NDP_HTTP_BAD; /* two Host headers */
      saw_host = 1;
      if ((rc = copy_value(out->host, sizeof out->host, buf + vs, ve - vs))) return rc;
    } else if (ieq(buf + ls, nl, "origin")) {
      out->has_origin = 1;
      if ((rc = copy_value(out->origin, sizeof out->origin, buf + vs, ve - vs))) return rc;
    } else if (ieq(buf + ls, nl, "upgrade")) {
      saw_upgrade = has_token(buf + vs, ve - vs, "websocket");
    } else if (ieq(buf + ls, nl, "connection")) {
      if (has_token(buf + vs, ve - vs, "upgrade")) saw_conn_upgrade = 1;
    } else if (ieq(buf + ls, nl, "sec-websocket-key")) {
      if ((rc = copy_value(out->ws_key, sizeof out->ws_key, buf + vs, ve - vs))) return rc;
    } else if (ieq(buf + ls, nl, "sec-websocket-version")) {
      long v;
      if (!all_digits(buf + vs, ve - vs, &v)) return NDP_HTTP_BAD;
      out->ws_version = (int)v;
    } else if (ieq(buf + ls, nl, "content-length")) {
      long v;
      if (!all_digits(buf + vs, ve - vs, &v)) return NDP_HTTP_BAD;
      if (v != 0) return NDP_HTTP_BODY;
    } else if (ieq(buf + ls, nl, "transfer-encoding")) {
      return NDP_HTTP_BODY;
    }
  }
  out->upgrade_websocket = saw_upgrade && saw_conn_upgrade;
  return NDP_HTTP_OK;
}

static int host_part_eq(const char *host, const char *ip) {
  size_t n = strlen(ip);
  if (strncmp(host, ip, n) != 0) return 0;
  if (host[n] == '\0') return 1;
  if (host[n] != ':') return 0;
  { /* ":port" digits only */
    const char *p = host + n + 1;
    if (*p == '\0') return 0;
    for (; *p; p++)
      if (*p < '0' || *p > '9') return 0;
  }
  return 1;
}

int ndp_http_host_ok(const ndp_http_req *r, const char *local_ip) {
  if (r->host[0] == '\0') return 0;
  if (host_part_eq(r->host, local_ip)) return 1;
  if (strncmp(local_ip, "127.", 4) == 0) {
    if (strncmp(r->host, "localhost", 9) == 0) {
      const char *p = r->host + 9;
      if (*p == '\0') return 1;
      if (*p == ':' && p[1] != '\0') {
        for (p++; *p; p++)
          if (*p < '0' || *p > '9') return 0;
        return 1;
      }
    }
  }
  return 0;
}

int ndp_http_origin_ok(const ndp_http_req *r) {
  size_t hl;
  if (!r->has_origin) return 1;
  if (strncmp(r->origin, "http://", 7) != 0) return 0;
  hl = strlen(r->host);
  return hl > 0 && strlen(r->origin) == 7 + hl && strcmp(r->origin + 7, r->host) == 0;
}

/* ------------------------------------------------------------------ WebSocket */
int ndp_ws_accept_key(const char *key, char out[29]) {
  static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  char buf[24 + sizeof GUID];
  uint8_t h[20];
  size_t kl = strlen(key), i;
  if (kl != 24) return -1; /* base64 of 16 random bytes */
  for (i = 0; i < kl; i++) {
    char c = key[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return -1;
  }
  memcpy(buf, key, kl);
  memcpy(buf + kl, GUID, sizeof GUID - 1);
  ndp_sha1(buf, kl + sizeof GUID - 1, h);
  return ndp_base64_encode(h, 20, out, 29) == 28 ? 0 : -1;
}

size_t ndp_ws_frame_header(uint8_t opcode, uint64_t len, uint8_t out[NDP_WS_HEADER_MAX]) {
  out[0] = (uint8_t)(0x80 | (opcode & 0x0F));
  if (len < 126) { out[1] = (uint8_t)len; return 2; }
  if (len <= 0xFFFF) { out[1] = 126; out[2] = (uint8_t)(len >> 8); out[3] = (uint8_t)len; return 4; }
  out[1] = 127;
  { int i; for (i = 0; i < 8; i++) out[2 + i] = (uint8_t)(len >> (8 * (7 - i))); }
  return 10;
}

enum { ST_HEAD = 0, ST_PAYLOAD = 1 };

void ndp_ws_dec_init(ndp_ws_dec *d) { memset(d, 0, sizeof *d); }

static ndp_ws_event ev_error(uint16_t code) { ndp_ws_event e; memset(&e, 0, sizeof e); e.kind = NDP_WS_ERROR; e.code = code; return e; }

/* How many header bytes this frame needs, once the first two are known. */
static size_t head_need(const uint8_t *h) {
  size_t n = 2, l7 = h[1] & 0x7F;
  if (l7 == 126) n += 2; else if (l7 == 127) n += 8;
  return n + 4; /* clients always mask */
}

ndp_ws_event ndp_ws_dec_feed(ndp_ws_dec *d, uint8_t *in, size_t len, size_t *used) {
  ndp_ws_event ev;
  size_t pos = 0;
  memset(&ev, 0, sizeof ev);
  *used = 0;
  for (;;) {
    if (d->state == ST_HEAD) {
      while (pos < len && d->head_len < (d->head_len < 2 ? 2u : d->head_need)) {
        d->head[d->head_len++] = in[pos++];
        if (d->head_len == 2) d->head_need = head_need(d->head);
      }
      *used = pos;
      if (d->head_len >= 2) { /* refuse as soon as the first two bytes show a violation, without waiting for the rest */
        if (d->head[0] & 0x70) return ev_error(1002);
        if (!(d->head[1] & 0x80)) return ev_error(1002);
      }
      if (d->head_len < 2 || d->head_len < d->head_need) return ev; /* NEED */
      { /* the whole header is in: validate */
        const uint8_t *h = d->head;
        int fin = (h[0] & 0x80) != 0, masked = (h[1] & 0x80) != 0;
        uint8_t op = h[0] & 0x0F;
        size_t l7 = h[1] & 0x7F, off = 2;
        uint64_t plen = l7;
        if (h[0] & 0x70) return ev_error(1002);          /* reserved bits */
        if (!masked) return ev_error(1002);              /* a client MUST mask */
        if (l7 == 126) { plen = (uint64_t)h[2] << 8 | h[3]; off = 4; }
        else if (l7 == 127) {
          int i;
          plen = 0;
          for (i = 0; i < 8; i++) plen = plen << 8 | h[2 + i];
          off = 10;
          if (plen >> 63) return ev_error(1002);
        }
        memcpy(d->mask, h + off, 4);
        if (op >= 8) { /* control frames: short, unfragmented */
          if (op != NDP_WS_OP_CLOSE && op != NDP_WS_OP_PING && op != NDP_WS_OP_PONG) return ev_error(1002);
          if (!fin || plen > 125) return ev_error(1002);
          d->ctl_len = 0;
        } else if (op == NDP_WS_OP_CONT) {
          if (!d->in_message) return ev_error(1002);
        } else if (op == NDP_WS_OP_BINARY) {
          if (d->in_message) return ev_error(1002);
        } else if (op == NDP_WS_OP_TEXT) {
          return ev_error(1003);                         /* the page only sends binary */
        } else {
          return ev_error(1002);
        }
        if (plen > NDP_WS_MAX_FRAME) return ev_error(1009);
        d->opcode = op;
        d->remaining = plen;
        d->mask_pos = 0;
        if (op < 8) d->in_message = !fin;
        d->state = ST_PAYLOAD;
        d->head_len = 0;
      }
    }
    /* payload */
    if (d->remaining == 0) {
      d->state = ST_HEAD;
      if (d->opcode == NDP_WS_OP_CLOSE) {
        ev.kind = NDP_WS_CLOSE;
        ev.code = d->ctl_len >= 2 ? (uint16_t)(d->ctl[0] << 8 | d->ctl[1]) : 1005;
        *used = pos;
        return ev;
      }
      if (d->opcode == NDP_WS_OP_PING) {
        ev.kind = NDP_WS_PING;
        ev.data = d->ctl;
        ev.len = d->ctl_len;
        *used = pos;
        return ev;
      }
      if (d->opcode == NDP_WS_OP_PONG) { d->ctl_len = 0; continue; } /* ignored */
      if (pos >= len) { *used = pos; return ev; }
      continue; /* an empty data frame */
    }
    if (pos >= len) { *used = pos; return ev; }
    {
      size_t avail = len - pos, n = avail < d->remaining ? avail : (size_t)d->remaining, i;
      for (i = 0; i < n; i++) in[pos + i] ^= d->mask[(d->mask_pos + i) & 3];
      d->mask_pos = (d->mask_pos + n) & 3;
      d->remaining -= n;
      if (d->opcode >= 8) {
        if (d->ctl_len + n > sizeof d->ctl) return ev_error(1002);
        memcpy(d->ctl + d->ctl_len, in + pos, n);
        d->ctl_len += n;
        pos += n;
        continue;
      }
      ev.kind = NDP_WS_DATA;
      ev.data = in + pos;
      ev.len = n;
      pos += n;
      *used = pos;
      return ev;
    }
  }
}
