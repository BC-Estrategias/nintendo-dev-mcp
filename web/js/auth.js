/* Pairing and authentication (spec §4): a port of bridge/packages/core/src/auth.ts, verified against the same vectors. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const { sha256, hmacSha256 } = NDP;
  const C = NDP.codec;

  const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

  /** "XXXX-XXXX-XXXX-XXXX" (case-insensitive; hyphens/spaces ignored; O->0, I/L->1) -> 10 bytes, or null. */
  function codeDecode(textIn) {
    let bits = 0n, n = 0;
    for (const raw of String(textIn)) {
      if (raw === "-" || raw === " ") continue;
      let c = raw.toUpperCase();
      if (c === "O") c = "0"; else if (c === "I" || c === "L") c = "1";
      const v = ALPHABET.indexOf(c);
      if (v < 0) return null;
      bits = (bits << 5n) | BigInt(v);
      n++;
    }
    if (n !== 16) return null;
    const out = new Uint8Array(10);
    for (let i = 9; i >= 0; i--) { out[i] = Number(bits & 255n); bits >>= 8n; }
    return out;
  }

  function codeEncode(code) {
    let bits = 0n;
    for (const b of code) bits = (bits << 8n) | BigInt(b);
    let s = "";
    for (let i = 15; i >= 0; i--) s += ALPHABET[Number((bits >> BigInt(i * 5)) & 31n)];
    return `${s.slice(0, 4)}-${s.slice(4, 8)}-${s.slice(8, 12)}-${s.slice(12)}`;
  }

  const derivePsk = (code) => sha256(C.utf8("NDP-PSK-v1"), code);
  const keyIdOf = (psk) => sha256(psk).subarray(0, 4);
  const proofOf = (psk, label, cn, dn, extra = new Uint8Array(0)) => hmacSha256(psk, C.utf8(label), cn, dn, extra);
  const sessionKeyOf = (psk, cn, dn) => proofOf(psk, "session", cn, dn);

  /** mac = HMAC-SHA256(key, u64le(counter) || header[0:20] || payload)[0:16] */
  function frameMac(key, counter, header, payload) {
    const ctr = new Uint8Array(8);
    new DataView(ctr.buffer).setBigUint64(0, BigInt(counter), true);
    return hmacSha256(key, ctr, header.subarray(0, C.HEADER_SIZE), payload).subarray(0, C.MAC_SIZE);
  }

  NDP.auth = { codeDecode, codeEncode, derivePsk, keyIdOf, proofOf, sessionKeyOf, frameMac };
})();
