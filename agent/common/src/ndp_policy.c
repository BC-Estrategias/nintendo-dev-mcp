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

void ndp_policy_init_default(ndp_policy *p) {
  memset(p, 0, sizeof *p);
  (void)ndp_pathlist_add(&p->read_roots, "/");
  (void)ndp_pathlist_add(&p->write_roots, "/3ds/nintendo-dev-agent");
  (void)ndp_pathlist_add(&p->never_read, "/3ds/nintendo-dev-agent/config");
  (void)ndp_pathlist_add(&p->never_write, "/Nintendo 3DS");
  (void)ndp_pathlist_add(&p->never_write, "/luma");
  (void)ndp_pathlist_add(&p->never_write, "/boot.firm");
  (void)ndp_pathlist_add(&p->never_write, "/gm9");
  (void)ndp_pathlist_add(&p->never_write, "/private");
  (void)ndp_pathlist_add(&p->never_write, "/3ds/nintendo-dev-agent/config");
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
  if (in_any(is_write ? &p->never_write : &p->never_read, norm)) return NDP_ST_PROTECTED_PATH;
  if (!in_any(is_write ? &p->write_roots : &p->read_roots, norm)) return NDP_ST_PROTECTED_PATH;
  if (norm_out) strcpy(norm_out, norm);
  return NDP_OK;
}
