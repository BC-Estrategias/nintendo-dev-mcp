/* FS_WRITE / FS_MKDIR (spec §14). Uploads go to "<path>.ndp-tmp" and replace the target only after
 * size and hash are verified, so an interrupted or corrupt transfer never damages existing files. */
#include <string.h>

#include "ndp_agent_internal.h"
#include "ndp/ndp_path.h"

#define SUFFIX_TMP ".ndp-tmp"
#define SUFFIX_OLD ".ndp-old"
#define SUFFIX_BAK ".bak"

static const char *wdetail(int status) {
  switch (status) {
    case NDP_ST_FORBIDDEN_MODE: return "writes are disabled (READ_ONLY mode)";
    case NDP_ST_PATH_INVALID: return "invalid path";
    case NDP_ST_PROTECTED_PATH: return "path not writable by policy";
    case NDP_ST_NOT_FOUND: return "parent directory does not exist";
    case NDP_ST_EXISTS: return "already exists";
    case NDP_ST_NO_SPACE: return "no space left on device";
    case NDP_ST_IO_ERROR: return "i/o error";
    default: return "error";
  }
}

static int has_suffix(const char *s, const char *suf) {
  size_t n = strlen(s), m = strlen(suf);
  return n >= m && strcmp(s + n - m, suf) == 0;
}

/* Builds "<base><suffix>" if the result is still a valid protocol path. */
static int with_suffix(const char *base, const char *suf, char *out, size_t cap) {
  size_t n = strlen(base), m = strlen(suf), i, comp = 0;
  if (n + m >= cap || n + m > NDP_PATH_MAX) return NDP_ST_PATH_INVALID;
  memcpy(out, base, n);
  memcpy(out + n, suf, m + 1);
  for (i = n + m; i > 0 && out[i - 1] != '/'; i--) comp++;
  return comp > NDP_COMPONENT_MAX ? NDP_ST_PATH_INVALID : NDP_OK;
}

/* Parent directory of a normalized path ("/a/b" -> "/a", "/a" -> "/"). */
static void parent_of(const char *path, char *out) {
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] != '/') n--;
  if (n > 1) n--; /* drop the slash unless it is the root */
  memcpy(out, path, n);
  out[n] = '\0';
}

static int opt_flag(const ndp_header *req, const uint8_t *pl, uint16_t tag, int *out) {
  const uint8_t *v;
  size_t l;
  if (!ndp_tlv_find(pl, req->payload_len, tag, &v, &l)) return 0;
  if (l != 1 || v[0] > 1) return -1;
  *out = v[0];
  return 0;
}

/* Extracts the path and applies the WRITE policy. */
static int resolve_write_path(ndp_agent *a, const ndp_header *req, const uint8_t *pl, char *norm, const char **detail) {
  const uint8_t *v;
  size_t l;
  int rc;
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_PATH, &v, &l)) {
    *detail = "path required";
    return NDP_ST_BAD_REQUEST;
  }
  rc = ndp_policy_check(&a->policy, a->cfg.mode, 1, v, l, norm);
  if (rc != NDP_OK) *detail = wdetail(rc);
  return rc;
}

void ndp_agent_upload_abort(ndp_agent *a, int discard) {
  const ndp_fs_ops *fs = a->cfg.fs;
  if (!a->up.active) return;
  if (a->up.file) fs->file_close(fs->ctx, a->up.file);
  a->up.file = NULL;
  fs->remove_file(fs->ctx, a->up.temp); /* best effort */
  a->up.active = 0;
  a->up.discard = discard;
}

size_t ndp_agent_mkdir(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char norm[NDP_PATH_MAX + 1], parent[NDP_PATH_MAX + 1];
  const char *detail = "";
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc = resolve_write_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  if (strcmp(norm, "/") == 0) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, "already exists", 0);
  parent_of(norm, parent);
  rc = fs->stat(fs->ctx, parent, &st);
  if (rc != NDP_OK || !st.is_dir) return ndp_agent_error(out, cap, req, NDP_ST_NOT_FOUND, wdetail(NDP_ST_NOT_FOUND), 0);
  if (fs->stat(fs->ctx, norm, &st) == NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, wdetail(NDP_ST_EXISTS), 0);
  rc = fs->mkdir(fs->ctx, norm);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, wdetail(rc), 0);
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

size_t ndp_agent_write_start(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char norm[NDP_PATH_MAX + 1], parent[NDP_PATH_MAX + 1];
  const char *detail = "";
  const uint8_t *v;
  size_t l;
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc, overwrite = 0, backup = 0, exists = 0;
  uint64_t size;

  rc = resolve_write_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_SIZE, &v, &l) || l != 8)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "size required (u64)", 0);
  size = ndp_tlv_get_u64(v);
  if (opt_flag(req, pl, NDP_TAG_OVERWRITE, &overwrite) < 0 || opt_flag(req, pl, NDP_TAG_BACKUP, &backup) < 0)
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "overwrite/backup must be 0 or 1", 0);
  if (has_suffix(norm, SUFFIX_TMP) || has_suffix(norm, SUFFIX_OLD) || has_suffix(norm, SUFFIX_BAK))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "reserved file name suffix", 0);
  if (strcmp(norm, "/") == 0) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "not a file path", 0);

  parent_of(norm, parent);
  rc = fs->stat(fs->ctx, parent, &st);
  if (rc != NDP_OK || !st.is_dir) return ndp_agent_error(out, cap, req, NDP_ST_NOT_FOUND, wdetail(NDP_ST_NOT_FOUND), 0);

  rc = fs->stat(fs->ctx, norm, &st);
  if (rc == NDP_OK) {
    if (st.is_dir) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "target is a directory", 0);
    if (!overwrite) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, wdetail(NDP_ST_EXISTS), 0);
    exists = 1;
  } else if (rc != NDP_ST_NOT_FOUND) {
    return ndp_agent_error(out, cap, req, (uint16_t)rc, wdetail(rc), 0);
  }

  memset(&a->up, 0, sizeof a->up);
  strcpy(a->up.target, norm);
  rc = with_suffix(norm, SUFFIX_TMP, a->up.temp, sizeof a->up.temp);
  if (rc == NDP_OK) { char probe[NDP_PATH_MAX + 16]; rc = with_suffix(norm, SUFFIX_OLD, probe, sizeof probe); }
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "file name too long for the temporary name", 0);
  if (fs->stat(fs->ctx, a->up.temp, &st) == NDP_OK && !st.is_dir) fs->remove_file(fs->ctx, a->up.temp); /* stale temp */

  rc = fs->file_create(fs->ctx, a->up.temp, &a->up.file);
  if (rc != NDP_OK) { a->up.file = NULL; return ndp_agent_error(out, cap, req, (uint16_t)rc, wdetail(rc), 0); }

  a->up.active = 1;
  a->up.id = req->request_id;
  a->up.declared = size;
  a->up.overwrite = overwrite;
  a->up.backup = backup;
  a->up.target_exists = exists;
  if (ndp_tlv_find(pl, req->payload_len, NDP_TAG_SHA256, &v, &l)) {
    if (l != 32) { ndp_agent_upload_abort(a, 0); return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "sha256 must be 32 bytes", 0); }
    memcpy(a->up.expected, v, 32);
    a->up.has_expected = 1;
  }
  ndp_sha256_init(&a->up.sha);

  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u32(&w, NDP_TAG_MAX_CHUNK, a->cfg.max_frame);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

/* Fails the upload: removes the temp file, replies ERR, and (if the END was not seen yet) starts discarding. */
static size_t fail_upload(ndp_agent *a, const ndp_header *hdr, uint8_t *out, size_t cap, uint16_t status,
                          const char *detail, int discard) {
  ndp_agent_upload_abort(a, discard);
  return ndp_agent_error(out, cap, hdr, status, detail, 0);
}

static size_t commit_upload(ndp_agent *a, const ndp_header *hdr, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char old[NDP_PATH_MAX + 16], bak[NDP_PATH_MAX + 16];
  uint8_t digest[32];
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc, replaced = 0;

  ndp_sha256_final(&a->up.sha, digest);
  if (a->up.has_expected && !ndp_ct_equal(digest, a->up.expected, 32))
    return fail_upload(a, hdr, out, cap, NDP_ST_HASH_MISMATCH, "sha256 mismatch", 0);

  rc = fs->file_sync(fs->ctx, a->up.file);
  if (rc != NDP_OK) return fail_upload(a, hdr, out, cap, (uint16_t)rc, "could not flush the file", 0);
  fs->file_close(fs->ctx, a->up.file);
  a->up.file = NULL;

  with_suffix(a->up.target, SUFFIX_OLD, old, sizeof old);
  if (a->up.target_exists) {
    /* 1) move the existing file aside, 2) put the new one in place, 3) drop (or keep) the old one */
    fs->remove_file(fs->ctx, old); /* stale leftover from an earlier crash */
    rc = fs->rename(fs->ctx, a->up.target, old);
    if (rc != NDP_OK) return fail_upload(a, hdr, out, cap, NDP_ST_IO_ERROR, "could not move the existing file aside", 0);
    rc = fs->rename(fs->ctx, a->up.temp, a->up.target);
    if (rc != NDP_OK) {
      fs->rename(fs->ctx, old, a->up.target); /* restore */
      return fail_upload(a, hdr, out, cap, NDP_ST_IO_ERROR, "could not move the new file into place (old file restored)", 0);
    }
    if (a->up.backup && with_suffix(a->up.target, SUFFIX_BAK, bak, sizeof bak) == NDP_OK) {
      fs->remove_file(fs->ctx, bak);
      if (fs->rename(fs->ctx, old, bak) != NDP_OK) fs->remove_file(fs->ctx, old);
    } else {
      fs->remove_file(fs->ctx, old);
    }
    replaced = 1;
  } else {
    if (fs->stat(fs->ctx, a->up.target, &st) == NDP_OK) /* someone created it meanwhile */
      return fail_upload(a, hdr, out, cap, NDP_ST_EXISTS, "target appeared during the upload", 0);
    rc = fs->rename(fs->ctx, a->up.temp, a->up.target);
    if (rc != NDP_OK) return fail_upload(a, hdr, out, cap, NDP_ST_IO_ERROR, "could not move the new file into place", 0);
  }

  a->last_transfer_bytes = a->up.received;
  a->up.active = 0;
  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_u64(&w, NDP_TAG_WRITTEN, a->up.received);
  ndp_tlv_put(&w, NDP_TAG_SHA256, digest, sizeof digest);
  { uint8_t r = (uint8_t)replaced; ndp_tlv_put(&w, NDP_TAG_REPLACED, &r, 1); }
  return ndp_agent_finish(out, cap, hdr, NDP_KIND_RES, NDP_OK, &w);
}

/* DATA / END frames of the active upload (or of one being discarded). */
size_t ndp_agent_upload_frame(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  if (a->up.discard) {
    if (hdr->kind == NDP_KIND_END) a->up.discard = 0;
    return NDP_NO_REPLY;
  }
  if (hdr->kind == NDP_KIND_DATA) {
    long n;
    if (a->up.received + hdr->payload_len > a->up.declared)
      return fail_upload(a, hdr, out, cap, NDP_ST_BAD_REQUEST, "more data than the declared size", 1);
    if (hdr->payload_len) {
      n = fs->file_write(fs->ctx, a->up.file, payload, hdr->payload_len);
      if (n != (long)hdr->payload_len)
        return fail_upload(a, hdr, out, cap, n < 0 ? (uint16_t)-n : NDP_ST_IO_ERROR, "write failed", 1);
      ndp_sha256_update(&a->up.sha, payload, hdr->payload_len);
      a->up.received += hdr->payload_len;
    }
    return NDP_NO_REPLY;
  }
  /* END */
  if (a->up.received != a->up.declared)
    return fail_upload(a, hdr, out, cap, NDP_ST_BAD_REQUEST, "size mismatch: fewer bytes than declared", 0);
  return commit_upload(a, hdr, out, cap);
}
