#if !defined(_POSIX_C_SOURCE) && !defined(__3DS__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ndp/ndp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ndp/ndp_names.h"
#include "ndp/ndp_ws.h"

#define ACCEPT_BACKLOG 2
#define STREAM_FRAMES_PER_STEP 8

static void slog(ndp_server *s, const char *fmt, ...) {
  char line[160];
  va_list ap;
  if (!s->plat.log) return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  s->plat.log(s->plat.ctx, line);
}

static uint64_t now(ndp_server *s) { return s->plat.now_ms(s->plat.ctx); }
static int would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN; }

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Abortive close: frees the (scarce, on 3DS) socket immediately instead of lingering in TIME_WAIT. */
static void hard_close(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  (void)shutdown(fd, SHUT_RDWR);
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  (void)close(fd);
}

#define HTTP_IDLE_MS 10000u
#define WS_BASE NDP_WS_HEADER_MAX

static ndp_conn *rawc(ndp_server *s) { return &s->nc[0]; }
static ndp_conn *webc(ndp_server *s) { return &s->nc[1]; }

static void reset_session_state(ndp_conn *c) {
  c->out_len = c->out_off = 0;
  c->in_len = c->in_pos = 0;
  c->cur_active = 0;
  c->pend = NULL;
  c->pend_len = 0;
  c->ctl_len = c->ctl_off = 0;
  c->close_after_ctl = 0;
}

/* Public mirrors kept for the platforms' UI code (who is connected). */
static void sync_public(ndp_server *s) {
  s->client_fd = s->nc[0].fd;
  s->web_client_fd = s->nc[1].fd;
  snprintf(s->peer, sizeof s->peer, "%s", s->nc[0].fd >= 0 ? s->nc[0].peer : "");
  snprintf(s->web_peer, sizeof s->web_peer, "%s", s->nc[1].fd >= 0 ? s->nc[1].peer : "");
}

static void server_keys_changed(void *ctx, const ndp_keystore *keys) {
  ndp_server *s = (ndp_server *)ctx;
  slog(s, "[PAIR] paired computers: %d", keys->count);
  if (s->plat.keys_changed) s->plat.keys_changed(s->plat.ctx, keys);
}

void ndp_server_init(ndp_server *s, const ndp_server_platform *plat, const ndp_agent_config *cfg) {
  int i;
  memset(s, 0, sizeof *s);
  s->plat = *plat;
  s->agent_cfg = *cfg;
  if (cfg->policy) s->policy = *cfg->policy;
  else ndp_policy_init_default(&s->policy);
  s->agent_cfg.policy = &s->policy;
  s->agent_cfg.keys = &s->keys;
  s->agent_cfg.pairing = &s->pairing;
  s->agent_cfg.keys_changed = server_keys_changed;
  s->agent_cfg.keys_ctx = s;
  s->idle_timeout_ms = 120000;
  s->listen_fd = -1;
  s->client_fd = -1;
  s->web_listen_fd = -1;
  s->web_client_fd = -1;
  for (i = 0; i < 2; i++) {
    s->nc[i].fd = -1;
    ndp_agent_init(&s->nc[i].agent, &s->agent_cfg);
    ndp_decoder_init(&s->nc[i].dec, s->nc[i].rbuf, sizeof s->nc[i].rbuf, cfg->max_frame);
  }
  for (i = 0; i < NDP_WEB_HTTP_SLOTS; i++) s->http[i].fd = -1;
}

void ndp_server_set_policy(ndp_server *s, const ndp_policy *p) {
  int i;
  s->policy = *p;
  for (i = 0; i < 2; i++) ndp_agent_set_policy(&s->nc[i].agent, p);
}

void ndp_server_set_keys(ndp_server *s, const ndp_keystore *keys) { s->keys = *keys; }

void ndp_server_open_pairing(ndp_server *s, const uint8_t code[NDP_CODE_BYTES], uint32_t duration_ms) {
  memcpy(s->pairing.code, code, NDP_CODE_BYTES);
  s->pairing.active = 1;
  s->pairing.expires_ms = now(s) + duration_ms;
  slog(s, "[PAIR] window open for %lu s", (unsigned long)(duration_ms / 1000u));
}

void ndp_server_close_pairing(ndp_server *s) {
  if (!s->pairing.active) return;
  s->pairing.active = 0;
  memset(s->pairing.code, 0, sizeof s->pairing.code);
  slog(s, "[PAIR] window closed");
}

uint32_t ndp_server_pairing_remaining_ms(ndp_server *s) {
  uint64_t n;
  if (!s->pairing.active) return 0;
  n = now(s);
  return n >= s->pairing.expires_ms ? 0 : (uint32_t)(s->pairing.expires_ms - n);
}

void ndp_server_clear_keys(ndp_server *s) {
  uint8_t id[16];
  int had = s->keys.has_device_id;
  memcpy(id, s->keys.device_id, 16);
  ndp_keystore_clear(&s->keys);
  memcpy(s->keys.device_id, id, 16); /* the console keeps its identity, only the pairings go */
  s->keys.has_device_id = had;
  slog(s, "[PAIR] all pairings forgotten");
  if (s->plat.keys_changed) s->plat.keys_changed(s->plat.ctx, &s->keys);
}

void ndp_server_set_mode(ndp_server *s, ndp_mode mode) {
  int i;
  s->agent_cfg.mode = mode;
  for (i = 0; i < 2; i++) ndp_agent_set_mode(&s->nc[i].agent, mode);
}

static void close_conn(ndp_server *s, ndp_conn *c, const char *reason) {
  if (c->fd < 0) return;
  ndp_agent_close(&c->agent); /* releases an open directory / aborts a transfer */
  hard_close(c->fd);
  c->fd = -1;
  c->kind = NDP_CONN_NONE;
  s->connections_closed++;
  slog(s, "[CLOSE] %s%s %s", c == webc(s) ? "web " : "", c->peer, reason);
  reset_session_state(c);
  sync_public(s);
}

static void close_http(ndp_server *s, ndp_http_conn *h) {
  (void)s;
  if (h->fd >= 0) hard_close(h->fd);
  h->fd = -1;
}

void ndp_server_web_close(ndp_server *s) {
  int i;
  close_conn(s, webc(s), "web server closing");
  for (i = 0; i < NDP_WEB_HTTP_SLOTS; i++) close_http(s, &s->http[i]);
  if (s->web_listen_fd >= 0) {
    hard_close(s->web_listen_fd);
    s->web_listen_fd = -1;
    slog(s, "[WEB] page server stopped");
  }
}

void ndp_server_close(ndp_server *s) {
  close_conn(s, rawc(s), "server closing");
  ndp_server_web_close(s);
  if (s->listen_fd >= 0) {
    hard_close(s->listen_fd);
    s->listen_fd = -1;
  }
}

static int listen_on(uint32_t s_addr, uint16_t *port) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd, one = 1, e;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return errno ? -errno : -1;
  if (set_nonblocking(fd) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (*port != 0) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(*port);
  sa.sin_addr.s_addr = s_addr;
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (listen(fd, ACCEPT_BACKLOG) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0) *port = ntohs(sa.sin_port);
  return fd;
}

int ndp_server_listen(ndp_server *s, uint32_t s_addr, uint16_t port) {
  int fd;
  ndp_server_close(s);
  fd = listen_on(s_addr, &port);
  if (fd < 0) return fd;
  s->listen_fd = fd;
  s->listen_addr = s_addr;
  s->port = port;
  return 0;
}

void ndp_server_set_web(ndp_server *s, const ndp_web_assets *assets) { s->web_assets = assets; }
int ndp_server_web_enabled(const ndp_server *s) { return s->web_listen_fd >= 0; }

int ndp_server_web_listen(ndp_server *s, uint32_t s_addr, uint16_t port) {
  int fd;
  ndp_server_web_close(s);
  if (!s->web_assets) return -ENOSYS;
  fd = listen_on(s_addr, &port);
  if (fd < 0) return fd;
  s->web_listen_fd = fd;
  s->web_port = port;
  slog(s, "[WEB] page server on port %u", (unsigned)port);
  return 0;
}

/* Puts a finished NDP frame of `n` bytes (written at c->out + base) on the output queue, wrapped in a WebSocket
 * binary frame when the client is the page. */
static void queue_frame(ndp_conn *c, size_t n) {
  if (c->kind == NDP_CONN_WS) {
    uint8_t h[NDP_WS_HEADER_MAX];
    size_t hl = ndp_ws_frame_header(NDP_WS_OP_BINARY, n, h);
    memcpy(c->out + WS_BASE - hl, h, hl);
    c->out_off = WS_BASE - hl;
    c->out_len = WS_BASE + n;
  } else {
    c->out_off = 0;
    c->out_len = n;
  }
}
static uint8_t *frame_area(ndp_conn *c) { return c->kind == NDP_CONN_WS ? c->out + WS_BASE : c->out; }
static size_t frame_cap(ndp_conn *c) { return c->kind == NDP_CONN_WS ? sizeof c->out - WS_BASE : sizeof c->out; }

/* A pong / close frame (no payload beyond `pl`) waiting for the frame in flight. */
static void queue_ctl(ndp_conn *c, uint8_t opcode, const uint8_t *pl, size_t n, int then_close) {
  uint8_t h[NDP_WS_HEADER_MAX];
  size_t hl = ndp_ws_frame_header(opcode, n, h);
  if (c->ctl_len + hl + n > sizeof c->ctl) return; /* a flood of pings: drop the extra pongs */
  memcpy(c->ctl + c->ctl_len, h, hl);
  memcpy(c->ctl + c->ctl_len + hl, pl, n);
  c->ctl_len += hl + n;
  if (then_close) c->close_after_ctl = 1;
}

/* Sends what is queued: the frame in flight first, then pending control frames. 0 = all sent, 1 = would block,
 * 2 = all sent and the connection must now be closed, -1 = error. */
static int flush_out(ndp_server *s, ndp_conn *c) {
  for (;;) {
    const uint8_t *p;
    size_t n;
    ssize_t w;
    if (c->out_off < c->out_len) { p = c->out + c->out_off; n = c->out_len - c->out_off; }
    else if (c->ctl_off < c->ctl_len) { p = c->ctl + c->ctl_off; n = c->ctl_len - c->ctl_off; }
    else break;
    w = send(c->fd, p, n, 0);
    if (w > 0) {
      if (c->out_off < c->out_len) { c->out_off += (size_t)w; if (c->out_off >= c->out_len) c->out_len = c->out_off = 0; }
      else { c->ctl_off += (size_t)w; if (c->ctl_off >= c->ctl_len) c->ctl_len = c->ctl_off = 0; }
      c->last_activity_ms = now(s);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && would_block(errno)) return 1;
    slog(s, "[ERR] send: %s", strerror(errno));
    return -1;
  }
  return c->close_after_ctl ? 2 : 0;
}

/* Logs the outcome of the request whose response (and stream) has just been fully sent. */
static void finish_current(ndp_server *s, ndp_conn *c) {
  uint64_t ms;
  if (!c->cur_active) return;
  c->cur_active = 0;
  ms = now(s) - c->cur_t0;
  if ((c->cur_cmd == NDP_CMD_FS_READ || c->cur_cmd == NDP_CMD_FS_WRITE) && c->agent.last_transfer_bytes > 0) {
    unsigned long kbps = ms ? (unsigned long)(c->agent.last_transfer_bytes * 1000u / 1024u / ms) : 0;
    slog(s, "[OK %lu] %lu bytes %lu ms %lu KiB/s", (unsigned long)c->cur_id,
         (unsigned long)c->agent.last_transfer_bytes, (unsigned long)ms, kbps);
  } else {
    slog(s, "[OK %lu] %lu ms", (unsigned long)c->cur_id, (unsigned long)ms);
  }
}

/* Feeds bytes to the NDP decoder and answers complete frames while no response/stream is pending.
 * Returns the number of bytes consumed, or -1 to close the connection. */
static long feed_ndp(ndp_server *s, ndp_conn *c, const uint8_t *p, size_t n, int *changed) {
  size_t pos = 0;
  while (pos < n && c->out_len == 0 && !ndp_agent_streaming(&c->agent)) {
    size_t used = 0;
    int r = ndp_decoder_feed(&c->dec, p + pos, n - pos, &used);
    pos += used;
    if (r == NDP_DEC_ERROR) {
      slog(s, "[ERR] bad stream (%s)", ndp_status_name((uint16_t)c->dec.error));
      return -1;
    }
    if (r == NDP_DEC_FRAME) {
      const ndp_header *h = ndp_decoder_header(&c->dec);
      ndp_header rh;
      size_t rn;
      if (h->kind == NDP_KIND_REQ) {
        c->cur_active = 1;
        c->cur_id = h->request_id;
        c->cur_cmd = h->command;
        c->cur_t0 = now(s);
        s->requests++;
        *changed = 1;
        slog(s, "[REQ %lu] %s", (unsigned long)h->request_id, ndp_command_name(h->command));
      }
      rn = ndp_agent_handle_frame(&c->agent, h, ndp_decoder_payload(&c->dec), ndp_decoder_mac(&c->dec), frame_area(c), frame_cap(c));
      ndp_decoder_release(&c->dec);
      if (rn == NDP_CLOSE) { slog(s, "[ERR] authentication/MAC failure -- closing"); return -1; }
      if (rn == NDP_NO_REPLY) continue; /* e.g. a DATA frame of an upload */
      if (rn == 0) { slog(s, "[ERR %lu] response did not fit", (unsigned long)c->cur_id); return -1; }
      queue_frame(c, rn);
      if (ndp_header_decode(frame_area(c), &rh) == NDP_OK && rh.kind == NDP_KIND_ERR) {
        slog(s, "[ERR %lu] %s", (unsigned long)rh.request_id, ndp_status_name(rh.status));
        c->cur_active = 0;
      }
      { /* try to send right away: most responses fit the socket buffer */
        int f = flush_out(s, c);
        if (f < 0 || f == 2) return -1;
        if (f == 0 && !ndp_agent_busy(&c->agent)) finish_current(s, c);
      }
    }
  }
  return (long)pos;
}

/* Decodes buffered input. Raw NDP: the bytes are the NDP stream. WebSocket: the bytes are WebSocket frames whose
 * binary payloads are the NDP stream (and pings/closes are answered). Returns -1 to close. */
static int process_input(ndp_server *s, ndp_conn *c, int *changed) {
  if (c->kind == NDP_CONN_NDP) {
    while (c->in_pos < c->in_len && c->out_len == 0 && !ndp_agent_streaming(&c->agent)) {
      long u = feed_ndp(s, c, c->in + c->in_pos, c->in_len - c->in_pos, changed);
      if (u < 0) return -1;
      c->in_pos += (size_t)u;
      if (u == 0) break;
    }
  } else {
    for (;;) {
      if (c->pend_len > 0) {
        long u;
        if (c->out_len != 0 || ndp_agent_streaming(&c->agent)) break; /* busy answering: resume when it is out */
        u = feed_ndp(s, c, c->pend, c->pend_len, changed);
        if (u < 0) return -1;
        c->pend += u;
        c->pend_len -= (size_t)u;
        continue;
      }
      if (c->in_pos >= c->in_len) break;
      {
        size_t used = 0;
        ndp_ws_event ev = ndp_ws_dec_feed(&c->wsd, c->in + c->in_pos, c->in_len - c->in_pos, &used);
        c->in_pos += used;
        if (ev.kind == NDP_WS_DATA) { c->pend = ev.data; c->pend_len = ev.len; continue; }
        if (ev.kind == NDP_WS_PING) { queue_ctl(c, NDP_WS_OP_PONG, ev.data, ev.len, 0); continue; }
        if (ev.kind == NDP_WS_CLOSE || ev.kind == NDP_WS_ERROR) {
          uint8_t code[2];
          uint16_t cc = ev.kind == NDP_WS_CLOSE ? (ev.code == 1005 ? 1000 : ev.code) : ev.code;
          code[0] = (uint8_t)(cc >> 8); code[1] = (uint8_t)cc;
          if (ev.kind == NDP_WS_ERROR) slog(s, "[ERR] web: bad WebSocket frame (%u)", (unsigned)ev.code);
          queue_ctl(c, NDP_WS_OP_CLOSE, code, 2, 1);
          c->in_pos = c->in_len; /* nothing after a close is read */
          break;
        }
        if (used == 0) break;
      }
    }
  }
  if (c->in_pos >= c->in_len && c->pend_len == 0) c->in_pos = c->in_len = 0;
  if (c->ctl_len > 0 || c->close_after_ctl) { /* pongs / the closing frame */
    int f = flush_out(s, c);
    if (f < 0 || f == 2) return -1;
  }
  return 0;
}

/* Sends queued bytes and produces up to STREAM_FRAMES_PER_STEP stream frames. Returns -1 to close. */
static int pump_output(ndp_server *s, ndp_conn *c, int *changed) {
  int budget = STREAM_FRAMES_PER_STEP;
  for (;;) {
    if (c->out_len || c->ctl_len) {
      int f = flush_out(s, c);
      if (f < 0 || f == 2) return -1;
      if (f > 0) return 0; /* socket full: continue on the next POLLOUT */
    }
    if (ndp_agent_streaming(&c->agent) && budget-- > 0) {
      size_t n = ndp_agent_next_frame(&c->agent, frame_area(c), frame_cap(c));
      if (n == 0) { slog(s, "[ERR] stream frame did not fit"); return -1; }
      queue_frame(c, n);
      continue;
    }
    break;
  }
  if (c->out_len == 0 && !ndp_agent_busy(&c->agent) && c->cur_active) {
    finish_current(s, c);
    *changed = 1;
  }
  return 0;
}

static void fmt_peer(char *dst, size_t cap, const struct sockaddr_in *sa) {
  const unsigned char *b = (const unsigned char *)&sa->sin_addr.s_addr;
  snprintf(dst, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void adopt(ndp_server *s, ndp_conn *c, int fd, ndp_conn_kind kind, const char *peer) {
  ndp_agent_init(&c->agent, &s->agent_cfg);
  ndp_decoder_init(&c->dec, c->rbuf, sizeof c->rbuf, s->agent_cfg.max_frame);
  reset_session_state(c);
  ndp_ws_dec_init(&c->wsd);
  c->fd = fd;
  c->kind = kind;
  snprintf(c->peer, sizeof c->peer, "%s", peer);
  c->last_activity_ms = now(s);
  s->connections_opened++;
  slog(s, "[CONNECT] %s%s", kind == NDP_CONN_WS ? "web " : "", peer);
  sync_public(s);
}

static void accept_client(ndp_server *s, int *changed) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd = accept(s->listen_fd, (struct sockaddr *)&sa, &sl);
  int buf = 32768;
  char peer[24];
  if (fd < 0) return; /* EWOULDBLOCK / ECONNABORTED: nothing to do */
  if (rawc(s)->fd >= 0) close_conn(s, rawc(s), "replaced by a new connection");
  if (set_nonblocking(fd) < 0) { slog(s, "[ERR] fcntl: %s", strerror(errno)); close(fd); return; }
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  fmt_peer(peer, sizeof peer, &sa);
  adopt(s, rawc(s), fd, NDP_CONN_NDP, peer);
  *changed = 1;
}

/* Reads what the client sent (several recv rounds while the buffer keeps filling: uploads are throughput-bound)
 * and processes it. Closes the client on errors. */
static void on_readable(ndp_server *s, ndp_conn *c, short ev, int *changed) {
  int rounds = 0;
  for (;;) {
    ssize_t r = recv(c->fd, c->in + c->in_len, sizeof c->in - c->in_len, 0);
    if (r == 0) { close_conn(s, c, "peer closed"); *changed = 1; return; }
    if (r < 0 && !would_block(errno) && errno != EINTR) {
      slog(s, "[ERR] recv: %s", strerror(errno));
      close_conn(s, c, "read error");
      *changed = 1;
      return;
    }
    if (r < 0) {
      if (ev & (POLLERR | POLLHUP)) { close_conn(s, c, "connection error"); *changed = 1; }
      return;
    }
    c->in_len += (size_t)r;
    c->last_activity_ms = now(s);
    if (process_input(s, c, changed) < 0) { close_conn(s, c, "protocol/send error"); *changed = 1; return; }
    if (c->in_len == 0) c->in_pos = 0;
    if ((size_t)r < sizeof c->in || c->fd < 0 || c->out_len || c->pend_len || ndp_agent_streaming(&c->agent) || ++rounds >= 4) return;
  }
}

/* ------------------------------------------------------------------------------------------ the web page (HTTP) */
static void http_reply(ndp_http_conn *h, int code, const char *reason, const char *ctype, const char *extra,
                       const uint8_t *body, size_t body_len, int head_only) {
  int n = snprintf(h->resp, sizeof h->resp,
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n%s"
                   "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                   "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                   "img-src blob: data:; connect-src ws:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
                   "Connection: close\r\n\r\n",
                   code, reason, ctype, (unsigned long)body_len, extra ? extra : "");
  h->resp_len = n > 0 && (size_t)n < sizeof h->resp ? (size_t)n : 0;
  h->resp_off = 0;
  h->body = head_only ? NULL : body;
  h->body_len = head_only ? 0 : body_len;
  h->body_off = 0;
  h->responding = 1;
}

static void http_error(ndp_http_conn *h, int code, const char *reason) {
  static const uint8_t none[1] = {0};
  http_reply(h, code, reason, "text/plain; charset=utf-8", NULL, none, 0, 0);
}

/* The 101 answer to a WebSocket upgrade, and the hand-over of the socket to the WebSocket client slot. */
static int upgrade_to_ws(ndp_server *s, ndp_http_conn *h, const ndp_http_req *r, int *changed) {
  char acc[29], resp[200];
  int n;
  if (ndp_ws_accept_key(r->ws_key, acc) != 0) { http_error(h, 400, "Bad Request"); return 0; }
  n = snprintf(resp, sizeof resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
  if (n <= 0 || (size_t)n >= sizeof resp) { http_error(h, 500, "Internal Server Error"); return 0; }
  if (send(h->fd, resp, (size_t)n, 0) != n) { close_http(s, h); return 1; } /* a fresh socket takes 130 bytes at once */
  if (webc(s)->fd >= 0) close_conn(s, webc(s), "replaced by a new page connection");
  {
    int fd = h->fd;
    char peer[24];
    snprintf(peer, sizeof peer, "%s", h->peer);
    h->fd = -1;
    if (h->head_len > r->head_len) { /* bytes after the head (a client that did not wait for the 101): not accepted */
      hard_close(fd);
      return 1;
    }
    adopt(s, webc(s), fd, NDP_CONN_WS, peer);
    *changed = 1;
  }
  return 1;
}

static void http_route(ndp_server *s, ndp_http_conn *h, const ndp_http_req *r, const char *local_ip, int *changed) {
  static const uint8_t none[1] = {0};
  int head_only = r->method == NDP_HTTP_HEAD;
  if (r->method != NDP_HTTP_GET && r->method != NDP_HTTP_HEAD) { http_error(h, 405, "Method Not Allowed"); return; }
  if (!ndp_http_host_ok(r, local_ip)) { http_error(h, 403, "Forbidden"); return; } /* DNS rebinding */
  if (strcmp(r->path, "/ws") == 0) {
    if (!r->upgrade_websocket) { http_reply(h, 426, "Upgrade Required", "text/plain; charset=utf-8", "Upgrade: websocket\r\n", none, 0, 0); return; }
    if (r->method != NDP_HTTP_GET || r->ws_version != 13) { http_reply(h, 400, "Bad Request", "text/plain; charset=utf-8", "Sec-WebSocket-Version: 13\r\n", none, 0, 0); return; }
    if (!ndp_http_origin_ok(r)) { slog(s, "[WEB] refused a cross-origin WebSocket from %s (Origin %.40s)", h->peer, r->origin); http_error(h, 403, "Forbidden"); return; }
    (void)upgrade_to_ws(s, h, r, changed);
    return;
  }
  if (strcmp(r->path, "/") == 0 || strcmp(r->path, "/index.html") == 0) {
    char extra[96];
    snprintf(extra, sizeof extra, "Content-Encoding: gzip\r\nETag: \"%s\"\r\n", s->web_assets && s->web_assets->version ? s->web_assets->version : "0");
    http_reply(h, 200, "OK", "text/html; charset=utf-8", extra, s->web_assets->index_gz, s->web_assets->index_gz_len, head_only);
    s->web_requests++;
    slog(s, "[WEB] page served to %s", h->peer);
    *changed = 1;
    return;
  }
  if (strcmp(r->path, "/favicon.ico") == 0) { http_reply(h, 204, "No Content", "image/x-icon", NULL, none, 0, 1); return; }
  http_error(h, 404, "Not Found");
}

static void http_on_readable(ndp_server *s, ndp_http_conn *h, int *changed) {
  ssize_t r = recv(h->fd, h->head + h->head_len, sizeof h->head - h->head_len, 0);
  ndp_http_req req;
  int pr;
  char local_ip[24];
  struct sockaddr_in la;
  socklen_t ll = sizeof la;
  if (r == 0) { close_http(s, h); return; }
  if (r < 0) { if (!would_block(errno) && errno != EINTR) close_http(s, h); return; }
  h->head_len += (size_t)r;
  pr = ndp_http_parse(h->head, h->head_len, &req);
  if (pr == NDP_HTTP_NEED_MORE) return;
  if (pr == NDP_HTTP_OK) {
    if (getsockname(h->fd, (struct sockaddr *)&la, &ll) == 0) fmt_peer(local_ip, sizeof local_ip, &la);
    else local_ip[0] = '\0';
    http_route(s, h, &req, local_ip, changed);
    return;
  }
  slog(s, "[WEB] bad request from %s (%d)", h->peer, pr);
  if (pr == NDP_HTTP_TOO_LARGE) http_error(h, 431, "Request Header Fields Too Large");
  else if (pr == NDP_HTTP_BODY) http_error(h, 413, "Payload Too Large");
  else if (pr == NDP_HTTP_VERSION) http_error(h, 505, "HTTP Version Not Supported");
  else http_error(h, 400, "Bad Request");
}

static void http_on_writable(ndp_server *s, ndp_http_conn *h) {
  while (h->resp_off < h->resp_len || h->body_off < h->body_len) {
    const uint8_t *p;
    size_t n;
    ssize_t w;
    if (h->resp_off < h->resp_len) { p = (const uint8_t *)h->resp + h->resp_off; n = h->resp_len - h->resp_off; }
    else { p = h->body + h->body_off; n = h->body_len - h->body_off; }
    w = send(h->fd, p, n, 0);
    if (w > 0) { if (h->resp_off < h->resp_len) h->resp_off += (size_t)w; else h->body_off += (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && would_block(errno)) return;
    close_http(s, h);
    return;
  }
  close_http(s, h); /* Connection: close */
}

static void accept_web(ndp_server *s) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd = accept(s->web_listen_fd, (struct sockaddr *)&sa, &sl), i;
  int buf = 32768;
  if (fd < 0) return;
  for (i = 0; i < NDP_WEB_HTTP_SLOTS && s->http[i].fd >= 0; i++) {}
  if (i == NDP_WEB_HTTP_SLOTS || set_nonblocking(fd) < 0) { hard_close(fd); return; } /* all slots busy: the browser retries */
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  memset(&s->http[i], 0, sizeof s->http[i]);
  s->http[i].fd = fd;
  s->http[i].started_ms = now(s);
  fmt_peer(s->http[i].peer, sizeof s->http[i].peer, &sa);
}

/* ------------------------------------------------------------------------------------------------ the main step */
typedef struct { int kind; int idx; } slot_t; /* kind: 0 listen, 1 web listen, 2 conn, 3 http */

int ndp_server_step(ndp_server *s, int timeout_ms) {
  struct pollfd pfd[4 + NDP_WEB_HTTP_SLOTS];
  slot_t slot[4 + NDP_WEB_HTTP_SLOTS];
  nfds_t n = 0, k;
  int rc, changed = 0, leftover = 0, i;
  short want_out[2] = {0, 0};

  if (s->listen_fd < 0) return -1;

  for (i = 0; i < 2; i++) {
    ndp_conn *c = &s->nc[i];
    if (c->fd < 0) continue;
    if ((c->in_pos < c->in_len || c->pend_len > 0) && c->out_len == 0 && !ndp_agent_streaming(&c->agent)) {
      if (process_input(s, c, &changed) < 0) { close_conn(s, c, "protocol/send error"); changed = 1; continue; }
    }
    want_out[i] = c->out_len > 0 || c->ctl_len > 0 || ndp_agent_streaming(&c->agent);
    if (!want_out[i] && (c->in_pos < c->in_len || c->pend_len > 0)) leftover = 1;
  }

  pfd[n].fd = s->listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; slot[n].kind = 0; slot[n].idx = 0; n++;
  if (s->web_listen_fd >= 0) { pfd[n].fd = s->web_listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; slot[n].kind = 1; slot[n].idx = 0; n++; }
  for (i = 0; i < 2; i++) {
    if (s->nc[i].fd < 0) continue;
    pfd[n].fd = s->nc[i].fd;
    pfd[n].events = (short)(want_out[i] ? POLLOUT : POLLIN);
    pfd[n].revents = 0;
    slot[n].kind = 2; slot[n].idx = i; n++;
  }
  for (i = 0; i < NDP_WEB_HTTP_SLOTS; i++) {
    if (s->http[i].fd < 0) continue;
    pfd[n].fd = s->http[i].fd;
    pfd[n].events = s->http[i].responding ? POLLOUT : POLLIN;
    pfd[n].revents = 0;
    slot[n].kind = 3; slot[n].idx = i; n++;
  }

  rc = poll(pfd, n, leftover ? 0 : timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) return changed;
    slog(s, "[ERR] poll: %s", strerror(errno));
    return -1;
  }
  if (rc > 0) {
    for (k = 0; k < n; k++) {
      short ev = pfd[k].revents;
      if (!ev) continue;
      if (slot[k].kind == 0) {
        if (ev & (POLLERR | POLLHUP | POLLNVAL)) { slog(s, "[ERR] listener failed"); return -1; }
        if (ev & POLLIN) accept_client(s, &changed);
      } else if (slot[k].kind == 1) {
        if (ev & (POLLERR | POLLHUP | POLLNVAL)) { slog(s, "[ERR] web listener failed"); ndp_server_web_close(s); changed = 1; }
        else if (ev & POLLIN) accept_web(s);
      } else if (slot[k].kind == 2) {
        ndp_conn *c = &s->nc[slot[k].idx];
        /* the client of this slot may have been replaced by an accept in this same round */
        if (c->fd != pfd[k].fd) continue;
        if (ev & POLLNVAL) { close_conn(s, c, "invalid socket"); changed = 1; }
        else if (want_out[slot[k].idx]) {
          if (ev & (POLLERR | POLLHUP)) { close_conn(s, c, "connection error"); changed = 1; }
          else if (ev & POLLOUT) {
            if (pump_output(s, c, &changed) < 0) { close_conn(s, c, "send error"); changed = 1; }
          }
        } else {
          on_readable(s, c, ev, &changed);
        }
      } else {
        ndp_http_conn *h = &s->http[slot[k].idx];
        if (h->fd != pfd[k].fd) continue;
        if (ev & (POLLERR | POLLHUP | POLLNVAL)) { if (!(ev & POLLIN) || h->responding) { close_http(s, h); continue; } }
        if (h->responding) http_on_writable(s, h);
        else if (ev & POLLIN) http_on_readable(s, h, &changed);
      }
    }
  }
  for (i = 0; i < 2; i++) {
    ndp_conn *c = &s->nc[i];
    if (c->fd >= 0 && now(s) - c->last_activity_ms > s->idle_timeout_ms) { close_conn(s, c, "idle timeout"); changed = 1; }
  }
  for (i = 0; i < NDP_WEB_HTTP_SLOTS; i++)
    if (s->http[i].fd >= 0 && now(s) - s->http[i].started_ms > HTTP_IDLE_MS) close_http(s, &s->http[i]);
  if (s->pairing.active && now(s) >= s->pairing.expires_ms) {
    ndp_server_close_pairing(s);
    changed = 1;
  }
  sync_public(s);
  return changed;
}
