/* Path normalization and prefix comparison (spec §9). */
#ifndef NDP_PATH_H
#define NDP_PATH_H

#include "ndp/ndp_defs.h"

#define NDP_PATH_MAX 1024
#define NDP_COMPONENT_MAX 255

/* Validates and normalizes `in` (arbitrary bytes, not NUL-terminated) into `out`
 * (NUL-terminated, at least NDP_PATH_MAX + 1 bytes). Returns NDP_OK or NDP_ST_PATH_INVALID. */
int ndp_path_normalize(const uint8_t *in, size_t len, char *out, size_t out_cap);

/* Both arguments must be normalized. Returns 1 when `path` equals or is inside `root`
 * (component-wise, ASCII-only case folding; non-ASCII bytes must match exactly). */
int ndp_path_inside(const char *path, const char *root);

#endif
