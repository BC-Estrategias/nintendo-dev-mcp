/* Connection handling shared by every platform that has BSD-style sockets (the host agent and
 * the 3DS agent via libctru): one listener, ONE active client, one request at a time.
 *
 * Policy: a new connection REPLACES the current client (a stale connection from a crashed Bridge
 * must never lock the console out). Idle clients are dropped after `idle_timeout_ms`.
 * All sockets are non-blocking; ndp_server_step() is meant to be called from the platform's main
 * loop and never blocks longer than `timeout_ms` (plus bounded send waits). */
#ifndef NDP_SERVER_H
#define NDP_SERVER_H

#include "ndp/ndp_agent.h"
#include "ndp/ndp_frame.h"

typedef struct {
  void *ctx;
  /* Monotonic milliseconds. Required. */
  uint64_t (*now_ms)(void *ctx);
  /* One log line, without trailing newline. May be NULL. */
  void (*log)(void *ctx, const char *line);
} ndp_server_platform;

typedef struct {
  ndp_server_platform plat;
  ndp_agent_config agent_cfg;
  uint32_t idle_timeout_ms;

  int listen_fd;
  int client_fd;
  uint32_t listen_addr; /* s_addr, network byte order (0 = INADDR_ANY) */
  uint16_t port;        /* actual listening port, host byte order */
  char peer[24];        /* "a.b.c.d" of the active client, "" if none */
  uint64_t last_activity_ms;

  /* statistics, monotonically increasing */
  uint32_t connections_opened;
  uint32_t connections_closed;
  uint32_t requests;

  ndp_agent agent;
  ndp_decoder dec;
  uint8_t rbuf[NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME + NDP_MAC_SIZE];
  uint8_t out[NDP_HEADER_SIZE + NDP_DEFAULT_MAX_FRAME];
} ndp_server;

void ndp_server_init(ndp_server *s, const ndp_server_platform *plat, const ndp_agent_config *cfg);

/* Starts listening on `s_addr` (network byte order) and `port` (0 = ephemeral). Closes any previous
 * listener/client first. Returns 0, or a negative errno-style value. */
int ndp_server_listen(ndp_server *s, uint32_t s_addr, uint16_t port);

/* Closes the client and the listener. Safe to call at any time. */
void ndp_server_close(ndp_server *s);

/* One iteration: waits up to timeout_ms for activity, then accepts/reads/answers/times out.
 * Returns 1 if something visible changed (connect, disconnect, request), 0 if not, or -1 when the
 * listener is broken and the caller should re-listen (typically after a network loss). */
int ndp_server_step(ndp_server *s, int timeout_ms);

#endif
