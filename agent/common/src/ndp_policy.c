#include "ndp/ndp_policy.h"

#include <string.h>

int ndp_pathlist_add(ndp_pathlist *l, const char *path) {
  char norm[NDP_PATH_MAX + 1];
  size_t n = strlen(path);
  int rc;
  if (l->count >= NDP_POLICY_MAX_ENTRIES) return NDP_ST_TOO_LARGE;
  rc = ndp_path_normalize((const uint8_t *)path, n, norm, sizeof norm);
  if (rc != NDP_OK) return rc;
  if (strlen(norm) >= NDP_POLICY_ENTRY_MAX) return NDP_ST_TOO_LARGE;
  strcpy(l->entries[l->count++], norm);
  return NDP_OK;
}

static const char *const DEFAULT_NEVER_READ[] = {"/3ds/nintendo-dev-agent/config"};
static const char *const DEFAULT_NEVER_WRITE[] = {"/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private",
                                                   "/3ds/nintendo-dev-agent/config"};
/* Plugins (3GX) and per-title file replacements (layeredfs) are what a developer edits under /luma; the rest of
 * it (config.ini, payloads, sysmodules, ...) can stop the console from booting. */
static const char *const DEFAULT_WRITE_EXCEPT[] = {"/luma/plugins", "/luma/titles"};

int ndp_policy_default_zones(int which, const char *const **list) {
  if (which == 2) { *list = DEFAULT_WRITE_EXCEPT; return (int)(sizeof DEFAULT_WRITE_EXCEPT / sizeof DEFAULT_WRITE_EXCEPT[0]); }
  if (which == 1) { *list = DEFAULT_NEVER_WRITE; return (int)(sizeof DEFAULT_NEVER_WRITE / sizeof DEFAULT_NEVER_WRITE[0]); }
  *list = DEFAULT_NEVER_READ;
  return (int)(sizeof DEFAULT_NEVER_READ / sizeof DEFAULT_NEVER_READ[0]);
}

int ndp_write_protected(const char *const *zones, int nz, const char *const *exc, int ne, const char *norm) {
  int i, j;
  for (i = 0; i < nz; i++) {
    int lifted = 0;
    if (!ndp_path_inside(norm, zones[i])) continue;
    for (j = 0; j < ne && !lifted; j++)
      lifted = ndp_path_inside(norm, exc[j]) && ndp_path_inside(exc[j], zones[i]) && !ndp_path_inside(zones[i], exc[j]);
    if (!lifted) return 1;
  }
  return 0;
}

void ndp_policy_init_default(ndp_policy *p) {
  const char *const *z;
  int i, n;
  memset(p, 0, sizeof *p);
  (void)ndp_pathlist_add(&p->read_roots, "/");
  (void)ndp_pathlist_add(&p->write_roots, "/3ds/nintendo-dev-agent");
  n = ndp_policy_default_zones(0, &z);
  for (i = 0; i < n; i++) (void)ndp_pathlist_add(&p->never_read, z[i]);
  n = ndp_policy_default_zones(1, &z);
  for (i = 0; i < n; i++) (void)ndp_pathlist_add(&p->never_write, z[i]);
  n = ndp_policy_default_zones(2, &z);
  for (i = 0; i < n; i++) (void)ndp_pathlist_add(&p->write_except, z[i]);
}

static int in_any(const ndp_pathlist *l, const char *path) {
  int i;
  for (i = 0; i < l->count; i++)
    if (ndp_path_inside(path, l->entries[i])) return 1;
  return 0;
}

int ndp_policy_check(const ndp_policy *p, ndp_mode mode, int is_write, const uint8_t *path, size_t path_len,
                     char *norm_out) {
  char norm[NDP_PATH_MAX + 1];
  int rc = ndp_path_normalize(path, path_len, norm, sizeof norm);
  if (rc != NDP_OK) return rc;
  if (is_write && mode == NDP_MODE_READ_ONLY) return NDP_ST_FORBIDDEN_MODE;
  if (is_write) {
    const char *zones[NDP_POLICY_MAX_ENTRIES], *exc[NDP_POLICY_MAX_ENTRIES];
    int i;
    for (i = 0; i < p->never_write.count; i++) zones[i] = p->never_write.entries[i];
    for (i = 0; i < p->write_except.count; i++) exc[i] = p->write_except.entries[i];
    if (ndp_write_protected(zones, p->never_write.count, exc, p->write_except.count, norm)) return NDP_ST_PROTECTED_PATH;
  } else if (in_any(&p->never_read, norm)) {
    return NDP_ST_PROTECTED_PATH;
  }
  if (!in_any(is_write ? &p->write_roots : &p->read_roots, norm)) return NDP_ST_PROTECTED_PATH;
  if (norm_out) strcpy(norm_out, norm);
  return NDP_OK;
}

int ndp_policy_traversable(const ndp_policy *p, const char *norm) {
  int i;
  if (in_any(&p->never_read, norm)) return 0;
  for (i = 0; i < p->read_roots.count; i++) {
    const char *r = p->read_roots.entries[i];
    if (ndp_path_inside(r, norm) && !ndp_path_inside(norm, r)) return 1; /* norm is a proper ancestor of r */
  }
  return 0;
}

int ndp_policy_child_visible(const ndp_policy *p, const char *norm) {
  if (in_any(&p->never_read, norm)) return 0;
  return in_any(&p->read_roots, norm) || ndp_policy_traversable(p, norm);
}
