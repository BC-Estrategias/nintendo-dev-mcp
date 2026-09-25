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

static void reset_session_state(ndp_server *s) {
  s->out_len = s->out_off = 0;
  s->in_len = s->in_pos = 0;
  s->cur_active = 0;
}

static void server_keys_changed(void *ctx, const ndp_keystore *keys) {
  ndp_server *s = (ndp_server *)ctx;
  slog(s, "[PAIR] paired computers: %d", keys->count);
  if (s->plat.keys_changed) s->plat.keys_changed(s->plat.ctx, keys);
}

void ndp_server_init(ndp_server *s, const ndp_server_platform *plat, const ndp_agent_config *cfg) {
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
  ndp_agent_init(&s->agent, cfg);
  ndp_decoder_init(&s->dec, s->rbuf, sizeof s->rbuf, cfg->max_frame);
}

void ndp_server_set_policy(ndp_server *s, const ndp_policy *p) {
  s->policy = *p;
  ndp_agent_set_policy(&s->agent, p);
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
  s->agent_cfg.mode = mode;
  ndp_agent_set_mode(&s->agent, mode);
}

static void close_client(ndp_server *s, const char *reason) {
  if (s->client_fd < 0) return;
  ndp_agent_close(&s->agent); /* releases an open directory / aborts a transfer */
  hard_close(s->client_fd);
  s->client_fd = -1;
  s->connections_closed++;
  slog(s, "[CLOSE] %s %s", s->peer, reason);
  s->peer[0] = '\0';
  reset_session_state(s);
}

void ndp_server_close(ndp_server *s) {
  close_client(s, "server closing");
  if (s->listen_fd >= 0) {
    hard_close(s->listen_fd);
    s->listen_fd = -1;
  }
}

int ndp_server_listen(ndp_server *s, uint32_t s_addr, uint16_t port) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd, one = 1, e;
  ndp_server_close(s);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return errno ? -errno : -1;
  if (set_nonblocking(fd) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (port != 0) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = s_addr;
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (listen(fd, ACCEPT_BACKLOG) < 0) { e = errno; close(fd); return e ? -e : -1; }
  if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0) port = ntohs(sa.sin_port);
  s->listen_fd = fd;
  s->listen_addr = s_addr;
  s->port = port;
  return 0;
}

/* Sends as much of the queued frame as the socket accepts. 0 = fully sent, 1 = would block, -1 = error. */
static int flush_out(ndp_server *s) {
  while (s->out_off < s->out_len) {
    ssize_t w = send(s->client_fd, s->out + s->out_off, s->out_len - s->out_off, 0);
    if (w > 0) { s->out_off += (size_t)w; s->last_activity_ms = now(s); continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && would_block(errno)) return 1;
    slog(s, "[ERR] send: %s", strerror(errno));
    return -1;
  }
  s->out_len = s->out_off = 0;
  return 0;
}

/* Logs the outcome of the request whose response (and stream) has just been fully sent. */
static void finish_current(ndp_server *s) {
  uint64_t ms;
  if (!s->cur_active) return;
  s->cur_active = 0;
  ms = now(s) - s->cur_t0;
  if ((s->cur_cmd == NDP_CMD_FS_READ || s->cur_cmd == NDP_CMD_FS_WRITE) && s->agent.last_transfer_bytes > 0) {
    unsigned long kbps = ms ? (unsigned long)(s->agent.last_transfer_bytes * 1000u / 1024u / ms) : 0;
    slog(s, "[OK %lu] %lu bytes %lu ms %lu KiB/s", (unsigned long)s->cur_id,
         (unsigned long)s->agent.last_transfer_bytes, (unsigned long)ms, kbps);
  } else {
    slog(s, "[OK %lu] %lu ms", (unsigned long)s->cur_id, (unsigned long)ms);
  }
}

/* Decodes and answers buffered input while no response/stream is pending. Returns -1 to close. */
static int process_input(ndp_server *s, int *changed) {
  while (s->in_pos < s->in_len && s->out_len == 0 && !ndp_agent_streaming(&s->agent)) {
    size_t used = 0;
    int r = ndp_decoder_feed(&s->dec, s->in + s->in_pos, s->in_len - s->in_pos, &used);
    s->in_pos += used;
    if (r == NDP_DEC_ERROR) {
      slog(s, "[ERR] bad stream (%s)", ndp_status_name((uint16_t)s->dec.error));
      return -1;
    }
    if (r == NDP_DEC_FRAME) {
      const ndp_header *h = ndp_decoder_header(&s->dec);
      ndp_header rh;
      size_t n;
      if (h->kind == NDP_KIND_REQ) {
        s->cur_active = 1;
        s->cur_id = h->request_id;
        s->cur_cmd = h->command;
        s->cur_t0 = now(s);
        s->requests++;
        *changed = 1;
        slog(s, "[REQ %lu] %s", (unsigned long)h->request_id, ndp_command_name(h->command));
      }
      n = ndp_agent_handle_frame(&s->agent, h, ndp_decoder_payload(&s->dec), ndp_decoder_mac(&s->dec), s->out, sizeof s->out);
      ndp_decoder_release(&s->dec);
      if (n == NDP_CLOSE) { slog(s, "[ERR] authentication/MAC failure -- closing"); return -1; }
      if (n == NDP_NO_REPLY) continue; /* e.g. a DATA frame of an upload */
      if (n == 0) { slog(s, "[ERR %lu] response did not fit", (unsigned long)s->cur_id); return -1; }
      s->out_len = n;
      s->out_off = 0;
      if (ndp_header_decode(s->out, &rh) == NDP_OK && rh.kind == NDP_KIND_ERR) {
        slog(s, "[ERR %lu] %s", (unsigned long)rh.request_id, ndp_status_name(rh.status));
        s->cur_active = 0;
      }
      { /* try to send right away: most responses fit the socket buffer */
        int f = flush_out(s);
        if (f < 0) return -1;
        if (f == 0 && !ndp_agent_busy(&s->agent)) finish_current(s);
      }
    }
  }
  if (s->in_pos >= s->in_len) s->in_pos = s->in_len = 0;
  return 0;
}

/* Sends queued bytes and produces up to STREAM_FRAMES_PER_STEP stream frames. Returns -1 to close. */
static int pump_output(ndp_server *s, int *changed) {
  int budget = STREAM_FRAMES_PER_STEP;
  for (;;) {
    if (s->out_len) {
      int f = flush_out(s);
      if (f < 0) return -1;
      if (f > 0) return 0; /* socket full: continue on the next POLLOUT */
    }
    if (ndp_agent_streaming(&s->agent) && budget-- > 0) {
      size_t n = ndp_agent_next_frame(&s->agent, s->out, sizeof s->out);
      if (n == 0) { slog(s, "[ERR] stream frame did not fit"); return -1; }
      s->out_len = n;
      s->out_off = 0;
      continue;
    }
    break;
  }
  if (s->out_len == 0 && !ndp_agent_busy(&s->agent) && s->cur_active) {
    finish_current(s);
    *changed = 1;
  }
  return 0;
}

static void accept_client(ndp_server *s, int *changed) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd = accept(s->listen_fd, (struct sockaddr *)&sa, &sl);
  int buf = 32768;
  if (fd < 0) return; /* EWOULDBLOCK / ECONNABORTED: nothing to do */
  if (s->client_fd >= 0) close_client(s, "replaced by a new connection");
  if (set_nonblocking(fd) < 0) { slog(s, "[ERR] fcntl: %s", strerror(errno)); close(fd); return; }
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  s->client_fd = fd;
  {
    const unsigned char *b = (const unsigned char *)&sa.sin_addr.s_addr;
    snprintf(s->peer, sizeof s->peer, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  }
  ndp_agent_init(&s->agent, &s->agent_cfg);
  ndp_decoder_init(&s->dec, s->rbuf, sizeof s->rbuf, s->agent_cfg.max_frame);
  reset_session_state(s);
  s->last_activity_ms = now(s);
  s->connections_opened++;
  slog(s, "[CONNECT] %s", s->peer);
  *changed = 1;
}

/* Reads what the client sent (several recv rounds while the buffer keeps filling: uploads are
 * throughput-bound) and processes it. Closes the client on errors. */
static void on_readable(ndp_server *s, short ev, int *changed) {
  int rounds = 0;
  for (;;) {
    ssize_t r = recv(s->client_fd, s->in, sizeof s->in, 0);
    if (r == 0) { close_client(s, "peer closed"); *changed = 1; return; }
    if (r < 0 && !would_block(errno) && errno != EINTR) {
      slog(s, "[ERR] recv: %s", strerror(errno));
      close_client(s, "read error");
      *changed = 1;
      return;
    }
    if (r < 0) {
      if (ev & (POLLERR | POLLHUP)) { close_client(s, "connection error"); *changed = 1; }
      return;
    }
    s->in_len = (size_t)r;
    s->in_pos = 0;
    s->last_activity_ms = now(s);
    if (process_input(s, changed) < 0) { close_client(s, "protocol/send error"); *changed = 1; return; }
    if ((size_t)r < sizeof s->in || s->client_fd < 0 || s->out_len || ndp_agent_streaming(&s->agent) || ++rounds >= 4) return;
  }
}

int ndp_server_step(ndp_server *s, int timeout_ms) {
  struct pollfd pfd[2];
  nfds_t n = 0;
  int li, ci = -1, rc, changed = 0, replaced = 0, want_out = 0, leftover = 0;

  if (s->listen_fd < 0) return -1;

  if (s->client_fd >= 0 && s->in_pos < s->in_len && s->out_len == 0 && !ndp_agent_streaming(&s->agent)) {
    if (process_input(s, &changed) < 0) { close_client(s, "protocol/send error"); changed = 1; }
  }
  if (s->client_fd >= 0) {
    want_out = s->out_len > 0 || ndp_agent_streaming(&s->agent);
    leftover = !want_out && s->in_pos < s->in_len;
  }

  pfd[n].fd = s->listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; li = (int)n++;
  if (s->client_fd >= 0) {
    pfd[n].fd = s->client_fd;
    pfd[n].events = (short)(want_out ? POLLOUT : POLLIN);
    pfd[n].revents = 0;
    ci = (int)n++;
  }

  rc = poll(pfd, n, leftover ? 0 : timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) return changed;
    slog(s, "[ERR] poll: %s", strerror(errno));
    return -1;
  }
  if (rc > 0) {
    if (pfd[li].revents & (POLLERR | POLLHUP | POLLNVAL)) { slog(s, "[ERR] listener failed"); return -1; }
    if (pfd[li].revents & POLLIN) {
      int had = s->client_fd;
      accept_client(s, &changed);
      replaced = (had >= 0 && s->client_fd != had) || (had < 0 && s->client_fd >= 0);
    }
    if (ci >= 0 && !replaced && s->client_fd >= 0 && pfd[ci].revents) {
      short ev = pfd[ci].revents;
      if (ev & POLLNVAL) { close_client(s, "invalid socket"); changed = 1; }
      else if (want_out) {
        if (ev & (POLLERR | POLLHUP)) { close_client(s, "connection error"); changed = 1; }
        else if (ev & POLLOUT) {
          if (pump_output(s, &changed) < 0) { close_client(s, "send error"); changed = 1; }
        }
      } else {
        on_readable(s, ev, &changed);
      }
    }
  }
  if (s->client_fd >= 0 && now(s) - s->last_activity_ms > s->idle_timeout_ms) {
    close_client(s, "idle timeout");
    changed = 1;
  }
  if (s->pairing.active && now(s) >= s->pairing.expires_ms) {
    ndp_server_close_pairing(s);
    changed = 1;
  }
  return changed;
}
