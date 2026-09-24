/* Transport-free agent core: bytes of one request frame in, one response frame out.
 * Platform code (sockets, filesystem, UI) lives outside this module. */
#ifndef NDP_AGENT_H
#define NDP_AGENT_H

#include "ndp/ndp_frame.h"
#include "ndp/ndp_policy.h"

typedef struct {
  const char *platform;      /* "3ds", "host", ... */
  const char *agent_version; /* "0.1.0" */
  ndp_mode mode;
  const char *auth;          /* "none" | "required" | "paired" */
  uint32_t max_frame;
  /* Fills n random bytes (used for the HELLO device nonce). */
  void (*random_bytes)(void *ctx, uint8_t *out, size_t n);
  void *random_ctx;
} ndp_agent_config;

typedef struct {
  ndp_agent_config cfg;
  int hello_done;
} ndp_agent;

void ndp_agent_init(ndp_agent *a, const ndp_agent_config *cfg);

/* Handles one decoded frame and writes exactly one response frame (RES or ERR) into `out`.
 * Returns the response length, or 0 if `cap` is too small. */
size_t ndp_agent_handle(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out,
                        size_t cap);

const char *ndp_mode_name(ndp_mode m);

#endif
