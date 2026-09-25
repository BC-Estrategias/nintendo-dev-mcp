/* ndp_fs_ops on top of POSIX/newlib file APIs. `root` is prepended to every protocol path:
 * the 3DS uses "sdmc:" ("/3ds/x" -> "sdmc:/3ds/x"); the host agent uses a sandbox directory. */
#ifndef NDP_POSIX_FS_H
#define NDP_POSIX_FS_H

#include "ndp/ndp_fs.h"

typedef struct {
  char root[256];
} ndp_posix_fs_ctx;

/* Fills `ops` (ops->ctx = ctx). `root` must not end with '/'. Returns 0, or -1 if root is too long. */
int ndp_posix_fs_init(ndp_fs_ops *ops, ndp_posix_fs_ctx *ctx, const char *root);

#endif
