/* Host agent: the shared NDP server (agent/posix) on macOS/Linux, for Bridge integration tests and
 * development without a console.
 * Usage: ndp-host-agent [--bind ADDR] [--port N] [--mode READ_ONLY|DEVELOPMENT|FULL] [--root DIR]
 *                        [--write-root PATH]... [--idle-ms N] [--once] [-v]
 * --root DIR serves DIR as the "SD card" (FS_*); without it FS commands are unsupported.
 * --write-root PATH (repeatable) replaces the default writable folder (/3ds/nintendo-dev-agent). */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ndp/ndp_posix_fs.h"
#include "ndp/ndp_server.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void *ctx) {
  struct timespec ts;
  (void)ctx;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int g_verbose = 0;
static void log_line(void *ctx, const char *line) {
  (void)ctx;
  if (g_verbose) fprintf(stderr, "%s\n", line);
}

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
  if (got < n) { fprintf(stderr, "fatal: /dev/urandom unavailable\n"); exit(1); }
}

int main(int argc, char **argv) {
  const char *bind_addr = "127.0.0.1";
  const char *root = NULL;
  int port = NDP_DEFAULT_PORT, once = 0, i, rc;
  unsigned idle_ms = 0;
  ndp_agent_config cfg;
  ndp_server_platform plat;
  struct in_addr ia;
  struct sigaction act;
  static ndp_server srv; /* large buffers inside: keep off the stack */
  static ndp_policy policy;
  int custom_policy = 0;
  static ndp_fs_ops fs_ops;
  static ndp_posix_fs_ctx fs_ctx;

  memset(&cfg, 0, sizeof cfg);
  cfg.platform = "host";
  cfg.agent_version = NDP_AGENT_VERSION;
  cfg.mode = NDP_MODE_READ_ONLY;
  cfg.auth = "none";
  cfg.max_frame = NDP_DEFAULT_MAX_FRAME;
  cfg.random_bytes = random_bytes;

  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
    else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
    else if (!strcmp(argv[i], "--write-root") && i + 1 < argc) {
      if (!custom_policy) { ndp_policy_init_default(&policy); policy.write_roots.count = 0; custom_policy = 1; }
      if (ndp_pathlist_add(&policy.write_roots, argv[++i]) != NDP_OK) { fprintf(stderr, "bad --write-root\n"); return 2; }
    }
    else if (!strcmp(argv[i], "--idle-ms") && i + 1 < argc) idle_ms = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
      const char *m = argv[++i];
      if (!strcmp(m, "READ_ONLY")) cfg.mode = NDP_MODE_READ_ONLY;
      else if (!strcmp(m, "DEVELOPMENT")) cfg.mode = NDP_MODE_DEVELOPMENT;
      else if (!strcmp(m, "FULL")) cfg.mode = NDP_MODE_FULL;
      else { fprintf(stderr, "bad --mode\n"); return 2; }
    } else if (!strcmp(argv[i], "--once")) once = 1;
    else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    else {
      fprintf(stderr, "usage: %s [--bind ADDR] [--port N] [--mode M] [--root DIR] [--write-root P]... [--idle-ms N] [--once] [-v]\n", argv[0]);
      return 2;
    }
  }
  if (inet_pton(AF_INET, bind_addr, &ia) != 1) { fprintf(stderr, "bad --bind address\n"); return 2; }

  memset(&act, 0, sizeof act);
  act.sa_handler = on_signal;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
  signal(SIGPIPE, SIG_IGN);

  if (root) {
    size_t n = strlen(root);
    char clean[256];
    if (n >= sizeof clean) { fprintf(stderr, "--root too long\n"); return 2; }
    memcpy(clean, root, n + 1);
    while (n > 1 && clean[n - 1] == '/') clean[--n] = '\0'; /* the root must not end with '/' */
    if (strcmp(clean, "/") == 0) clean[0] = '\0';
    if (ndp_posix_fs_init(&fs_ops, &fs_ctx, clean) != 0) { fprintf(stderr, "--root too long\n"); return 2; }
    cfg.fs = &fs_ops;
  }
  if (custom_policy) cfg.policy = &policy;

  memset(&plat, 0, sizeof plat);
  plat.now_ms = now_ms;
  plat.log = log_line;
  ndp_server_init(&srv, &plat, &cfg);
  if (idle_ms) srv.idle_timeout_ms = idle_ms;

  rc = ndp_server_listen(&srv, ia.s_addr, (uint16_t)port);
  if (rc < 0) { fprintf(stderr, "listen failed: %d\n", rc); return 1; }
  printf("LISTENING %s:%u mode=%s root=%s\n", bind_addr, (unsigned)srv.port, ndp_mode_name(cfg.mode),
         root ? root : "(none)");
  fflush(stdout);

  while (!g_stop) {
    if (ndp_server_step(&srv, 200) < 0) {
      fprintf(stderr, "[ERR] listener broken; re-listening\n");
      if (ndp_server_listen(&srv, ia.s_addr, (uint16_t)port) < 0) return 1;
    }
    if (once && srv.connections_opened > 0 && srv.client_fd < 0) break;
  }
  ndp_server_close(&srv);
  return 0;
}
