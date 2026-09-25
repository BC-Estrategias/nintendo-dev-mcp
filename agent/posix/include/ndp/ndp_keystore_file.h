/* Persistence of the key store in a file (POSIX/newlib: the host agent and the 3DS "sdmc:" card). */
#ifndef NDP_KEYSTORE_FILE_H
#define NDP_KEYSTORE_FILE_H

#include "ndp/ndp_auth.h"

/* 0 = loaded; 1 = no file (a fresh store is left in `ks`); -1 = a file exists but is corrupt (the
 * backup is tried first; if both fail `ks` is empty). */
int ndp_keystore_load_file(ndp_keystore *ks, const char *path);

/* Atomic-ish save: writes "<path>.tmp", keeps the previous file as "<path>.bak", then renames the new one in.
 * Returns 0, or a negative errno-style value. The parent folder must exist. */
int ndp_keystore_save_file(const ndp_keystore *ks, const char *path);

#endif
