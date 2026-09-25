/* The folders the console's owner has opened to the Bridge (spec §11.2). The owner edits this list ON THE
 * CONSOLE; no protocol command changes it. It is turned into the access policy (ndp_policy) that the agent
 * enforces. The agent's own workspace (/3ds/nintendo-dev-agent) is always readable and writable. */
#ifndef NDP_ACCESS_H
#define NDP_ACCESS_H

#include <stddef.h>
#include <stdint.h>

#include "ndp/ndp_policy.h"

#define NDP_WORKSPACE "/3ds/nintendo-dev-agent"
#define NDP_ACCESS_MAX 6 /* user entries; with the workspace the policy lists stay within NDP_POLICY_MAX_ENTRIES */

typedef enum { NDP_LVL_NONE = 0, NDP_LVL_READ = 1, NDP_LVL_WRITE = 2 } ndp_level; /* WRITE implies READ */

typedef struct {
  char path[NDP_POLICY_ENTRY_MAX]; /* normalized */
  uint8_t level;                   /* NDP_LVL_READ or NDP_LVL_WRITE */
} ndp_access_entry;

typedef struct {
  ndp_access_entry e[NDP_ACCESS_MAX];
  int count;
} ndp_access;

void ndp_access_init(ndp_access *a);

/* The level `norm` has today: the highest level among the entries that contain it (the workspace counts as
 * WRITE). `*explicit_level` (may be NULL) receives the level of an entry for exactly this path, or NONE. */
ndp_level ndp_access_level(const ndp_access *a, const char *norm, ndp_level *explicit_level);

/* Sets the level of exactly `norm` (NONE removes the entry). Returns NDP_OK, NDP_ST_PATH_INVALID,
 * NDP_ST_PROTECTED_PATH (READ inside a never_read zone, WRITE inside a never_write zone) or NDP_ST_NO_SPACE. */
int ndp_access_set(ndp_access *a, const char *path, ndp_level level);

/* The next level in the owner's cycle for `norm` (NONE -> READ -> WRITE -> NONE), skipping levels that are
 * redundant (already granted by a parent) or not allowed (protected zones). */
ndp_level ndp_access_next(const ndp_access *a, const char *norm);

/* 1 if `level` may be granted on `norm` at all (protected zones refuse). */
int ndp_access_allowed(const char *norm, ndp_level level);

/* Builds the policy: default protected zones + workspace + the entries (write roots are also read roots). */
void ndp_access_to_policy(const ndp_access *a, ndp_policy *p);

/* "NDPA" v1 file: magic, version, count, then {level, len, path}, then SHA-256 of everything before. */
#define NDP_ACCESS_MAX_BYTES (8 + NDP_ACCESS_MAX * (2 + NDP_POLICY_ENTRY_MAX) + 32)
size_t ndp_access_serialize(const ndp_access *a, uint8_t *out, size_t cap); /* 0 when cap is too small */
/* 0 ok; -1 on ANY inconsistency (the list is then left empty: fail closed to the workspace only). */
int ndp_access_parse(ndp_access *a, const uint8_t *in, size_t len);

#endif
