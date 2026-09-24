/* Human-readable names for logs (never sent on the wire). */
#ifndef NDP_NAMES_H
#define NDP_NAMES_H

#include "ndp/ndp_defs.h"

const char *ndp_command_name(uint16_t command);
const char *ndp_status_name(uint16_t status);

#endif
