#include "ndp/ndp_access.h"

#include <string.h>

#include "ndp/ndp_sha256.h"

void ndp_access_init(ndp_access *a) { memset(a, 0, sizeof *a); }

static int find_entry(const ndp_access *a, const char *norm) {
  int i;
  for (i = 0; i < a->count; i++)
    if (ndp_path_inside(norm, a->e[i].path) && ndp_path_inside(a->e[i].path, norm)) return i; /* equal (ASCII case-fold) */
  return -1;
}

/* Highest level given to `norm` by the workspace and by entries other than `skip` (-1 = none skipped). */
static ndp_level level_from(const ndp_access *a, const char *norm, int skip) {
  ndp_level best = NDP_LVL_NONE;
  int i;
  if (ndp_path_inside(norm, NDP_WORKSPACE)) best = NDP_LVL_WRITE;
  for (i = 0; i < a->count; i++)
    if (i != skip && ndp_path_inside(norm, a->e[i].path) && a->e[i].level > best) best = (ndp_level)a->e[i].level;
  return best;
}

ndp_level ndp_access_level(const ndp_access *a, const char *norm, ndp_level *explicit_level) {
  int i = find_entry(a, norm);
  ndp_level eff = level_from(a, norm, -1);
  if (explicit_level) *explicit_level = i >= 0 ? (ndp_level)a->e[i].level : NDP_LVL_NONE;
  /* what the agent will really allow: a parent's WRITE does not reach protected zones (spec §11) */
  if (eff == NDP_LVL_WRITE && !ndp_access_allowed(norm, NDP_LVL_WRITE)) eff = NDP_LVL_READ;
  if (eff != NDP_LVL_NONE && !ndp_access_allowed(norm, NDP_LVL_READ)) eff = NDP_LVL_NONE;
  return eff;
}

/* Uses the constant zone tables: this runs for every row the console draws, and building a policy here would put
 * ~8 KB on the (32 KB) stack of the 3DS's main thread. */
int ndp_access_allowed(const char *norm, ndp_level level) {
  const char *const *z, *const *x;
  int i, n, nx;
  if (level == NDP_LVL_NONE) return 1;
  n = ndp_policy_default_zones(0, &z);
  for (i = 0; i < n; i++)
    if (ndp_path_inside(norm, z[i])) return 0;
  if (level == NDP_LVL_WRITE) {
    n = ndp_policy_default_zones(1, &z);
    nx = ndp_policy_default_zones(2, &x);
    if (ndp_write_protected(z, n, x, nx, norm)) return 0;
  }
  return 1;
}

int ndp_access_set(ndp_access *a, const char *path, ndp_level level) {
  char norm[NDP_PATH_MAX + 1];
  int rc, i;
  if (level > NDP_LVL_WRITE) return NDP_ST_BAD_REQUEST;
  rc = ndp_path_normalize((const uint8_t *)path, strlen(path), norm, sizeof norm);
  if (rc != NDP_OK) return rc;
  if (strlen(norm) >= NDP_POLICY_ENTRY_MAX) return NDP_ST_TOO_LARGE;
  if (!ndp_access_allowed(norm, level)) return NDP_ST_PROTECTED_PATH;
  i = find_entry(a, norm);
  if (level == NDP_LVL_NONE) {
    if (i >= 0) {
      memmove(&a->e[i], &a->e[i + 1], (size_t)(a->count - i - 1) * sizeof a->e[0]);
      a->count--;
      memset(&a->e[a->count], 0, sizeof a->e[0]);
    }
    return NDP_OK;
  }
  if (i < 0) {
    if (a->count >= NDP_ACCESS_MAX) return NDP_ST_NO_SPACE;
    i = a->count++;
    strcpy(a->e[i].path, norm);
  }
  a->e[i].level = (uint8_t)level;
  return NDP_OK;
}

ndp_level ndp_access_next(const ndp_access *a, const char *norm) {
  ndp_level exp, inh, opt[3];
  int n = 0, i;
  (void)ndp_access_level(a, norm, &exp);
  inh = level_from(a, norm, find_entry(a, norm));
  opt[n++] = NDP_LVL_NONE;
  if (inh < NDP_LVL_READ && ndp_access_allowed(norm, NDP_LVL_READ)) opt[n++] = NDP_LVL_READ;
  if (inh < NDP_LVL_WRITE && ndp_access_allowed(norm, NDP_LVL_WRITE)) opt[n++] = NDP_LVL_WRITE;
  for (i = 0; i < n; i++)
    if (opt[i] == exp) return i + 1 < n ? opt[i + 1] : NDP_LVL_NONE;
  return NDP_LVL_NONE; /* the current explicit level is not among the useful ones (e.g. now redundant) */
}

void ndp_access_to_policy(const ndp_access *a, ndp_policy *p) {
  int i;
  ndp_policy_init_default(p);
  p->read_roots.count = 0;
  p->write_roots.count = 0;
  (void)ndp_pathlist_add(&p->read_roots, NDP_WORKSPACE);
  (void)ndp_pathlist_add(&p->write_roots, NDP_WORKSPACE);
  for (i = 0; i < a->count; i++) {
    (void)ndp_pathlist_add(&p->read_roots, a->e[i].path);
    if (a->e[i].level == NDP_LVL_WRITE) (void)ndp_pathlist_add(&p->write_roots, a->e[i].path);
  }
}

size_t ndp_access_serialize(const ndp_access *a, uint8_t *out, size_t cap) {
  size_t o = 8;
  int i;
  if (cap < NDP_ACCESS_MAX_BYTES) return 0;
  memcpy(out, "NDPA", 4);
  out[4] = 1;
  out[5] = (uint8_t)a->count;
  out[6] = out[7] = 0;
  for (i = 0; i < a->count; i++) {
    size_t l = strlen(a->e[i].path);
    out[o++] = a->e[i].level;
    out[o++] = (uint8_t)l;
    memcpy(out + o, a->e[i].path, l);
    o += l;
  }
  ndp_sha256(out, o, out + o);
  return o + 32;
}

int ndp_access_parse(ndp_access *a, const uint8_t *in, size_t len) {
  size_t o = 8;
  int count, i;
  uint8_t h[32];
  ndp_access_init(a);
  if (len < 8 + 32 || memcmp(in, "NDPA", 4) != 0 || in[4] != 1 || in[6] != 0 || in[7] != 0) return -1;
  count = in[5];
  if (count > NDP_ACCESS_MAX) return -1;
  for (i = 0; i < count; i++) { /* walk the entries to find where the body ends */
    if (o + 2 > len) return -1;
    o += 2 + in[o + 1];
  }
  if (o + 32 != len) return -1;
  ndp_sha256(in, o, h);
  if (!ndp_ct_equal(h, in + o, 32)) return -1;
  o = 8;
  for (i = 0; i < count; i++) {
    char path[NDP_POLICY_ENTRY_MAX], norm[NDP_PATH_MAX + 1];
    size_t l = in[o + 1];
    uint8_t level = in[o];
    if ((level != NDP_LVL_READ && level != NDP_LVL_WRITE) || l == 0) { ndp_access_init(a); return -1; }
    memcpy(path, in + o + 2, l);
    path[l] = '\0';
    if (ndp_path_normalize((const uint8_t *)path, l, norm, sizeof norm) != NDP_OK || strcmp(norm, path) != 0 ||
        find_entry(a, norm) >= 0 || ndp_access_set(a, norm, (ndp_level)level) != NDP_OK) {
      ndp_access_init(a);
      return -1;
    }
    o += 2 + l;
  }
  return 0;
}
