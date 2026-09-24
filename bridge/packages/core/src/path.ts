// Path normalization and prefix comparison — mirror of docs/protocol/ndp-v1.md §9
// and agent/common/src/ndp_path.c. Operates on bytes so that every rule is byte-exact.

export const PATH_MAX = 1024;
export const COMPONENT_MAX = 255;

export class PathInvalidError extends Error {
  constructor(reason: string) {
    super(`invalid path: ${reason}`);
    this.name = "PathInvalidError";
  }
}

const strictUtf8 = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
const encoder = new TextEncoder();

const BAD_CHARS = new Set([...":*?\"<>|"].map((c) => c.charCodeAt(0)));
const SLASH = 0x2f;

function badComponent(c: Uint8Array): string | null {
  if (c.length === 0) return "empty component";
  if (c.length > COMPONENT_MAX) return "component too long";
  if ((c.length === 1 && c[0] === 0x2e) || (c.length === 2 && c[0] === 0x2e && c[1] === 0x2e)) return "'.' or '..'";
  const last = c[c.length - 1];
  if (last === 0x2e || last === 0x20) return "trailing dot or space";
  for (let i = 0; i < c.length; i++) {
    const b = c[i] as number;
    if (BAD_CHARS.has(b)) return "character illegal in FAT";
    if (b === 0x7e && i + 1 < c.length && (c[i + 1] as number) >= 0x30 && (c[i + 1] as number) <= 0x39)
      return "8.3 short-name alias";
  }
  return null;
}

/** Validates and normalizes a path; throws PathInvalidError. Strings are encoded as UTF-8. */
export function normalizePath(input: Uint8Array | string): string {
  const b = typeof input === "string" ? encoder.encode(input) : input;
  if (b.length === 0 || b[0] !== SLASH) throw new PathInvalidError("not absolute");
  if (b.length > PATH_MAX) throw new PathInvalidError("too long");
  for (const c of b) if (c < 0x20 || c === 0x7f || c === 0x5c) throw new PathInvalidError("control character or backslash");
  let text: string;
  try {
    text = strictUtf8.decode(b);
  } catch {
    throw new PathInvalidError("invalid UTF-8");
  }
  if (b.length === 1) return "/";
  let end = b.length;
  if (b[end - 1] === SLASH) end--; // remove one trailing slash
  let start = 1;
  for (let i = 1; i <= end; i++) {
    if (i === end || b[i] === SLASH) {
      const reason = badComponent(b.subarray(start, i));
      if (reason) throw new PathInvalidError(reason);
      start = i + 1;
    }
  }
  return end === b.length ? text : strictUtf8.decode(b.subarray(0, end));
}

function fold(c: number): number {
  return c >= 0x41 && c <= 0x5a ? c + 32 : c;
}

function components(path: string): Uint8Array[] {
  if (path === "/") return [];
  return path
    .slice(1)
    .split("/")
    .map((c) => Uint8Array.from(encoder.encode(c), fold));
}

/** Both arguments must be normalized. True when `path` equals or lies inside `root` (ASCII-only case folding). */
export function pathInside(path: string, root: string): boolean {
  const r = components(root);
  const p = components(path);
  if (p.length < r.length) return false;
  for (let i = 0; i < r.length; i++) {
    const a = p[i] as Uint8Array;
    const b = r[i] as Uint8Array;
    if (a.length !== b.length) return false;
    for (let k = 0; k < a.length; k++) if (a[k] !== b[k]) return false;
  }
  return true;
}
