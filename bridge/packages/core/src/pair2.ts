// Pairing by number comparison (spec §4.8): X25519 (node:crypto, an implementation independent of the console's C code and the
// page's JavaScript), a commitment, and the derivations both sides compute. The number is shown on both screens and the person
// confirms on the console.
import { createHash, createHmac, createPrivateKey, createPublicKey, diffieHellman, randomBytes } from "node:crypto";

const PKCS8_X25519 = Buffer.from("302e020100300506032b656e04220420", "hex");
const SPKI_X25519 = Buffer.from("302a300506032b656e032100", "hex");

export const PAIR2_NONCE = 16;

/** scalar * 9 (the public key of an X25519 secret). */
export function x25519Public(secret: Uint8Array): Uint8Array {
  const priv = createPrivateKey({ key: Buffer.concat([PKCS8_X25519, secret]), format: "der", type: "pkcs8" });
  const der = createPublicKey(priv).export({ format: "der", type: "spki" });
  return new Uint8Array(der.subarray(der.length - 32));
}

/** The shared secret, or null when the peer's key is a low-order point (all-zero result): the exchange must be abandoned. */
export function x25519Shared(secret: Uint8Array, peerPublic: Uint8Array): Uint8Array | null {
  try {
    const priv = createPrivateKey({ key: Buffer.concat([PKCS8_X25519, secret]), format: "der", type: "pkcs8" });
    const pub = createPublicKey({ key: Buffer.concat([SPKI_X25519, peerPublic]), format: "der", type: "spki" });
    const out = diffieHellman({ privateKey: priv, publicKey: pub });
    return out.every((b) => b === 0) ? null : new Uint8Array(out);
  } catch {
    return null;
  }
}

export function pair2Commit(pubB: Uint8Array, nonceB: Uint8Array): Uint8Array {
  return new Uint8Array(createHash("sha256").update("NDP-PAIR2-COMMIT").update(pubB).update(nonceB).digest());
}

export interface Pair2Keys {
  psk: Uint8Array;
  keyId: Uint8Array;
  /** 0..999999 */
  sas: number;
}

export function pair2Derive(shared: Uint8Array, pubB: Uint8Array, pubC: Uint8Array, nonceB: Uint8Array, nonceC: Uint8Array, label: Uint8Array): Pair2Keys {
  const th = createHash("sha256").update("NDP-PAIR2-TH").update(pubB).update(pubC).update(nonceB).update(nonceC).update(Uint8Array.of(label.length)).update(label).digest();
  const k = createHmac("sha256", shared).update(th).digest();
  const sas = createHmac("sha256", k).update("sas").digest().readUInt32BE(0) % 1_000_000;
  const psk = new Uint8Array(createHmac("sha256", k).update("psk").digest());
  return { psk, keyId: new Uint8Array(createHash("sha256").update(psk).digest().subarray(0, 4)), sas };
}

export function pair2OkProof(psk: Uint8Array, pubB: Uint8Array, pubC: Uint8Array): Uint8Array {
  return new Uint8Array(createHmac("sha256", psk).update("NDP-PAIR2-OK").update(pubB).update(pubC).digest());
}

/** "482913" → "482 913" */
export const formatSas = (sas: number): string => {
  const s = String(sas).padStart(6, "0");
  return `${s.slice(0, 3)} ${s.slice(3)}`;
};

export const randomSecret = (): Uint8Array => new Uint8Array(randomBytes(32));
export const randomNonce = (): Uint8Array => new Uint8Array(randomBytes(PAIR2_NONCE));

/** The console's person said no (or the request expired), or its key store is full. */
export class PairingRefusedError extends Error {
  reason: "denied" | "full";
  constructor(reason: "denied" | "full", message: string) {
    super(message);
    this.name = "PairingRefusedError";
    this.reason = reason;
  }
}
