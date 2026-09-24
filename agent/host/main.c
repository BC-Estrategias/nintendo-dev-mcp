/* Host agent: the platform-neutral NDP core behind a POSIX TCP server.
 * Used by the Bridge integration tests and for development without a console.
 * Usage: ndp-host-agent [--bind ADDR] [--port N] [--mode READ_ONLY|DEVELOPMENT|FULL] [--once] [-v] */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ndp/ndp_agent.h"

#define AGENT_VERSION "0.1.0"
#define IDLE_TIMEOUT_MS 30000

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void random_bytes(void *ctx, uint8_t *out, size_t n) {
  int fd = open("/dev/urandom", O_RDONLY);
  size_t got = 0;
  (void)ctx;
  if (fd >= 0) {
    while (got < n) {
      ssize_t r = read(fd, out + got, n - got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    close(fd);
  }
  while (got < n) out[got++] = (uint8_t)rand(); /* not secure; only reached if /dev/urandom fails */
}

static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    ssize_t w = send(fd, p, n, 0);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static const char *cmd_name(uint16_t c) {
  switch (c) {
    case NDP_CMD_HELLO: return "HELLO";
    case NDP_CMD_PING: return "PING";
    default: return "?";
  }
}

static void serve_connection(int fd, ndp_agent_config *cfg, int verbose) {
  static uint8_t rbuf[NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME + NDP_MAC_SIZE];
  static uint8_t in[4096];
  static uint8_t out[NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME];
  ndp_decoder dec;
  ndp_agent agent;
  ndp_decoder_init(&dec, rbuf, sizeof rbuf, cfg->max_frame);
  ndp_agent_init(&agent, cfg);
  for (;;) {
    struct pollfd pfd;
    ssize_t n;
    size_t pos = 0;
    int rc;
    pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
    rc = poll(&pfd, 1, IDLE_TIMEOUT_MS);
    if (rc < 0 && errno == EINTR) { if (g_stop) return; continue; }
    if (rc <= 0) { if (verbose) fprintf(stderr, "[CLOSE] idle timeout\n"); return; }
    n = recv(fd, in, sizeof in, 0);
    if (n <= 0) { if (verbose) fprintf(stderr, "[CLOSE] peer closed\n"); return; }
    while (pos < (size_t)n) {
      size_t used = 0;
      int r = ndp_decoder_feed(&dec, in + pos, (size_t)n - pos, &used);
      pos += used;
      if (r == NDP_DEC_ERROR) {
        fprintf(stderr, "[ERR] bad stream: status %d -- closing\n", dec.error);
        return;
      }
      if (r == NDP_DEC_FRAME) {
        const ndp_header *h = ndp_decoder_header(&dec);
        size_t len = ndp_agent_handle(&agent, h, ndp_decoder_payload(&dec), out, sizeof out);
        if (verbose) fprintf(stderr, "[REQ %u] %s (0x%04x) payload=%u\n", h->request_id, cmd_name(h->command),
                             h->command, h->payload_len);
        if (len == 0) { fprintf(stderr, "[ERR] response did not fit\n"); return; }
        if (verbose) {
          ndp_header rh;
          if (ndp_header_decode(out, &rh) == NDP_OK)
            fprintf(stderr, "[%s %u] status=%u payload=%u\n", rh.kind == NDP_KIND_ERR ? "ERR" : "OK", rh.request_id,
                    rh.status, rh.payload_len);
        }
        ndp_decoder_release(&dec);
        if (send_all(fd, out, len) != 0) { fprintf(stderr, "[ERR] send failed\n"); return; }
      }
    }
  }
}

int main(int argc, char **argv) {
  const char *bind_addr = "127.0.0.1";
  int port = NDP_DEFAULT_PORT, once = 0, verbose = 0, i, srv, one = 1;
  ndp_agent_config cfg;
  struct sockaddr_in sa;
  socklen_t sl = sizeof sa;
  struct sigaction act;

  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host";
  cfg.agent_version = AGENT_VERSION;
  cfg.mode = NDP_MODE_READ_ONLY;
  cfg.auth = "none";
  cfg.max_frame = NDP_DEFAULT_MAX_FRAME;
  cfg.random_bytes = random_bytes;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
      const char *m = argv[++i];
      if (!strcmp(m, "READ_ONLY")) cfg.mode = NDP_MODE_READ_ONLY;
      else if (!strcmp(m, "DEVELOPMENT")) cfg.mode = NDP_MODE_DEVELOPMENT;
      else if (!strcmp(m, "FULL")) cfg.mode = NDP_MODE_FULL;
      else { fprintf(stderr, "bad --mode\n"); return 2; }
    } else if (!strcmp(argv[i], "--once")) once = 1;
    else if (!strcmp(argv[i], "-v")) verbose = 1;
    else { fprintf(stderr, "usage: %s [--bind ADDR] [--port N] [--mode M] [--once] [-v]\n", argv[0]); return 2; }
  }

  memset(&act, 0, sizeof act);
  act.sa_handler = on_signal;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
  signal(SIGPIPE, SIG_IGN);

  srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) { perror("socket"); return 1; }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) { fprintf(stderr, "bad --bind address\n"); return 2; }
  if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 1; }
  if (listen(srv, 1) < 0) { perror("listen"); return 1; }
  getsockname(srv, (struct sockaddr *)&sa, &sl);
  printf("LISTENING %s:%u mode=%s\n", bind_addr, (unsigned)ntohs(sa.sin_port), ndp_mode_name(cfg.mode));
  fflush(stdout);

  while (!g_stop) {
    struct pollfd pfd;
    int c;
    pfd.fd = srv; pfd.events = POLLIN; pfd.revents = 0;
    if (poll(&pfd, 1, 500) <= 0) continue;
    c = accept(srv, NULL, NULL);
    if (c < 0) continue;
    if (verbose) fprintf(stderr, "[CONNECT]\n");
    serve_connection(c, &cfg, verbose);
    close(c);
    if (once) break;
  }
  close(srv);
  return 0;
}
