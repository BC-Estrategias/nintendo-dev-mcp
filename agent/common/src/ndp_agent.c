#include "ndp/ndp_agent.h"

#include <string.h>

#include "ndp_agent_internal.h"

const char *ndp_mode_name(ndp_mode m) {
  switch (m) {
    case NDP_MODE_READ_ONLY: return "READ_ONLY";
    case NDP_MODE_DEVELOPMENT: return "DEVELOPMENT";
    case NDP_MODE_FULL: return "FULL";
  }
  return "READ_ONLY";
}

void ndp_agent_init(ndp_agent *a, const ndp_agent_config *cfg) {
  memset(a, 0, sizeof *a);
  a->cfg = *cfg;
  if (cfg->policy) a->policy = *cfg->policy;
  else ndp_policy_init_default(&a->policy);
}

/* Response frames are built with the payload written directly after the header slot. */
size_t ndp_agent_finish(uint8_t *out, size_t cap, const ndp_header *req, uint8_t kind, uint16_t status,
                        const ndp_tlv_w *w) {
  ndp_header h;
  if (w->overflow) return 0;
  h.version = NDP_PROTOCOL_VERSION;
  h.kind = kind;
  h.flags = 0;
  h.request_id = req->request_id;
  h.command = req->command;
  h.status = status;
  h.payload_len = 0;
  return ndp_frame_encode(out, cap, &h, out + NDP_HEADER_SIZE, w->len, NULL);
}

size_t ndp_agent_error(uint8_t *out, size_t cap, const ndp_header *req, uint16_t status, const char *detail,
                       int with_supported) {
  ndp_tlv_w w;
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  if (with_supported) {
    ndp_tlv_put_u16(&w, NDP_TAG_SUPPORTED_MIN, NDP_PROTOCOL_VERSION);
    ndp_tlv_put_u16(&w, NDP_TAG_SUPPORTED_MAX, NDP_PROTOCOL_VERSION);
  }
  ndp_tlv_put_str(&w, NDP_TAG_DETAIL, detail);
  return ndp_agent_finish(out, cap, req, NDP_KIND_ERR, status, &w);
}

static size_t do_hello(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *vmin, *vmax, *nonce;
  size_t lmin, lmax, lnon;
  unsigned lo, hi, chosen;
  ndp_tlv_w w;
  uint8_t device_nonce[16];
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROTOCOL, &vmin, &lmin) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_PROTOCOL_MAX, &vmax, &lmax) ||
      !ndp_tlv_find(pl, req->payload_len, NDP_TAG_NONCE, &nonce, &lnon))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "missing field", 0);
  if (lmin != 2 || lmax != 2 || lnon != 16) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "bad field size", 0);
  lo = ndp_tlv_get_u16(vmin);
  hi = ndp_tlv_get_u16(vmax);
  if (lo > hi) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "protocol_min > protocol_max", 0);
  chosen = hi < NDP_PROTOCOL_VERSION ? hi : NDP_PROTOCOL_VERSION;
  if (chosen < (lo > NDP_PROTOCOL_VERSION ? lo : NDP_PROTOCOL_VERSION))
    return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_PROTOCOL, "no common protocol version", 1);
  ndp_agent_fs_close(a); /* a new HELLO starts a clean session */
  memset(device_nonce, 0, sizeof device_nonce);
  if (a->cfg.random_bytes) a->cfg.random_bytes(a->cfg.random_ctx, device_nonce, sizeof device_nonce);
  a->hello_done = 1;
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u16(&w, NDP_TAG_PROTOCOL, (uint16_t)chosen);
  ndp_tlv_put_str(&w, NDP_TAG_PLATFORM, a->cfg.platform);
  ndp_tlv_put_str(&w, NDP_TAG_AGENT_VERSION, a->cfg.agent_version);
  ndp_tlv_put(&w, NDP_TAG_NONCE, device_nonce, sizeof device_nonce);
  ndp_tlv_put_str(&w, NDP_TAG_AUTH, a->cfg.auth);
  ndp_tlv_put_str(&w, NDP_TAG_MODE, ndp_mode_name(a->cfg.mode));
  ndp_tlv_put_u32(&w, NDP_TAG_MAX_FRAME, a->cfg.max_frame);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

static size_t do_ping(const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const uint8_t *n;
  size_t ln;
  ndp_tlv_w w;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PING_NONCE, &n, &ln) || ln != 8)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "ping_nonce required", 0);
  if (cap < NDP_HEADER_SIZE) return 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u64(&w, NDP_TAG_PING_NONCE, ndp_tlv_get_u64(n));
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_handle(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out,
                        size_t cap) {
  if (req->version != NDP_PROTOCOL_VERSION)
    return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_PROTOCOL, "unsupported frame version", 1);
  if (a->up.active || a->up.discard) {
    if ((req->kind == NDP_KIND_DATA || req->kind == NDP_KIND_END) && req->request_id == a->up.id)
      return ndp_agent_upload_frame(a, req, payload, out, cap);
    if (a->up.discard && req->kind == NDP_KIND_REQ) a->up.discard = 0; /* a new request resynchronizes */
  }
  if (req->kind != NDP_KIND_REQ) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "expected REQ", 0);
  if (!ndp_tlv_validate(payload, req->payload_len))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "malformed payload", 0);
  if (a->xfer.active || a->up.active) return ndp_agent_error(out, cap, req, NDP_ST_BUSY, "transfer in progress", 0);
  if (req->command == NDP_CMD_HELLO) return do_hello(a, req, payload, out, cap);
  if (!a->hello_done) return ndp_agent_error(out, cap, req, NDP_ST_HELLO_REQUIRED, "send HELLO first", 0);
  if (req->command == NDP_CMD_PING) return do_ping(req, payload, out, cap);
  if (a->cfg.fs && (req->command == NDP_CMD_FS_LIST || req->command == NDP_CMD_FS_STAT ||
                    req->command == NDP_CMD_FS_READ))
    return ndp_agent_fs_handle(a, req, payload, out, cap);
  if (a->cfg.fs && a->cfg.fs->file_create) {
    if (req->command == NDP_CMD_FS_WRITE) return ndp_agent_write_start(a, req, payload, out, cap);
    if (req->command == NDP_CMD_FS_MKDIR) return ndp_agent_mkdir(a, req, payload, out, cap);
  }
  return ndp_agent_error(out, cap, req, NDP_ST_UNSUPPORTED_COMMAND, "unknown command", 0);
}

int ndp_agent_streaming(const ndp_agent *a) { return a->xfer.active; }

size_t ndp_agent_next_frame(ndp_agent *a, uint8_t *out, size_t cap) {
  return a->xfer.active ? ndp_agent_fs_next_frame(a, out, cap) : 0;
}

int ndp_agent_busy(const ndp_agent *a) { return a->xfer.active || a->up.active; }

void ndp_agent_set_mode(ndp_agent *a, ndp_mode mode) {
  a->cfg.mode = mode;
  if (mode == NDP_MODE_READ_ONLY && a->up.active && a->cfg.fs) ndp_agent_upload_abort(a, 1);
}

void ndp_agent_close(ndp_agent *a) {
  if (a->cfg.fs && a->up.active) ndp_agent_upload_abort(a, 0);
  ndp_agent_fs_close(a);
}
