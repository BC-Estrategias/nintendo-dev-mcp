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
#define SEND_WAIT_MS 1000
#define SEND_TOTAL_MS 5000
#define RECV_CHUNK 4096

static void slog(ndp_server *s, const char *fmt, ...) {
  char line[160];
  va_list ap;
  if (!s->plat.log) return;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  s->plat.log(s->plat.ctx, line);
}

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

void ndp_server_init(ndp_server *s, const ndp_server_platform *plat, const ndp_agent_config *cfg) {
  memset(s, 0, sizeof *s);
  s->plat = *plat;
  s->agent_cfg = *cfg;
  s->idle_timeout_ms = 120000;
  s->listen_fd = -1;
  s->client_fd = -1;
  ndp_agent_init(&s->agent, cfg);
  ndp_decoder_init(&s->dec, s->rbuf, sizeof s->rbuf, cfg->max_frame);
}

static void close_client(ndp_server *s, const char *reason) {
  if (s->client_fd < 0) return;
  hard_close(s->client_fd);
  s->client_fd = -1;
  s->connections_closed++;
  slog(s, "[CLOSE] %s %s", s->peer, reason);
  s->peer[0] = '\0';
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

/* Sends everything, waiting for POLLOUT on EWOULDBLOCK. Returns 0 or -1 (connection is then closed by caller). */
static int send_all(ndp_server *s, const uint8_t *p, size_t n) {
  uint64_t start = s->plat.now_ms(s->plat.ctx);
  while (n) {
    ssize_t w = send(s->client_fd, p, n, 0);
    if (w > 0) { p += w; n -= (size_t)w; continue; }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && would_block(errno)) {
      struct pollfd pfd;
      if (s->plat.now_ms(s->plat.ctx) - start > SEND_TOTAL_MS) { slog(s, "[ERR] send timed out"); return -1; }
      pfd.fd = s->client_fd; pfd.events = POLLOUT; pfd.revents = 0;
      if (poll(&pfd, 1, SEND_WAIT_MS) < 0 && errno != EINTR) return -1;
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
      continue;
    }
    slog(s, "[ERR] send: %s", strerror(errno));
    return -1;
  }
  return 0;
}

static void accept_client(ndp_server *s, int *changed) {
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  int fd = accept(s->listen_fd, (struct sockaddr *)&sa, &sl);
  int lg_buf = 32768;
  if (fd < 0) return; /* EWOULDBLOCK / ECONNABORTED: nothing to do */
  if (s->client_fd >= 0) close_client(s, "replaced by a new connection");
  if (set_nonblocking(fd) < 0) { slog(s, "[ERR] fcntl: %s", strerror(errno)); close(fd); return; }
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &lg_buf, sizeof lg_buf);
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &lg_buf, sizeof lg_buf);
  s->client_fd = fd;
  {
    const unsigned char *b = (const unsigned char *)&sa.sin_addr.s_addr;
    snprintf(s->peer, sizeof s->peer, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  }
  ndp_agent_init(&s->agent, &s->agent_cfg);
  ndp_decoder_init(&s->dec, s->rbuf, sizeof s->rbuf, s->agent_cfg.max_frame);
  s->last_activity_ms = s->plat.now_ms(s->plat.ctx);
  s->connections_opened++;
  slog(s, "[CONNECT] %s", s->peer);
  *changed = 1;
}

/* Handles the bytes just read. Returns 0 to keep the connection, -1 to close it. */
static int process_bytes(ndp_server *s, const uint8_t *data, size_t len, int *changed) {
  size_t pos = 0;
  while (pos < len) {
    size_t used = 0;
    int r = ndp_decoder_feed(&s->dec, data + pos, len - pos, &used);
    pos += used;
    if (r == NDP_DEC_ERROR) {
      slog(s, "[ERR] bad stream (%s)", ndp_status_name((uint16_t)s->dec.error));
      return -1;
    }
    if (r == NDP_DEC_FRAME) {
      const ndp_header *h = ndp_decoder_header(&s->dec);
      uint32_t id = h->request_id;
      uint64_t t0 = s->plat.now_ms(s->plat.ctx);
      size_t n;
      ndp_header rh;
      slog(s, "[REQ %lu] %s", (unsigned long)id, ndp_command_name(h->command));
      n = ndp_agent_handle(&s->agent, h, ndp_decoder_payload(&s->dec), s->out, sizeof s->out);
      ndp_decoder_release(&s->dec);
      s->requests++;
      *changed = 1;
      if (n == 0) { slog(s, "[ERR %lu] response did not fit", (unsigned long)id); return -1; }
      if (send_all(s, s->out, n) != 0) return -1;
      if (ndp_header_decode(s->out, &rh) == NDP_OK && rh.kind == NDP_KIND_ERR)
        slog(s, "[ERR %lu] %s", (unsigned long)id, ndp_status_name(rh.status));
      else
        slog(s, "[OK %lu] %lu ms", (unsigned long)id, (unsigned long)(s->plat.now_ms(s->plat.ctx) - t0));
    }
  }
  return 0;
}

int ndp_server_step(ndp_server *s, int timeout_ms) {
  struct pollfd pfd[2];
  nfds_t n = 0;
  int li, ci = -1, rc, changed = 0, replaced = 0;
  uint64_t now;

  if (s->listen_fd < 0) return -1;
  pfd[n].fd = s->listen_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; li = (int)n++;
  if (s->client_fd >= 0) { pfd[n].fd = s->client_fd; pfd[n].events = POLLIN; pfd[n].revents = 0; ci = (int)n++; }

  rc = poll(pfd, n, timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) return 0;
    slog(s, "[ERR] poll: %s", strerror(errno));
    return -1;
  }
  if (rc > 0) {
    if (pfd[li].revents & (POLLERR | POLLHUP | POLLNVAL)) { slog(s, "[ERR] listener failed"); return -1; }
    if (pfd[li].revents & POLLIN) {
      int had = s->client_fd;
      accept_client(s, &changed);
      replaced = (had >= 0 && s->client_fd != had);
    }
    if (ci >= 0 && !replaced && s->client_fd >= 0 && pfd[ci].revents) {
      uint8_t buf[RECV_CHUNK];
      ssize_t r = 0;
      if (pfd[ci].revents & POLLNVAL) { close_client(s, "invalid socket"); changed = 1; }
      else {
        r = recv(s->client_fd, buf, sizeof buf, 0);
        if (r == 0) { close_client(s, "peer closed"); changed = 1; }
        else if (r < 0 && !would_block(errno) && errno != EINTR) {
          slog(s, "[ERR] recv: %s", strerror(errno));
          close_client(s, "read error");
          changed = 1;
        } else if (r > 0) {
          s->last_activity_ms = s->plat.now_ms(s->plat.ctx);
          if (process_bytes(s, buf, (size_t)r, &changed) != 0) { close_client(s, "protocol/send error"); changed = 1; }
        } else if (pfd[ci].revents & (POLLERR | POLLHUP)) {
          close_client(s, "connection error");
          changed = 1;
        }
      }
    }
  }
  now = s->plat.now_ms(s->plat.ctx);
  if (s->client_fd >= 0 && now - s->last_activity_ms > s->idle_timeout_ms) {
    close_client(s, "idle timeout");
    changed = 1;
  }
  return changed;
}
