#if !defined(_POSIX_C_SOURCE) && !defined(__3DS__)
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "ndp/ndp_posix_fs.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __3DS__
#define lstat stat /* the sdmc devoptab has no symlinks */
#endif

#define FULL_MAX 1400

static int map_errno(int e) {
  switch (e) {
    case ENOENT: case ENOTDIR: return NDP_ST_NOT_FOUND;
    case ENOSPC: return NDP_ST_NO_SPACE;
    default: return NDP_ST_IO_ERROR;
  }
}

#ifdef __3DS__
static int contained(const ndp_posix_fs_ctx *c, const char *full) { (void)c; (void)full; return 1; } /* FAT: no symlinks */
#else
/* lstat() only looks at the LAST path component, so a symlinked directory inside the root could lead
 * outside it. The real path of the containing directory must stay inside the real root. */
static int contained(const ndp_posix_fs_ctx *c, const char *full) {
  char dir[FULL_MAX], real_dir[PATH_MAX], real_root[PATH_MAX];
  const char *slash = strrchr(full, '/');
  size_t n, rl;
  if (!slash) return 0;
  n = (size_t)(slash - full);
  if (n >= sizeof dir) return 0;
  memcpy(dir, full, n);
  dir[n] = '\0';
  if (!realpath(n ? dir : "/", real_dir) || !realpath(c->root[0] ? c->root : "/", real_root)) return 0;
  rl = strlen(real_root);
  if (rl == 1) return 1; /* root is "/" */
  return strncmp(real_dir, real_root, rl) == 0 && (real_dir[rl] == '\0' || real_dir[rl] == '/');
}
#endif

static int full_path(const ndp_posix_fs_ctx *c, const char *path, char *out, size_t cap) {
  int n = snprintf(out, cap, "%s%s", c->root, path);
  if (!(n > 0 && (size_t)n < cap)) return -1;
  return contained(c, out) ? 0 : -1;
}

/* Only regular files and directories exist for the protocol. */
static int do_lstat(const char *full, ndp_fs_stat *st) {
  struct stat sb;
  if (lstat(full, &sb) != 0) return map_errno(errno);
  if (S_ISDIR(sb.st_mode)) st->is_dir = 1;
  else if (S_ISREG(sb.st_mode)) st->is_dir = 0;
  else return NDP_ST_NOT_FOUND;
  st->size = st->is_dir ? 0 : (uint64_t)sb.st_size;
  st->mtime = sb.st_mtime > 0 ? (uint64_t)sb.st_mtime : 0;
  return NDP_OK;
}

static int p_stat(void *ctx, const char *path, ndp_fs_stat *st) {
  char full[FULL_MAX];
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  return do_lstat(full, st);
}

typedef struct {
  DIR *dp;
  char base[FULL_MAX]; /* directory path without trailing slash */
} dir_handle;

static int p_dir_open(void *ctx, const char *path, void **out) {
  static dir_handle h; /* one directory open at a time (the agent keeps at most one) */
  char full[FULL_MAX];
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  h.dp = opendir(full);
  if (!h.dp) return map_errno(errno);
  {
    size_t n = strlen(full);
    while (n > 1 && full[n - 1] == '/') n--;
    if (n >= sizeof h.base) { closedir(h.dp); return NDP_ST_PATH_INVALID; }
    memcpy(h.base, full, n);
    h.base[n] = '\0';
  }
  *out = &h;
  return NDP_OK;
}

static int p_dir_next(void *ctx, void *handle, char *name, size_t name_cap, ndp_fs_stat *st) {
  dir_handle *h = (dir_handle *)handle;
  (void)ctx;
  for (;;) {
    struct dirent *de;
    char full[FULL_MAX];
    size_t nl;
    errno = 0;
    de = readdir(h->dp);
    if (!de) return errno ? -NDP_ST_IO_ERROR : 0;
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    nl = strlen(de->d_name);
    if (nl > 255 || nl >= name_cap) continue; /* not addressable by the protocol (spec §10) */
    if (snprintf(full, sizeof full, "%s/%s", h->base, de->d_name) >= (int)sizeof full) continue;
    if (do_lstat(full, st) != NDP_OK) continue; /* vanished, or not a regular file/dir */
    memcpy(name, de->d_name, nl + 1);
    return 1;
  }
}

static void p_dir_close(void *ctx, void *handle) {
  (void)ctx;
  closedir(((dir_handle *)handle)->dp);
}

static int p_file_open(void *ctx, const char *path, void **out) {
  char full[FULL_MAX];
  FILE *fp;
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  fp = fopen(full, "rb");
  if (!fp) return map_errno(errno);
  *out = fp;
  return NDP_OK;
}

static int p_file_seek(void *ctx, void *file, uint64_t offset) {
  (void)ctx;
  return fseeko((FILE *)file, (off_t)offset, SEEK_SET) == 0 ? NDP_OK : NDP_ST_IO_ERROR;
}

static long p_file_read(void *ctx, void *file, void *buf, size_t n) {
  size_t r;
  (void)ctx;
  r = fread(buf, 1, n, (FILE *)file);
  if (r == 0 && ferror((FILE *)file)) return -1;
  return (long)r;
}

static void p_file_close(void *ctx, void *file) {
  (void)ctx;
  fclose((FILE *)file);
}

/* ---- write side ---- */
static int p_mkdir(void *ctx, const char *path) {
  char full[FULL_MAX];
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  if (mkdir(full, 0777) == 0) return NDP_OK;
  return errno == EEXIST ? NDP_ST_EXISTS : map_errno(errno);
}

static int p_file_create(void *ctx, const char *path, void **out) {
  char full[FULL_MAX];
  FILE *fp;
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  fp = fopen(full, "wb");
  if (!fp) return map_errno(errno);
  *out = fp;
  return NDP_OK;
}

static long p_file_write(void *ctx, void *file, const void *buf, size_t n) {
  size_t w;
  (void)ctx;
  w = fwrite(buf, 1, n, (FILE *)file);
  if (w == n) return (long)w;
  return errno == ENOSPC ? -NDP_ST_NO_SPACE : -NDP_ST_IO_ERROR;
}

static int p_file_sync(void *ctx, void *file) {
  (void)ctx;
  if (fflush((FILE *)file) != 0) return errno == ENOSPC ? NDP_ST_NO_SPACE : NDP_ST_IO_ERROR;
#ifndef __3DS__
  if (fsync(fileno((FILE *)file)) != 0) return NDP_ST_IO_ERROR;
#endif
  return NDP_OK;
}

static int p_rename(void *ctx, const char *from, const char *to) {
  char a[FULL_MAX], b[FULL_MAX];
  if (full_path((const ndp_posix_fs_ctx *)ctx, from, a, sizeof a) != 0 ||
      full_path((const ndp_posix_fs_ctx *)ctx, to, b, sizeof b) != 0)
    return NDP_ST_NOT_FOUND;
  return rename(a, b) == 0 ? NDP_OK : map_errno(errno);
}

static int p_remove(void *ctx, const char *path) {
  char full[FULL_MAX];
  if (full_path((const ndp_posix_fs_ctx *)ctx, path, full, sizeof full) != 0) return NDP_ST_NOT_FOUND;
  return unlink(full) == 0 ? NDP_OK : map_errno(errno);
}

int ndp_posix_fs_init(ndp_fs_ops *ops, ndp_posix_fs_ctx *ctx, const char *root) {
  size_t n = strlen(root);
  if (n >= sizeof ctx->root) return -1;
  memcpy(ctx->root, root, n + 1);
  memset(ops, 0, sizeof *ops);
  ops->ctx = ctx;
  ops->stat = p_stat;
  ops->dir_open = p_dir_open;
  ops->dir_next = p_dir_next;
  ops->dir_close = p_dir_close;
  ops->file_open = p_file_open;
  ops->file_seek = p_file_seek;
  ops->file_read = p_file_read;
  ops->file_close = p_file_close;
  ops->mkdir = p_mkdir;
  ops->file_create = p_file_create;
  ops->file_write = p_file_write;
  ops->file_sync = p_file_sync;
  ops->rename = p_rename;
  ops->remove_file = p_remove;
  return 0;
}
