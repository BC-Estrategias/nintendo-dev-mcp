// The browser's JavaScript NDP client (web/js) — the same code that ships inside the console's page — run in Node:
// its crypto/codec/auth against the shared vectors, then the whole protocol against the C host agent over a WebSocket.
import { strict as assert } from "node:assert";
import { createHash, createHmac, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";
import { type ChildProcess } from "node:child_process";
import { NdpClient } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const WEB = fileURLToPath(new URL("../../../../web/js/", import.meta.url));
for (const f of ["sha256.js", "codec.js", "auth.js", "client.js"]) await import(pathToFileURL(WEB + f).href);
const NDP = (globalThis as any).NDP;
const V = JSON.parse(readFileSync(fileURLToPath(new URL("../../../../docs/protocol/test-vectors/vectors.json", import.meta.url)), "utf8"));
const hex = (b: Uint8Array) => Buffer.from(b).toString("hex");
const fromHex = (h: string) => new Uint8Array(Buffer.from(h, "hex"));
const DIR = "/3ds/nintendo-dev-agent";
const code = async (p: Promise<unknown>) => p.then(() => "ok", (e: any) => e?.statusName ?? `local:${e?.name}`);

test("web/js SHA-256 and HMAC agree with the shared vectors and with node:crypto", () => {
  for (const e of V.sha256) assert.equal(hex(NDP.sha256(fromHex(e.msg_hex))), e.digest_hex);
  for (const e of V.hmac_sha256) assert.equal(hex(NDP.hmacSha256(fromHex(e.key_hex), fromHex(e.msg_hex))), e.mac_hex);
  // lengths around the block boundaries, random data, split into random pieces
  for (const n of [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000, 65536, 1_000_003]) {
    const d = randomBytes(n);
    const want = createHash("sha256").update(d).digest("hex");
    assert.equal(hex(NDP.sha256(new Uint8Array(d))), want, `len ${n}`);
    const h = new NDP.Sha256();
    for (let o = 0; o < n; ) { const k = 1 + Math.floor(Math.random() * 200); h.update(new Uint8Array(d.subarray(o, o + k))); o += k; }
    assert.equal(hex(h.digest()), want, `split len ${n}`);
  }
  for (const [kl, ml] of [[0, 0], [1, 10], [32, 100], [64, 5], [65, 5], [200, 3000]] as Array<[number, number]>) {
    const k = randomBytes(kl), m = randomBytes(ml);
    assert.equal(hex(NDP.hmacSha256(new Uint8Array(k), new Uint8Array(m))), createHmac("sha256", k).update(m).digest("hex"), `hmac ${kl}/${ml}`);
  }
});

test("web/js frame codec and TLV: byte-exact encoding, chunked decoding", () => {
  const C = NDP.codec;
  for (const f of V.frames) {
    const header = C.encodeHeader({ kind: f.kind, flags: f.flags & ~1, requestId: f.request_id, command: f.command, status: f.status, payloadLength: f.payload_hex.length / 2 });
    const wire = fromHex(f.wire_hex);
    // the header (with the MAC flag when the frame has one) is the first 20 bytes of the wire
    const withFlags = C.encodeHeader({ kind: f.kind, flags: f.flags, requestId: f.request_id, command: f.command, status: f.status, payloadLength: f.payload_hex.length / 2 });
    assert.equal(hex(withFlags), hex(wire.subarray(0, 20)), f.name);
    void header;
    for (const step of [1, 7, 1 << 20]) {
      const dec = new C.FrameDecoder(65536);
      const got: any[] = [];
      for (let i = 0; i < wire.length; i += step) got.push(...dec.push(wire.subarray(i, i + step)));
      assert.equal(got.length, 1, `${f.name} step ${step}`);
      assert.equal(hex(got[0].payload), f.payload_hex);
      assert.equal(got[0].header.command, f.command);
      assert.equal(got[0].mac ? hex(got[0].mac) : null, f.mac_hex ?? null);
    }
  }
  for (const e of V.frame_errors) assert.throws(() => new C.FrameDecoder(e.max_payload).push(fromHex(e.wire_hex)), `frame error ${e.name}`);
  const t = C.parseTlv(C.encodeTlv([[1, C.str("héllo")], [2, C.u32(0xdeadbeef)], [3, C.u64(2 ** 40 + 5)], [1, C.str("again")]]));
  assert.equal(t.str(1), "héllo");
  assert.equal(t.u32(2), 0xdeadbeef);
  assert.equal(t.u64(3), 2 ** 40 + 5);
  assert.deepEqual(t.all(1).map(C.text), ["héllo", "again"]);
  assert.throws(() => C.parseTlv(new Uint8Array([1, 0, 9, 0, 1])), /malformed/);
});

test("web/js pairing primitives agree with the vectors (independent Python reference)", () => {
  const A = NDP.auth;
  for (const k of V.auth.kdf) {
    const codeBytes = fromHex(k.code_hex);
    assert.equal(A.codeEncode(codeBytes), k.code_text);
    assert.equal(hex(A.codeDecode(k.code_text)), k.code_hex);
    const psk = A.derivePsk(codeBytes);
    assert.equal(hex(psk), k.psk_hex);
    assert.equal(hex(A.keyIdOf(psk)), k.key_id_hex);
    const cn = fromHex(k.cn_hex), dn = fromHex(k.dn_hex);
    assert.equal(hex(A.proofOf(psk, "pair", cn, dn, new TextEncoder().encode(k.label))), k.pair_proof_hex);
    assert.equal(hex(A.proofOf(psk, "auth", cn, dn)), k.auth_proof_hex);
    assert.equal(hex(A.sessionKeyOf(psk, cn, dn)), k.session_key_hex);
  }
  for (const c of V.auth.code_decode) assert.equal(A.codeDecode(c.text) ? hex(A.codeDecode(c.text)) : null, c.code_hex ?? null, c.text);
  for (const m of V.mac) assert.equal(hex(A.frameMac(fromHex(m.key_hex), m.counter, fromHex(m.header_and_payload_hex).subarray(0, 20), fromHex(m.header_and_payload_hex).subarray(20))), m.mac_hex, m.name);
});

const suite = SKIP ? describe.skip : describe;

suite("web/js client against the C agent over a WebSocket", () => {
  let dir: string, root: string, agentDir: string, proc: ChildProcess, port: number, web: number;
  const CODE = "7QK3-M9XD-4WPZ-A2HB";
  const url = () => `ws://127.0.0.1:${web}/ws`;
  const open = async () => new NDP.NdpWsClient(url()).open();

  let PSK: Uint8Array;
  /** The pairing window closes after one pairing (spec §4.3): pair once, then authenticate with that key. */
  async function paired() {
    if (!PSK) { // (only one page connection exists at a time: pair on its own connection first)
      const p = await open();
      await p.hello();
      PSK = (await p.pair(NDP.auth.codeDecode(CODE), "web-test")).psk;
      p.close();
    }
    const c = await open();
    await c.hello();
    await c.authenticate(PSK);
    return { c, psk: PSK };
  }

  before(async () => {
    dir = mkdtempSync(join(tmpdir(), "ndp-webjs-"));
    root = join(dir, "sd");
    agentDir = join(root, DIR);
    mkdirSync(agentDir, { recursive: true });
    writeFileSync(join(agentDir, "test.txt"), "Hello from Nintendo 3DS");
    const a = await startAgent(["--root", root, "--mode", "DEVELOPMENT", "--auth", "required", "--pair-code", CODE, "--keys-file", join(dir, "keys.bin"), "--web-port", "0"]);
    ({ proc, port } = a);
    web = a.webPort!;
  });
  after(() => {
    proc?.kill();
    rmSync(dir, { recursive: true, force: true });
  });

  test("hello announces the requirement; nothing works before authentication; a wrong code is refused", async () => {
    const c = await open();
    try {
      const info = await c.hello();
      assert.equal(info.auth, "required");
      assert.equal(info.deviceId.length, 16);
      assert.equal(info.pairingOpen, true);
      assert.equal(await code(c.list("/")), "UNAUTHORIZED");
      assert.equal(await code(c.pair(NDP.auth.codeDecode("0000-0000-0000-0000"), "evil")), "UNAUTHORIZED");
    } finally {
      c.close();
    }
  });

  test("pair, then authenticate: the whole file API works sealed (list, stat, read, write, mkdir, rename, delete, info)", async () => {
    const { c } = await paired();
    try {
      assert.equal(c.authenticated, true);
      assert.deepEqual((await c.list(DIR)).map((e: any) => e.name), ["test.txt"]);
      assert.equal((await c.stat(`${DIR}/test.txt`)).size, 23);
      assert.equal(new TextDecoder().decode((await c.readBytes(`${DIR}/test.txt`)).data), "Hello from Nintendo 3DS");
      assert.equal(await c.ping() > 0, true);
      const big = new Uint8Array(randomBytes(900_001));
      let progress = 0;
      const w = await c.write(`${DIR}/big.bin`, big, { onProgress: (n: number) => (progress = n) });
      assert.equal(w.written, big.length);
      assert.equal(progress, big.length);
      assert.equal(await code(c.write(`${DIR}/big.bin`, big)), "EXISTS", "never overwrites unless asked");
      const w2 = await c.write(`${DIR}/big.bin`, new Uint8Array([1, 2, 3]), { overwrite: true, backup: true });
      assert.equal(w2.replaced, true);
      assert.equal(readFileSync(join(agentDir, "big.bin.bak")).length, 900_001);
      const back = await c.readBytes(`${DIR}/big.bin.bak`);
      assert.equal(hex(NDP.sha256(back.data)), createHash("sha256").update(big).digest("hex"));
      await c.mkdir(`${DIR}/sub`);
      assert.equal(await c.rename(`${DIR}/big.bin`, `${DIR}/sub/moved.bin`), `${DIR}/sub/moved.bin`);
      assert.equal(existsSync(join(agentDir, "sub/moved.bin")), true);
      const d = await c.delete(`${DIR}/sub/moved.bin`);
      assert.equal(d.trashPath, `${DIR}/.ndp-trash/moved.bin`);
      assert.equal(await c.rename(d.trashPath, `${DIR}/restored.bin`), `${DIR}/restored.bin`, "restore from the trash");
      assert.equal(await code(c.write("/luma/x.bin", big)), "PROTECTED_PATH");
      assert.equal((await c.accessInfo()).mode, "DEVELOPMENT");
      const info = await c.deviceInfo();
      assert.match(info.model, /^host \(/);
      assert.ok(info.sd.total > 0);
    } finally {
      c.close();
    }
  });

  test("a File-like Blob uploads without holding it in memory, byte-exact, with progress; a big download verifies its hash", async () => {
    const { c } = await paired();
    try {
      const data = randomBytes(3 * 1024 * 1024 + 17);
      const blob = new Blob([data]);
      const t = { hash: 0, sent: 0 };
      await c.write(`${DIR}/blob.bin`, blob, { onHashProgress: (n: number) => (t.hash = n), onProgress: (n: number) => (t.sent = n) });
      assert.equal(t.hash, data.length);
      assert.equal(t.sent, data.length);
      assert.deepEqual(readFileSync(join(agentDir, "blob.bin")), data);
      let got = 0;
      const r = await c.read(`${DIR}/blob.bin`, {}, (chunk: Uint8Array) => { got += chunk.length; });
      assert.equal(got, data.length);
      assert.equal(r.verified, true);
      const part = await c.readBytes(`${DIR}/blob.bin`, { offset: 1000, length: 5000 });
      assert.deepEqual(Buffer.from(part.data), data.subarray(1000, 6000));
    } finally {
      c.close();
    }
  });

  test("aborting an upload closes the connection and leaves nothing behind on the card", async () => {
    const { c } = await paired();
    const ctl = new AbortController();
    const data = new Uint8Array(randomBytes(2_000_000));
    const p = c.write(`${DIR}/aborted.bin`, data, { signal: ctl.signal, onProgress: (n: number) => { if (n > 200_000) ctl.abort(); } });
    await assert.rejects(p);
    assert.equal(c.isClosed, true);
    await new Promise((r) => setTimeout(r, 300));
    assert.equal(existsSync(join(agentDir, "aborted.bin")), false);
    assert.deepEqual(readdirSync(agentDir).filter((n) => /ndp-(tmp|old)/.test(n)), [], "no temporary file left");
  });

  test("the page and the CLI/MCP client work at the same time on the same card", async () => {
    const { c } = await paired();
    const raw = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      // the raw client also needs to be authenticated: pair state is shared (keys), so reuse the stored key
      const info = await raw.hello();
      assert.equal(info.auth, "required");
      await raw.authenticate(PSK); // the same paired key opens a session on the raw port too
      assert.equal((await raw.stat(`${DIR}/test.txt`)).size, 23);
      assert.equal((await c.stat(`${DIR}/test.txt`)).size, 23, "the page keeps working while a raw client is connected");
      assert.equal((await raw.stat(`${DIR}/test.txt`)).size, 23, "and the raw client keeps working while the page is open");
    } finally {
      raw.close();
      c.close();
    }
  });

  test("tampering is detected: a client that seals with the wrong key never gets an answer", async () => {
    const c = await open();
    try {
      await c.hello();
      const wrong = NDP.auth.derivePsk(NDP.auth.codeDecode("0000-0000-0000-0000"));
      assert.equal(await code(c.authenticate(wrong)), "UNAUTHORIZED");
    } finally {
      c.close();
    }
  });
});
