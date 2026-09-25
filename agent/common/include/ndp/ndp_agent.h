/* Transport-free agent core: bytes of one request frame in, one response frame out.
 * Platform code (sockets, filesystem, UI) lives outside this module. */
#ifndef NDP_AGENT_H
#define NDP_AGENT_H

#include "ndp/ndp_frame.h"
#include "ndp/ndp_fs.h"
#include "ndp/ndp_policy.h"
#include "ndp/ndp_sha256.h"

typedef struct {
  const char *platform;      /* "3ds", "host", ... */
  const char *agent_version; /* "0.1.0" */
  ndp_mode mode;
  const char *auth;          /* "none" | "required" | "paired" */
  uint32_t max_frame;
  /* Fills n random bytes (used for the HELLO device nonce). */
  void (*random_bytes)(void *ctx, uint8_t *out, size_t n);
  void *random_ctx;
  /* Optional. NULL fs => FS commands answer UNSUPPORTED_COMMAND. NULL policy => spec defaults. */
  const ndp_fs_ops *fs;
  const ndp_policy *policy;
} ndp_agent_config;

typedef struct {
  ndp_agent_config cfg;
  int hello_done;
  ndp_policy policy;
  struct {           /* directory kept open between FS_LIST pages */
    int open;
    void *handle;
    char path[NDP_PATH_MAX + 1];
    uint32_t pos;
  } dir;
  struct {           /* active FS_READ */
    int active;
    void *file;
    uint64_t remaining;
    uint64_t sent;
    uint32_t chunk;
    uint32_t request_id;
    uint16_t command;
    int want_hash;
    ndp_sha256_ctx sha;
  } xfer;
  struct {           /* active FS_WRITE (upload) */
    int active;
    int discard;     /* after an ERR: ignore this request's remaining DATA until END or the next REQ */
    uint32_t id;
    void *file;
    uint64_t declared, received;
    int overwrite, backup, target_exists, has_expected;
    uint8_t expected[32];
    char target[NDP_PATH_MAX + 1];
    char temp[NDP_PATH_MAX + 16];
    ndp_sha256_ctx sha;
  } up;
  uint64_t last_transfer_bytes; /* bytes of the most recently finished transfer */
} ndp_agent;

void ndp_agent_init(ndp_agent *a, const ndp_agent_config *cfg);

/* Handles one decoded frame and writes exactly one response frame (RES or ERR) into `out`.
 * Returns the response length, NDP_NO_REPLY when the frame needs no response (DATA of an upload),
 * or 0 if `cap` is too small. */
size_t ndp_agent_handle(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out,
                        size_t cap);

/* Streaming (FS_READ): after ndp_agent_handle returned the RES that starts a transfer,
 * ndp_agent_streaming() is 1 and the platform loop calls ndp_agent_next_frame() to obtain the DATA
 * frames and the final END (or ERR) one at a time. Returns the frame length; 0 when there is no
 * active transfer. `cap` must be at least NDP_HEADER_SIZE + the negotiated max_frame. */
int ndp_agent_streaming(const ndp_agent *a);
/* 1 while a download or an upload is in progress (the reply for the current request is not final yet). */
int ndp_agent_busy(const ndp_agent *a);
/* Changes the access mode at runtime; dropping to READ_ONLY aborts an active upload. */
void ndp_agent_set_mode(ndp_agent *a, ndp_mode mode);
size_t ndp_agent_next_frame(ndp_agent *a, uint8_t *out, size_t cap);

/* Releases directory/file handles (connection closed or replaced). Safe to call repeatedly. */
void ndp_agent_close(ndp_agent *a);

const char *ndp_mode_name(ndp_mode m);

#endif
