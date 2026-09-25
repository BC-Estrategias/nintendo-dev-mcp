#include "ndp/ndp_names.h"

const char *ndp_command_name(uint16_t command) {
  switch (command) {
    case NDP_CMD_HELLO: return "HELLO";
    case NDP_CMD_PING: return "PING";
    case NDP_CMD_FS_LIST: return "FS_LIST";
    case NDP_CMD_FS_STAT: return "FS_STAT";
    case NDP_CMD_FS_READ: return "FS_READ";
    case NDP_CMD_FS_WRITE: return "FS_WRITE";
    case NDP_CMD_FS_MKDIR: return "FS_MKDIR";
    default: return "?";
  }
}

const char *ndp_status_name(uint16_t status) {
  switch (status) {
    case NDP_OK: return "OK";
    case NDP_ST_UNSUPPORTED_PROTOCOL: return "UNSUPPORTED_PROTOCOL";
    case NDP_ST_UNAUTHORIZED: return "UNAUTHORIZED";
    case NDP_ST_FORBIDDEN_MODE: return "FORBIDDEN_MODE";
    case NDP_ST_PROTECTED_PATH: return "PROTECTED_PATH";
    case NDP_ST_NOT_FOUND: return "NOT_FOUND";
    case NDP_ST_EXISTS: return "EXISTS";
    case NDP_ST_IO_ERROR: return "IO_ERROR";
    case NDP_ST_NO_SPACE: return "NO_SPACE";
    case NDP_ST_BAD_REQUEST: return "BAD_REQUEST";
    case NDP_ST_BUSY: return "BUSY";
    case NDP_ST_TOO_LARGE: return "TOO_LARGE";
    case NDP_ST_HASH_MISMATCH: return "HASH_MISMATCH";
    case NDP_ST_TIMEOUT: return "TIMEOUT";
    case NDP_ST_UNSUPPORTED_COMMAND: return "UNSUPPORTED_COMMAND";
    case NDP_ST_HELLO_REQUIRED: return "HELLO_REQUIRED";
    case NDP_ST_BAD_FRAME: return "BAD_FRAME";
    case NDP_ST_PATH_INVALID: return "PATH_INVALID";
    default: return "UNKNOWN";
  }
}
