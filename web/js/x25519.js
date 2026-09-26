/* X25519 (RFC 7748) with BigInt: the Montgomery ladder, for pairing by number comparison. http:// pages have no crypto.subtle,
 * so this ships with the page. The secret is ephemeral (one pairing) so timing side channels are not a concern here; it is
 * verified against the RFC vectors, the Python reference, node:crypto and the console's C code. */
(function () {
  "use strict";
  const NDP = (globalThis.NDP = globalThis.NDP || {});
  const P = (1n << 255n) - 19n;
  const A24 = 121665n;

  const fromLE = (b) => { let n = 0n; for (let i = 31; i >= 0; i--) n = (n << 8n) | BigInt(b[i]); return n; };
  const toLE = (n) => { const o = new Uint8Array(32); for (let i = 0; i < 32; i++) { o[i] = Number(n & 255n); n >>= 8n; } return o; };
  const mod = (a) => { const r = a % P; return r < 0n ? r + P : r; };
  function inv(a) { // a^(P-2)
    let result = 1n, base = mod(a), e = P - 2n;
    while (e > 0n) { if (e & 1n) result = (result * base) % P; base = (base * base) % P; e >>= 1n; }
    return result;
  }

  /** scalar * u. Returns 32 bytes (all zero when u has a small order). */
  function scalarmult(scalar, u) {
    const k = Uint8Array.from(scalar);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    const kn = fromLE(k);
    const x1 = fromLE(u) & ((1n << 255n) - 1n);
    let x2 = 1n, z2 = 0n, x3 = x1, z3 = 1n, swap = 0n;
    for (let t = 254n; t >= 0n; t--) {
      const kt = (kn >> t) & 1n;
      swap ^= kt;
      if (swap) { [x2, x3] = [x3, x2]; [z2, z3] = [z3, z2]; }
      swap = kt;
      const a = mod(x2 + z2), aa = (a * a) % P, b = mod(x2 - z2), bb = (b * b) % P, e = mod(aa - bb);
      const c = mod(x3 + z3), d = mod(x3 - z3), da = (d * a) % P, cb = (c * b) % P;
      const s = mod(da + cb), r = mod(da - cb);
      x3 = (s * s) % P;
      z3 = (x1 * ((r * r) % P)) % P;
      x2 = (aa * bb) % P;
      z2 = (e * mod(aa + A24 * e)) % P;
    }
    if (swap) { [x2, x3] = [x3, x2]; [z2, z3] = [z3, z2]; }
    return toLE((x2 * inv(z2)) % P);
  }

  const BASE = (() => { const b = new Uint8Array(32); b[0] = 9; return b; })();
  const publicKey = (secret) => scalarmult(secret, BASE);
  /** The shared secret, or null for a low-order peer key (the exchange must be abandoned). */
  function shared(secret, peerPublic) {
    const out = scalarmult(secret, peerPublic);
    return out.every((x) => x === 0) ? null : out;
  }

  NDP.x25519 = { publicKey, shared, scalarmult };
})();
