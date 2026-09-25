/* Filesystem interface the platform provides to the agent core (read-only for now).
 * Paths passed in are already normalized protocol paths ("/3ds/x"). Error returns are NDP_ST_* codes. */
#ifndef NDP_FS_H
#define NDP_FS_H

#include "ndp/ndp_defs.h"

typedef struct {
  int is_dir;
  uint64_t size;
  uint64_t mtime; /* seconds since the Unix epoch, 0 = unknown */
} ndp_fs_stat;

typedef struct {
  void *ctx;
  /* 0 or NDP_ST_NOT_FOUND / NDP_ST_IO_ERROR. Only regular files and directories exist. */
  int (*stat)(void *ctx, const char *path, ndp_fs_stat *st);
  int (*dir_open)(void *ctx, const char *path, void **dir);
  /* 1 = entry filled in (never "." or ".."; names > 255 bytes are skipped), 0 = end, negative = -NDP_ST_*. */
  int (*dir_next)(void *ctx, void *dir, char *name, size_t name_cap, ndp_fs_stat *st);
  void (*dir_close)(void *ctx, void *dir);
  int (*file_open)(void *ctx, const char *path, void **file); /* read-only */
  int (*file_seek)(void *ctx, void *file, uint64_t offset);
  /* bytes read (> 0), 0 at end of file, negative on error */
  long (*file_read)(void *ctx, void *file, void *buf, size_t n);
  void (*file_close)(void *ctx, void *file);

  /* Write side. All optional: when file_create is NULL the agent answers UNSUPPORTED_COMMAND to
   * FS_WRITE/FS_MKDIR. */
  int (*mkdir)(void *ctx, const char *path);                      /* 0, NDP_ST_EXISTS, NDP_ST_NOT_FOUND... */
  int (*file_create)(void *ctx, const char *path, void **file);   /* new file for writing (truncates) */
  long (*file_write)(void *ctx, void *file, const void *buf, size_t n); /* n, or -NDP_ST_* (e.g. NO_SPACE) */
  int (*file_sync)(void *ctx, void *file);                        /* flush to the medium */
  int (*rename)(void *ctx, const char *from, const char *to);
  int (*remove_file)(void *ctx, const char *path);                /* internal cleanup only; never exposed */
} ndp_fs_ops;

#endif
