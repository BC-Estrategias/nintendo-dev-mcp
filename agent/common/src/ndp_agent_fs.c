/* FS_LIST / FS_STAT / FS_READ (spec §13). Read-only; every path goes through the access policy. */
#include <string.h>

#include "ndp_agent_internal.h"
#include "ndp/ndp_path.h"

static void put_u8(ndp_tlv_w *w, uint16_t tag, uint8_t v) { ndp_tlv_put(w, tag, &v, 1); }

static const char *detail_for(int status) {
  switch (status) {
    case NDP_ST_NOT_FOUND: return "no such file or directory";
    case NDP_ST_PATH_INVALID: return "invalid path";
    case NDP_ST_PROTECTED_PATH: return "path not allowed by policy";
    case NDP_ST_IO_ERROR: return "i/o error";
    default: return "error";
  }
}

/* Extracts and policy-checks the `path` field (read access). Returns NDP_OK or an error status. */
static int resolve_path(ndp_agent *a, const ndp_header *req, const uint8_t *pl, char *norm, const char **detail) {
  const uint8_t *v;
  size_t l;
  int rc;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PATH, &v, &l)) {
    *detail = "path required";
    return NDP_ST_BAD_REQUEST;
  }
  rc = ndp_policy_check(&a->policy, a->cfg.mode, 0, v, l, norm);
  if (rc != NDP_OK) *detail = detail_for(rc);
  return rc;
}

/* Optional fixed-size numeric field. Returns 0 (absent -> *out untouched), 1 (present) or -1 (wrong size). */
static int opt_num(const ndp_header *req, const uint8_t *pl, uint16_t tag, size_t size, uint64_t *out) {
  const uint8_t *v;
  size_t l;
  if (!ndp_tlv_find(pl, req->payload_len, tag, &v, &l)) return 0;
  if (l != size) return -1;
  *out = size == 1 ? v[0] : size == 4 ? ndp_tlv_get_u32(v) : ndp_tlv_get_u64(v);
  return 1;
}

static void close_dir(ndp_agent *a) {
  if (a->dir.open) a->cfg.fs->dir_close(a->cfg.fs->ctx, a->dir.handle);
  a->dir.open = 0;
  a->dir.handle = NULL;
}

static void end_transfer(ndp_agent *a) {
  if (a->xfer.file) a->cfg.fs->file_close(a->cfg.fs->ctx, a->xfer.file);
  a->xfer.file = NULL;
  a->xfer.active = 0;
  a->last_transfer_bytes = a->xfer.sent;
}

void ndp_agent_fs_close(ndp_agent *a) {
  if (!a->cfg.fs) return;
  close_dir(a);
  if (a->xfer.active) end_transfer(a);
}

static size_t do_stat(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  char norm[NDP_PATH_MAX + 1];
  const char *detail = "";
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc = resolve_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  rc = a->cfg.fs->stat(a->cfg.fs->ctx, norm, &st);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0);
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  put_u8(&w, NDP_TAG_TYPE, st.is_dir ? NDP_TYPE_DIR : NDP_TYPE_FILE);
  ndp_tlv_put_u64(&w, NDP_TAG_SIZE, st.size);
  ndp_tlv_put_u64(&w, NDP_TAG_MTIME, st.mtime);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

static size_t do_list(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char norm[NDP_PATH_MAX + 1];
  char name[256];
  uint8_t ev[1 + 8 + 255];
  const char *detail = "";
  ndp_fs_stat st, es;
  ndp_tlv_w w;
  uint64_t cursor = 0;
  int rc, entries = 0, more = 1, r;

  rc = resolve_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  if (opt_num(req, pl, NDP_TAG_CURSOR, 4, &cursor) < 0) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "bad cursor", 0);
  rc = fs->stat(fs->ctx, norm, &st);
  if (rc != NDP_OK) { close_dir(a); return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0); }
  if (!st.is_dir) { close_dir(a); return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "not a directory", 0); }

  if (!(a->dir.open && a->dir.pos == cursor && strcmp(a->dir.path, norm) == 0)) {
    close_dir(a);
    rc = fs->dir_open(fs->ctx, norm, &a->dir.handle);
    if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0);
    a->dir.open = 1;
    strcpy(a->dir.path, norm);
    a->dir.pos = 0;
    while (a->dir.pos < cursor) {
      r = fs->dir_next(fs->ctx, a->dir.handle, name, sizeof name, &es);
      if (r < 0) { close_dir(a); return ndp_agent_error(out, cap, req, (uint16_t)-r, detail_for(-r), 0); }
      if (r == 0) break;
      a->dir.pos++;
    }
  }

  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  while (entries < NDP_LIST_PAGE_MAX) {
    size_t nl;
    int i;
    r = fs->dir_next(fs->ctx, a->dir.handle, name, sizeof name, &es);
    if (r == 0) { more = 0; break; }
    if (r < 0) { close_dir(a); return ndp_agent_error(out, cap, req, (uint16_t)-r, detail_for(-r), 0); }
    nl = strlen(name);
    ev[0] = es.is_dir ? NDP_TYPE_DIR : NDP_TYPE_FILE;
    for (i = 0; i < 8; i++) ev[1 + i] = (uint8_t)(es.size >> (8 * i));
    memcpy(ev + 9, name, nl);
    ndp_tlv_put(&w, NDP_TAG_ENTRY, ev, 9 + nl);
    a->dir.pos++;
    entries++;
  }
  put_u8(&w, NDP_TAG_LIST_MORE, (uint8_t)more);
  ndp_tlv_put_u32(&w, NDP_TAG_NEXT_CURSOR, a->dir.pos);
  if (!more) close_dir(a);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

static size_t do_read(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char norm[NDP_PATH_MAX + 1];
  const char *detail = "";
  ndp_fs_stat st;
  ndp_tlv_w w;
  uint64_t offset = 0, length = 0, chunk = NDP_DEFAULT_CHUNK, want_hash = 0, will;
  int rc;

  rc = resolve_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  if (opt_num(req, pl, NDP_TAG_OFFSET, 8, &offset) < 0 || opt_num(req, pl, NDP_TAG_LENGTH, 8, &length) < 0 ||
      opt_num(req, pl, NDP_TAG_CHUNK, 4, &chunk) < 0 || opt_num(req, pl, NDP_TAG_WANT_HASH, 1, &want_hash) < 0)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "bad field size", 0);
  if (chunk < NDP_MIN_CHUNK) chunk = NDP_MIN_CHUNK;
  if (chunk > a->cfg.max_frame) chunk = a->cfg.max_frame;

  rc = fs->stat(fs->ctx, norm, &st);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0);
  if (st.is_dir) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "is a directory", 0);
  if (offset > st.size) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "offset beyond end of file", 0);
  will = st.size - offset;
  if (length != 0 && length < will) will = length;

  rc = fs->file_open(fs->ctx, norm, &a->xfer.file);
  if (rc != NDP_OK) { a->xfer.file = NULL; return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0); }
  if (offset > 0 && (rc = fs->file_seek(fs->ctx, a->xfer.file, offset)) != NDP_OK) {
    fs->file_close(fs->ctx, a->xfer.file);
    a->xfer.file = NULL;
    return ndp_agent_error(out, cap, req, (uint16_t)rc, detail_for(rc), 0);
  }

  a->xfer.active = 1;
  a->xfer.remaining = will;
  a->xfer.sent = 0;
  a->xfer.chunk = (uint32_t)chunk;
  a->xfer.request_id = req->request_id;
  a->xfer.command = req->command;
  a->xfer.want_hash = want_hash ? 1 : 0;
  if (a->xfer.want_hash) ndp_sha256_init(&a->xfer.sha);

  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u64(&w, NDP_TAG_TOTAL_SIZE, st.size);
  ndp_tlv_put_u64(&w, NDP_TAG_WILL_SEND, will);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_fs_handle(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  if (cap < NDP_HEADER_SIZE) return 0;
  switch (req->command) {
    case NDP_CMD_FS_STAT: return do_stat(a, req, pl, out, cap);
    case NDP_CMD_FS_LIST: return do_list(a, req, pl, out, cap);
    default: return do_read(a, req, pl, out, cap);
  }
}

size_t ndp_agent_fs_next_frame(ndp_agent *a, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  ndp_header h;
  ndp_tlv_w w;
  size_t n;
  long r;
  if (cap <= NDP_HEADER_SIZE) return 0;
  memset(&h, 0, sizeof h);
  h.version = NDP_PROTOCOL_VERSION;
  h.request_id = a->xfer.request_id;
  h.command = a->xfer.command;

  if (a->xfer.remaining == 0) { /* finished: END, with the digest when asked */
    ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
    if (a->xfer.want_hash) {
      uint8_t digest[32];
      ndp_sha256_final(&a->xfer.sha, digest);
      ndp_tlv_put(&w, NDP_TAG_SHA256, digest, sizeof digest);
    }
    end_transfer(a);
    return ndp_agent_finish(out, cap, &h, NDP_KIND_END, NDP_OK, &w);
  }

  n = a->xfer.chunk;
  if (n > a->xfer.remaining) n = (size_t)a->xfer.remaining;
  if (n > cap - NDP_HEADER_SIZE) n = cap - NDP_HEADER_SIZE;
  r = fs->file_read(fs->ctx, a->xfer.file, out + NDP_HEADER_SIZE, n);
  if (r <= 0) { /* read error, or the file shrank under us */
    end_transfer(a);
    return ndp_agent_error(out, cap, &h, NDP_ST_IO_ERROR, r < 0 ? "read error" : "unexpected end of file", 0);
  }
  a->xfer.remaining -= (uint64_t)r;
  a->xfer.sent += (uint64_t)r;
  if (a->xfer.want_hash) ndp_sha256_update(&a->xfer.sha, out + NDP_HEADER_SIZE, (size_t)r);
  h.kind = NDP_KIND_DATA;
  h.flags = a->xfer.remaining > 0 ? NDP_FLAG_MORE : 0;
  return ndp_frame_encode(out, cap, &h, out + NDP_HEADER_SIZE, (size_t)r, NULL);
}
