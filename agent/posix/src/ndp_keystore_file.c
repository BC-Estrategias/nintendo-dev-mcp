#include "ndp/ndp_keystore_file.h"

#include "ndp/ndp_atomic_file.h"

static int verify(const uint8_t *b, size_t n) {
  ndp_keystore tmp;
  return ndp_keystore_parse(&tmp, b, n);
}

int ndp_keystore_load_file(ndp_keystore *ks, const char *path) {
  uint8_t buf[NDP_KEYSTORE_MAX_BYTES + 8];
  long n = ndp_atomic_load(path, buf, sizeof buf, verify);
  if (n > 0 && ndp_keystore_parse(ks, buf, (size_t)n) == 0) return 0;
  ndp_keystore_clear(ks);
  return n == 0 ? 1 : -1;
}

int ndp_keystore_save_file(const ndp_keystore *ks, const char *path) {
  uint8_t buf[NDP_KEYSTORE_MAX_BYTES];
  size_t n = ndp_keystore_serialize(ks, buf, sizeof buf);
  return n == 0 ? -22 : ndp_atomic_save(path, buf, n, verify);
}
