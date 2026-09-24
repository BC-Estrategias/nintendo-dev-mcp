/* NDP v1 — constants shared by every module. See docs/protocol/ndp-v1.md. */
#ifndef NDP_DEFS_H
#define NDP_DEFS_H

#include <stddef.h>
#include <stdint.h>

#define NDP_AGENT_VERSION "0.1.0"
#define NDP_PROTOCOL_VERSION 1
#define NDP_HEADER_SIZE 20
#define NDP_MAC_SIZE 16
#define NDP_DEFAULT_PORT 6464
#define NDP_DEFAULT_MAX_FRAME 65536u

enum ndp_kind {
  NDP_KIND_REQ = 1,
  NDP_KIND_RES = 2,
  NDP_KIND_DATA = 3,
  NDP_KIND_END = 4,
  NDP_KIND_ERR = 5,
  NDP_KIND_EVT = 6
};

#define NDP_FLAG_MAC 0x0001u
#define NDP_FLAG_MORE 0x0002u
#define NDP_FLAGS_KNOWN (NDP_FLAG_MAC | NDP_FLAG_MORE)

enum ndp_command { NDP_CMD_HELLO = 0x0001, NDP_CMD_PING = 0x0002 };

/* Status codes (spec §6). Functions in this library return these directly. */
enum ndp_status {
  NDP_OK = 0,
  NDP_ST_UNSUPPORTED_PROTOCOL = 1,
  NDP_ST_UNAUTHORIZED = 2,
  NDP_ST_FORBIDDEN_MODE = 3,
  NDP_ST_PROTECTED_PATH = 4,
  NDP_ST_NOT_FOUND = 5,
  NDP_ST_EXISTS = 6,
  NDP_ST_IO_ERROR = 7,
  NDP_ST_NO_SPACE = 8,
  NDP_ST_BAD_REQUEST = 9,
  NDP_ST_BUSY = 10,
  NDP_ST_TOO_LARGE = 11,
  NDP_ST_HASH_MISMATCH = 12,
  NDP_ST_TIMEOUT = 13,
  NDP_ST_UNSUPPORTED_COMMAND = 14,
  NDP_ST_HELLO_REQUIRED = 15,
  NDP_ST_BAD_FRAME = 16,
  NDP_ST_PATH_INVALID = 17
};

/* TLV tags (spec §7). */
enum ndp_tag {
  NDP_TAG_DETAIL = 0x0001,
  NDP_TAG_OS_RESULT = 0x0002,
  NDP_TAG_PROTOCOL = 0x0010, /* HELLO REQ: protocol_min; HELLO RES: chosen version */
  NDP_TAG_PROTOCOL_MAX = 0x0011,
  NDP_TAG_BRIDGE_NAME = 0x0012,
  NDP_TAG_NONCE = 0x0013,
  NDP_TAG_PLATFORM = 0x0014,
  NDP_TAG_AGENT_VERSION = 0x0015,
  NDP_TAG_AUTH = 0x0016,
  NDP_TAG_MODE = 0x0017,
  NDP_TAG_MAX_FRAME = 0x0018,
  NDP_TAG_SUPPORTED_MIN = 0x0019,
  NDP_TAG_SUPPORTED_MAX = 0x001A,
  NDP_TAG_PING_NONCE = 0x0020
};

#endif
