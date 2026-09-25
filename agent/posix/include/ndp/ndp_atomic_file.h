/* Small config files that must never be half-written: write "<path>.tmp", verify it, keep the previous file as
 * "<path>.bak", rename. Loading falls back to the backup. Used for the key store and the access list. */
#ifndef NDP_ATOMIC_FILE_H
#define NDP_ATOMIC_FILE_H

#include <stddef.h>
#include <stdint.h>

/* Returns 0 when the bytes are a valid file of the caller's format. */
typedef int (*ndp_file_verify)(const uint8_t *bytes, size_t len);

/* 0 = ok; -EINVAL/-errno on failure. The parent folder must exist. */
int ndp_atomic_save(const char *path, const uint8_t *bytes, size_t len, ndp_file_verify verify);

/* Reads `path` (else "<path>.bak") into buf. Returns the length (>0), 0 when neither file exists, or -1 when
 * files exist but none is valid. */
long ndp_atomic_load(const char *path, uint8_t *buf, size_t cap, ndp_file_verify verify);

#endif
