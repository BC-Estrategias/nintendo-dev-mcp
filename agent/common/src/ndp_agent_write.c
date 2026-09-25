/* FS_WRITE / FS_MKDIR (spec §14). Uploads go to "<path>.ndp-tmp" and replace the target only after
 * size and hash are verified, so an interrupted or corrupt transfer never damages existing files. */
#include <stdio.h>
#include <string.h>

#include "ndp_agent_internal.h"
#include "ndp/ndp_path.h"

#define SUFFIX_TMP ".ndp-tmp"
#define SUFFIX_OLD ".ndp-old"
#define SUFFIX_BAK ".bak"
#define TRASH_NAME ".ndp-trash"

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
/* "<root>/.ndp-trash" ("/.ndp-trash" for the filesystem root). */
static int trash_dir_of(const char *root, char *out, size_t cap) {
  int n = strcmp(root, "/") == 0 ? snprintf(out, cap, "/%s", TRASH_NAME) : snprintf(out, cap, "%s/%s", root, TRASH_NAME);
  return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* True when `path` lies inside the trash of any write root. */
static int in_any_trash(const ndp_agent *a, const char *path) {
  char t[NDP_PATH_MAX + 16];
  int i;
  for (i = 0; i < a->policy.write_roots.count; i++)
    if (trash_dir_of(a->policy.write_roots.entries[i], t, sizeof t) == 0 && ndp_path_inside(path, t)) return 1;
  return 0;
}

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
  if (in_any_trash(a, norm)) return ndp_agent_error(out, cap, req, NDP_ST_PROTECTED_PATH, "the trash is managed by the agent", 0);
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
  if (in_any_trash(a, norm)) return ndp_agent_error(out, cap, req, NDP_ST_PROTECTED_PATH, "the trash is managed by the agent", 0);

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

/* FS_DELETE: never removes data, only moves the item into "<write root>/.ndp-trash" (spec §14). */
size_t ndp_agent_delete(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char norm[NDP_PATH_MAX + 1], trash[NDP_PATH_MAX + 16], dest[NDP_PATH_MAX + 32], name[NDP_COMPONENT_MAX + 1];
  const char *detail = "", *root = NULL, *slash;
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc, i, n;
  size_t best = 0;

  rc = resolve_write_path(a, req, pl, norm, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  for (i = 0; i < a->policy.write_roots.count; i++) { /* the most specific root that contains the path */
    const char *r = a->policy.write_roots.entries[i];
    if (ndp_path_inside(norm, r) && (root == NULL || strlen(r) > best)) { root = r; best = strlen(r); }
  }
  if (!root) return ndp_agent_error(out, cap, req, NDP_ST_PROTECTED_PATH, wdetail(NDP_ST_PROTECTED_PATH), 0);
  if (strcmp(norm, root) == 0) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "cannot delete a write root", 0);
  if (trash_dir_of(root, trash, sizeof trash) != 0) return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "path too long", 0);
  if (in_any_trash(a, norm)) return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "already in the trash (permanent deletion is not supported)", 0);

  rc = fs->stat(fs->ctx, norm, &st);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, rc == NDP_ST_NOT_FOUND ? "no such file or directory" : wdetail(rc), 0);

  rc = fs->stat(fs->ctx, trash, &st);
  if (rc == NDP_ST_NOT_FOUND) {
    rc = fs->mkdir(fs->ctx, trash); /* on the 3DS this one call takes ~6 s, once per root */
    if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, "could not create the trash folder", 0);
  } else if (rc != NDP_OK || !st.is_dir) {
    return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "the trash folder is unusable", 0);
  }

  slash = strrchr(norm, '/');
  n = snprintf(name, sizeof name, "%s", slash + 1);
  if (n <= 0 || (size_t)n >= sizeof name) return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "name too long", 0);
  for (i = 0; i < 1000; i++) { /* <name>, then <name>.1, <name>.2 ... */
    n = i == 0 ? snprintf(dest, sizeof dest, "%s/%s", trash, name) : snprintf(dest, sizeof dest, "%s/%s.%d", trash, name, i);
    if (n <= 0 || (size_t)n >= sizeof dest || strlen(dest) > NDP_PATH_MAX) return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "name too long", 0);
    if (fs->stat(fs->ctx, dest, &st) == NDP_ST_NOT_FOUND) break;
  }
  if (i == 1000) return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "too many items with this name in the trash", 0);

  rc = fs->rename(fs->ctx, norm, dest);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "could not move the item to the trash (left in place)", 0);

  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_str(&w, NDP_TAG_TRASH_PATH, dest);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}

/* FS_RENAME (spec §14): moves or renames a file or folder, atomically, without ever overwriting. Both the source and the
 * destination must pass the WRITE policy. Items enter the trash only through FS_DELETE; leaving it (restoring) is a rename. */
static int is_configured_root(const ndp_agent *a, const char *norm) {
  int i;
  if (strcmp(norm, "/") == 0) return 1;
  for (i = 0; i < a->policy.write_roots.count; i++)
    if (strcmp(norm, a->policy.write_roots.entries[i]) == 0) return 1;
  for (i = 0; i < a->policy.read_roots.count; i++)
    if (strcmp(norm, a->policy.read_roots.entries[i]) == 0) return 1;
  return 0;
}

static int is_a_trash_dir(const ndp_agent *a, const char *norm) {
  char t[NDP_PATH_MAX + 16];
  int i;
  for (i = 0; i < a->policy.write_roots.count; i++)
    if (trash_dir_of(a->policy.write_roots.entries[i], t, sizeof t) == 0 && strcmp(norm, t) == 0) return 1;
  return 0;
}

size_t ndp_agent_rename(ndp_agent *a, const ndp_header *req, const uint8_t *pl, uint8_t *out, size_t cap) {
  const ndp_fs_ops *fs = a->cfg.fs;
  char from[NDP_PATH_MAX + 1], to[NDP_PATH_MAX + 1], parent[NDP_PATH_MAX + 1], tmp[NDP_PATH_MAX + 16];
  const char *detail = "";
  const uint8_t *v;
  size_t l;
  ndp_fs_stat st;
  ndp_tlv_w w;
  int rc, case_only = 0;

  rc = resolve_write_path(a, req, pl, from, &detail);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, detail, 0);
  if (!ndp_tlv_find(pl, req->payload_len, NDP_TAG_NEW_PATH, &v, &l))
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "new_path required", 0);
  rc = ndp_policy_check(&a->policy, a->cfg.mode, 1, v, l, to);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, wdetail(rc), 0);

  if (is_configured_root(a, from) || is_a_trash_dir(a, from))
    return ndp_agent_error(out, cap, req, NDP_ST_PROTECTED_PATH, "this folder is a configured root or the trash and cannot be moved", 0);
  if (is_configured_root(a, to)) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, wdetail(NDP_ST_EXISTS), 0);
  if (in_any_trash(a, to))
    return ndp_agent_error(out, cap, req, NDP_ST_PROTECTED_PATH, "items enter the trash only by deleting them", 0);
  if (strlen(to) + sizeof ".ndp-ren" > NDP_PATH_MAX) return ndp_agent_error(out, cap, req, NDP_ST_PATH_INVALID, "path too long", 0);

  /* "a" -> "A": the same name on a case-insensitive card (FAT) */
  if (ndp_path_inside(from, to) && ndp_path_inside(to, from)) {
    if (strcmp(from, to) == 0) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, "source and destination are the same", 0);
    case_only = 1;
  } else if (ndp_path_inside(to, from)) {
    return ndp_agent_error(out, cap, req, NDP_ST_BAD_REQUEST, "cannot move a folder into itself", 0);
  }

  rc = fs->stat(fs->ctx, from, &st);
  if (rc != NDP_OK) return ndp_agent_error(out, cap, req, (uint16_t)rc, rc == NDP_ST_NOT_FOUND ? "no such file or directory" : wdetail(rc), 0);
  parent_of(to, parent);
  rc = fs->stat(fs->ctx, parent, &st);
  if (rc != NDP_OK || !st.is_dir) return ndp_agent_error(out, cap, req, NDP_ST_NOT_FOUND, "destination folder does not exist", 0);
  if (!case_only && fs->stat(fs->ctx, to, &st) == NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, wdetail(NDP_ST_EXISTS), 0);

  if (case_only) { /* two steps through a temporary name so the OS does not see "the target exists" */
    snprintf(tmp, sizeof tmp, "%s.ndp-ren", from);
    if (fs->stat(fs->ctx, tmp, &st) == NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_EXISTS, "a leftover temporary name is in the way", 0);
    if (fs->rename(fs->ctx, from, tmp) != NDP_OK) return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "could not rename (left in place)", 0);
    if (fs->rename(fs->ctx, tmp, to) != NDP_OK) {
      (void)fs->rename(fs->ctx, tmp, from); /* put it back */
      return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "could not rename (left in place)", 0);
    }
  } else if (fs->rename(fs->ctx, from, to) != NDP_OK) {
    return ndp_agent_error(out, cap, req, NDP_ST_IO_ERROR, "could not rename (left in place)", 0);
  }

  ndp_tlv_w_init(&w, out + NDP_HEADER_SIZE, cap - NDP_HEADER_SIZE);
  ndp_tlv_put_str(&w, NDP_TAG_NEW_PATH, to);
  return ndp_agent_finish(out, cap, req, NDP_KIND_RES, NDP_OK, &w);
}
