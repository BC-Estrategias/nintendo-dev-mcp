// NDP v1 constants — mirror of docs/protocol/ndp-v1.md and agent/common/include/ndp/ndp_defs.h.

export const MAGIC = new Uint8Array([0x4e, 0x44, 0x50, 0x31]); // "NDP1"
export const PROTOCOL_VERSION = 1;
export const HEADER_SIZE = 20;
export const MAC_SIZE = 16;
export const DEFAULT_PORT = 6464;
export const DEFAULT_MAX_FRAME = 65536;

export const Kind = { REQ: 1, RES: 2, DATA: 3, END: 4, ERR: 5, EVT: 6 } as const;

export const Flag = { MAC: 0x0001, MORE: 0x0002 } as const;
export const FLAGS_KNOWN = Flag.MAC | Flag.MORE;

export const Command = { HELLO: 0x0001, PING: 0x0002 } as const;

export const Status = {
  OK: 0,
  UNSUPPORTED_PROTOCOL: 1,
  UNAUTHORIZED: 2,
  FORBIDDEN_MODE: 3,
  PROTECTED_PATH: 4,
  NOT_FOUND: 5,
  EXISTS: 6,
  IO_ERROR: 7,
  NO_SPACE: 8,
  BAD_REQUEST: 9,
  BUSY: 10,
  TOO_LARGE: 11,
  HASH_MISMATCH: 12,
  TIMEOUT: 13,
  UNSUPPORTED_COMMAND: 14,
  HELLO_REQUIRED: 15,
  BAD_FRAME: 16,
  PATH_INVALID: 17,
} as const;

export type StatusName = keyof typeof Status;

export function statusName(code: number): StatusName | `UNKNOWN_${number}` {
  for (const [name, value] of Object.entries(Status)) if (value === code) return name as StatusName;
  return `UNKNOWN_${code}`;
}

export const Tag = {
  DETAIL: 0x0001,
  OS_RESULT: 0x0002,
  PROTOCOL: 0x0010, // HELLO REQ: protocol_min; HELLO RES: chosen version
  PROTOCOL_MAX: 0x0011,
  BRIDGE_NAME: 0x0012,
  NONCE: 0x0013,
  PLATFORM: 0x0014,
  AGENT_VERSION: 0x0015,
  AUTH: 0x0016,
  MODE: 0x0017,
  MAX_FRAME: 0x0018,
  SUPPORTED_MIN: 0x0019,
  SUPPORTED_MAX: 0x001a,
  PING_NONCE: 0x0020,
} as const;

export type Mode = "READ_ONLY" | "DEVELOPMENT" | "FULL";
export const MODES: readonly Mode[] = ["READ_ONLY", "DEVELOPMENT", "FULL"];
