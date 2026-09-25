#include "ndp/ndp_access_file.h"

#include "ndp/ndp_atomic_file.h"

static int verify(const uint8_t *b, size_t n) {
  ndp_access tmp;
  return ndp_access_parse(&tmp, b, n);
}

int ndp_access_load_file(ndp_access *a, const char *path) {
  uint8_t buf[NDP_ACCESS_MAX_BYTES + 8];
  long n = ndp_atomic_load(path, buf, sizeof buf, verify);
  if (n > 0 && ndp_access_parse(a, buf, (size_t)n) == 0) return 0;
  ndp_access_init(a);
  return n == 0 ? 1 : -1;
}

int ndp_access_save_file(const ndp_access *a, const char *path) {
  uint8_t buf[NDP_ACCESS_MAX_BYTES];
  size_t n = ndp_access_serialize(a, buf, sizeof buf);
  return n == 0 ? -22 : ndp_atomic_save(path, buf, n, verify);
}
