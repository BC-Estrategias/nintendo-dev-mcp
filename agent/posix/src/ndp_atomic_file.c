#if !defined(_POSIX_C_SOURCE) && !defined(__3DS__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "ndp/ndp_atomic_file.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define PATH_LIMIT 320

/* >0 length, 0 missing, -1 present but invalid */
static long read_valid(const char *path, uint8_t *buf, size_t cap, ndp_file_verify verify) {
  FILE *f = fopen(path, "rb");
  size_t n;
  if (!f) return 0;
  n = fread(buf, 1, cap, f);
  fclose(f);
  if (n == 0 || verify(buf, n) != 0) return -1;
  return (long)n;
}

long ndp_atomic_load(const char *path, uint8_t *buf, size_t cap, ndp_file_verify verify) {
  char bak[PATH_LIMIT];
  long r = read_valid(path, buf, cap, verify), rb;
  if (r > 0) return r;
  if (strlen(path) + 5 >= sizeof bak) return r;
  snprintf(bak, sizeof bak, "%s.bak", path);
  rb = read_valid(bak, buf, cap, verify);
  if (rb > 0) return rb; /* the main file was lost or damaged: the previous one is fine */
  return r == -1 || rb == -1 ? -1 : 0;
}

int ndp_atomic_save(const char *path, const uint8_t *bytes, size_t len, ndp_file_verify verify) {
  char tmp[PATH_LIMIT], bak[PATH_LIMIT];
  uint8_t check[2048];
  FILE *f;
  if (len == 0 || len > sizeof check) return -EINVAL;
  if (strlen(path) + 5 >= sizeof tmp) return -ENAMETOOLONG;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  snprintf(bak, sizeof bak, "%s.bak", path);
  f = fopen(tmp, "wb");
  if (!f) return errno ? -errno : -EIO;
  if (fwrite(bytes, 1, len, f) != len || fflush(f) != 0) {
    int e = errno ? errno : EIO;
    fclose(f);
    remove(tmp);
    return -e;
  }
  fclose(f);
  if (read_valid(tmp, check, sizeof check, verify) != (long)len || memcmp(check, bytes, len) != 0) { /* what reached the medium */
    remove(tmp);
    return -EIO;
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
