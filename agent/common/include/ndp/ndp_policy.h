/* Access policy: modes, read/write roots and protected zones (spec §10). */
#ifndef NDP_POLICY_H
#define NDP_POLICY_H

#include "ndp/ndp_defs.h"
#include "ndp/ndp_path.h"

typedef enum { NDP_MODE_READ_ONLY = 0, NDP_MODE_DEVELOPMENT = 1, NDP_MODE_FULL = 2 } ndp_mode;

#define NDP_POLICY_MAX_ENTRIES 8
#define NDP_POLICY_ENTRY_MAX 256

typedef struct {
  char entries[NDP_POLICY_MAX_ENTRIES][NDP_POLICY_ENTRY_MAX];
  int count;
} ndp_pathlist;

typedef struct {
  ndp_pathlist read_roots;
  ndp_pathlist write_roots;
  ndp_pathlist never_read;
  ndp_pathlist never_write;
} ndp_policy;

/* Normalizes and appends `path`. Returns NDP_OK, NDP_ST_PATH_INVALID or NDP_ST_TOO_LARGE. */
int ndp_pathlist_add(ndp_pathlist *l, const char *path);

/* Spec §10 defaults: read "/", write "/3ds/nintendo-dev-agent", plus the never_* zones. */
void ndp_policy_init_default(ndp_policy *p);

/* Returns NDP_OK, NDP_ST_PATH_INVALID, NDP_ST_FORBIDDEN_MODE or NDP_ST_PROTECTED_PATH.
 * When `norm_out` is not NULL (NDP_PATH_MAX + 1 bytes) it receives the normalized path on success. */
int ndp_policy_check(const ndp_policy *p, ndp_mode mode, int is_write, const uint8_t *path, size_t path_len,
                     char *norm_out);

#endif
