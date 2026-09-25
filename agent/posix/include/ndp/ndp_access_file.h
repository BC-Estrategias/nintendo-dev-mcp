/* Persistence of the owner's folder list (see ndp_access.h). */
#ifndef NDP_ACCESS_FILE_H
#define NDP_ACCESS_FILE_H

#include "ndp/ndp_access.h"

/* 0 = loaded; 1 = no file (an empty list is left in `a`: workspace only); -1 = corrupt (empty list, fail closed). */
int ndp_access_load_file(ndp_access *a, const char *path);
/* 0 or a negative errno-style value. */
int ndp_access_save_file(const ndp_access *a, const char *path);

#endif
