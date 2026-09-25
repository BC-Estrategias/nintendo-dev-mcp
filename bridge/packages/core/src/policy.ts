// Access policy — mirror of docs/protocol/ndp-v1.md §10 and agent/common/src/ndp_policy.c.
import type { Mode } from "./constants.ts";
import { Status, type StatusName } from "./constants.ts";
import { normalizePath, pathInside, PathInvalidError } from "./path.ts";

export interface PolicyConfig {
  readRoots: readonly string[];
  writeRoots: readonly string[];
  neverRead: readonly string[];
  neverWrite: readonly string[];
  /** Folders inside a neverWrite zone that ARE writable (spec §11); optional, default none. */
  writeExcept?: readonly string[];
}

export type Operation = "read" | "write";

export interface PolicyDecision {
  status: StatusName;
  code: number;
  /** Normalized path (only when status is OK). */
  normalized?: string;
}

export const DEFAULT_POLICY: PolicyConfig = Object.freeze({
  readRoots: Object.freeze(["/"]),
  writeRoots: Object.freeze(["/3ds/nintendo-dev-agent"]),
  neverRead: Object.freeze(["/3ds/nintendo-dev-agent/config"]),
  neverWrite: Object.freeze([
    "/Nintendo 3DS",
    "/luma",
    "/boot.firm",
    "/gm9",
    "/private",
    "/3ds/nintendo-dev-agent/config",
  ]),
  writeExcept: Object.freeze(["/luma/plugins", "/luma/titles"]),
});

/** Normalizes every list entry; throws PathInvalidError for a bad entry. */
export function makePolicy(cfg: PolicyConfig): PolicyConfig {
  const norm = (l: readonly string[]) => Object.freeze(l.map((p) => normalizePath(p)));
  return Object.freeze({
    readRoots: norm(cfg.readRoots),
    writeRoots: norm(cfg.writeRoots),
    neverRead: norm(cfg.neverRead),
    neverWrite: norm(cfg.neverWrite),
    writeExcept: norm(cfg.writeExcept ?? []),
  });
}

function decide(status: StatusName, normalized?: string): PolicyDecision {
  return normalized === undefined ? { status, code: Status[status] } : { status, code: Status[status], normalized };
}

/** Spec §11: inside a never_write zone Z, unless an exception E has the path inside it and is a PROPER sub-folder of Z. */
export function isWriteProtected(cfg: PolicyConfig, norm: string): boolean {
  const exc = cfg.writeExcept ?? [];
  return cfg.neverWrite.some(
    (z) => pathInside(norm, z) && !exc.some((e) => pathInside(norm, e) && pathInside(e, z) && !pathInside(z, e)),
  );
}

/** `cfg` must come from makePolicy (or be pre-normalized). Order of checks follows the spec. */
export function checkPolicy(cfg: PolicyConfig, mode: Mode, op: Operation, path: Uint8Array | string): PolicyDecision {
  let normalized: string;
  try {
    normalized = normalizePath(path);
  } catch (e) {
    if (e instanceof PathInvalidError) return decide("PATH_INVALID");
    throw e;
  }
  const write = op === "write";
  if (write && mode === "READ_ONLY") return decide("FORBIDDEN_MODE");
  if (write ? isWriteProtected(cfg, normalized) : cfg.neverRead.some((n) => pathInside(normalized, n)))
    return decide("PROTECTED_PATH");
  const roots = write ? cfg.writeRoots : cfg.readRoots;
  if (!roots.some((r) => pathInside(normalized, r))) return decide("PROTECTED_PATH");
  return decide("OK", normalized);
}

/** Spec §11.1: a directory that is a PROPER ANCESTOR of a read root (and not in a never_read zone) may be stat'ed
 * and listed (the listing shows only what leads to a readable folder). `norm` must be a normalized path. */
export function isTraversable(cfg: PolicyConfig, norm: string): boolean {
  if (cfg.neverRead.some((n) => pathInside(norm, n))) return false;
  return cfg.readRoots.some((r) => pathInside(r, norm) && !pathInside(norm, r));
}

/** Whether the listing of a traversable directory shows the entry `norm`. */
export function isChildVisible(cfg: PolicyConfig, norm: string): boolean {
  if (cfg.neverRead.some((n) => pathInside(norm, n))) return false;
  return cfg.readRoots.some((r) => pathInside(norm, r)) || isTraversable(cfg, norm);
}
