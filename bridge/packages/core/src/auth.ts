// Pairing and authentication primitives (spec §4). The Bridge derives everything from the code the
// user types; the code itself never crosses the network.
import { createHash, createHmac, timingSafeEqual } from "node:crypto";
import { chmodSync, mkdirSync, readFileSync, renameSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join } from "node:path";

export const CODE_BYTES = 10;
export const LABEL_MAX = 15;

const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/** Base32 Crockford, 80 bits → "XXXX-XXXX-XXXX-XXXX". */
export function codeEncode(code: Uint8Array): string {
  if (code.length !== CODE_BYTES) throw new RangeError("code must be 10 bytes");
  let bits = 0n;
  for (const b of code) bits = (bits << 8n) | BigInt(b);
  let s = "";
  for (let i = 15; i >= 0; i--) s += ALPHABET[Number((bits >> BigInt(i * 5)) & 31n)];
  return `${s.slice(0, 4)}-${s.slice(4, 8)}-${s.slice(8, 12)}-${s.slice(12)}`;
}

/** Lenient decode: case-insensitive, hyphens/spaces ignored, O→0, I/L→1. Returns null when invalid. */
export function codeDecode(text: string): Uint8Array | null {
  let bits = 0n;
  let n = 0;
  for (const raw of text) {
    if (raw === "-" || raw === " ") continue;
    let c = raw.toUpperCase();
    if (c === "O") c = "0";
    else if (c === "I" || c === "L") c = "1";
    const v = ALPHABET.indexOf(c);
    if (v < 0) return null;
    bits = (bits << 5n) | BigInt(v);
    n++;
  }
  if (n !== 16) return null;
  const out = new Uint8Array(CODE_BYTES);
  for (let i = CODE_BYTES - 1; i >= 0; i--) {
    out[i] = Number(bits & 255n);
    bits >>= 8n;
  }
  return out;
}

export function derivePsk(code: Uint8Array): Uint8Array {
  return new Uint8Array(createHash("sha256").update("NDP-PSK-v1").update(code).digest());
}

export function keyIdOf(psk: Uint8Array): Uint8Array {
  return new Uint8Array(createHash("sha256").update(psk).digest().subarray(0, 4));
}

export type ProofLabel = "pair" | "auth" | "session";

/** HMAC(psk, label ‖ cn ‖ dn ‖ extra). */
export function proofOf(psk: Uint8Array, label: ProofLabel, cn: Uint8Array, dn: Uint8Array, extra: Uint8Array = new Uint8Array(0)): Uint8Array {
  return new Uint8Array(createHmac("sha256", psk).update(label).update(cn).update(dn).update(extra).digest());
}

export function sessionKeyOf(psk: Uint8Array, cn: Uint8Array, dn: Uint8Array): Uint8Array {
  return proofOf(psk, "session", cn, dn);
}

export function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  return a.length === b.length && timingSafeEqual(a, b);
}

// ---------------------------------------------------------------------------------------------
// Key store: what this computer knows about the consoles it is paired with.

export interface PairedConsole {
  /** 16-byte console identity, hex (stable across IP changes). */
  deviceId: string;
  /** PSK, hex. Secret. */
  psk: string;
  keyId: string;
  label: string;
  /** Last address the console was seen at (informational). */
  lastHost?: string;
  pairedAt: string;
}

interface StoreFile {
  version: 1;
  consoles: PairedConsole[];
}

/** Default location; override with NDEV_KEYS_FILE. */
export function defaultKeysPath(): string {
  return process.env.NDEV_KEYS_FILE ?? join(process.env.XDG_CONFIG_HOME ?? join(homedir(), ".config"), "nintendo-dev", "keys.json");
}

/** JSON file with owner-only permissions. Contains secrets: never logged, never sent anywhere. */
export class KeyStore {
  readonly path: string;

  constructor(path: string = defaultKeysPath()) {
    this.path = path;
  }

  #read(): StoreFile {
    let text: string;
    try {
      text = readFileSync(this.path, "utf8");
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code === "ENOENT") return { version: 1, consoles: [] };
      throw e;
    }
    const data = JSON.parse(text) as StoreFile;
    if (data.version !== 1 || !Array.isArray(data.consoles)) throw new Error(`unrecognized key store format: ${this.path}`);
    return data;
  }

  #write(data: StoreFile): void {
    mkdirSync(dirname(this.path), { recursive: true, mode: 0o700 });
    const tmp = `${this.path}.tmp`;
    writeFileSync(tmp, JSON.stringify(data, null, 2) + "\n", { mode: 0o600 });
    chmodSync(tmp, 0o600);
    renameSync(tmp, this.path);
  }

  list(): PairedConsole[] {
    return this.#read().consoles;
  }

  find(deviceId: Uint8Array | string): PairedConsole | undefined {
    const id = typeof deviceId === "string" ? deviceId : Buffer.from(deviceId).toString("hex");
    return this.#read().consoles.find((c) => c.deviceId === id);
  }

  save(entry: PairedConsole): void {
    const data = this.#read();
    data.consoles = data.consoles.filter((c) => c.deviceId !== entry.deviceId);
    data.consoles.push(entry);
    this.#write(data);
  }

  remove(deviceId: string): boolean {
    const data = this.#read();
    const before = data.consoles.length;
    data.consoles = data.consoles.filter((c) => c.deviceId !== deviceId);
    if (data.consoles.length === before) return false;
    this.#write(data);
    return true;
  }
}
