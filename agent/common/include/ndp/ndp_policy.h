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

/* The built-in protected zones (the never_* defaults), without building a whole policy on the stack. */
int ndp_policy_default_zones(int write, const char *const **list);

/* Returns NDP_OK, NDP_ST_PATH_INVALID, NDP_ST_FORBIDDEN_MODE or NDP_ST_PROTECTED_PATH.
 * When `norm_out` is not NULL (NDP_PATH_MAX + 1 bytes) it receives the normalized path on success. */
int ndp_policy_check(const ndp_policy *p, ndp_mode mode, int is_write, const uint8_t *path, size_t path_len,
                     char *norm_out);

/* Directory traversal (spec §11.1): a directory that is a PROPER ANCESTOR of some read root (e.g. "/" or "/roms"
 * when "/roms/gba" is a read root) may be stat'ed and listed — the listing shows only the entries that lead to
 * (or are inside) a read root — so a client can navigate down to what it is allowed to read. Nothing else about
 * such a directory is readable. Returns 1 when `norm` (a normalized path) qualifies. */
int ndp_policy_traversable(const ndp_policy *p, const char *norm);

/* 1 when a listing may show the entry `norm`: it is inside a read root (and not in a never_read zone) or it is
 * traversable. Used to filter the listing of a traversable directory. */
int ndp_policy_child_visible(const ndp_policy *p, const char *norm);

#endif
