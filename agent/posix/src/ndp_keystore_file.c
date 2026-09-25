#if !defined(_POSIX_C_SOURCE) && !defined(__3DS__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ndp/ndp_keystore_file.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PATH_LIMIT 320

static int read_store(ndp_keystore *ks, const char *path) { /* 0 ok, 1 missing, -1 corrupt */
  uint8_t buf[NDP_KEYSTORE_MAX_BYTES + 8];
  FILE *f = fopen(path, "rb");
  size_t n;
  if (!f) return 1;
  n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  return ndp_keystore_parse(ks, buf, n) == 0 ? 0 : -1;
}

int ndp_keystore_load_file(ndp_keystore *ks, const char *path) {
  char bak[PATH_LIMIT];
  int rc = read_store(ks, path);
  if (rc == 0) return 0;
  if (strlen(path) + 5 >= sizeof bak) return rc;
  snprintf(bak, sizeof bak, "%s.bak", path);
  if (read_store(ks, bak) == 0) return 0; /* the main file was lost or damaged: the previous one is fine */
  ndp_keystore_clear(ks);
  return rc;
}

int ndp_keystore_save_file(const ndp_keystore *ks, const char *path) {
  char tmp[PATH_LIMIT], bak[PATH_LIMIT];
  uint8_t buf[NDP_KEYSTORE_MAX_BYTES];
  size_t n = ndp_keystore_serialize(ks, buf, sizeof buf);
  FILE *f;
  if (n == 0) return -EINVAL;
  if (strlen(path) + 5 >= sizeof tmp) return -ENAMETOOLONG;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  snprintf(bak, sizeof bak, "%s.bak", path);
  f = fopen(tmp, "wb");
  if (!f) return errno ? -errno : -EIO;
  if (fwrite(buf, 1, n, f) != n || fflush(f) != 0) {
    int e = errno ? errno : EIO;
    fclose(f);
    remove(tmp);
    return -e;
  }
  fclose(f);
  { /* verify what reached the medium before replacing anything */
    ndp_keystore check;
    if (read_store(&check, tmp) != 0) { remove(tmp); return -EIO; }
  }
  remove(bak);
  (void)rename(path, bak); /* fine if there was no previous file */
  if (rename(tmp, path) != 0) {
    int e = errno ? errno : EIO;
    (void)rename(bak, path); /* put the old one back */
    remove(tmp);
    return -e;
  }
  return 0;
}
