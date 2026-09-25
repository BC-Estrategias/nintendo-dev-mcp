// Pairing / authentication: primitives against the generator's vectors, then the whole flow between the
// TypeScript client and the C agent (host build, --auth required) over real TCP.
import { strict as assert } from "node:assert";
import { createHash, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { connect as tcpConnect, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { execFile, spawn, type ChildProcess } from "node:child_process";
import {
  Command, Flag, FrameDecoder, Kind, KeyStore, NdpClient, NdpRemoteError, NdpTransportError, Status, Tag,
  codeDecode, codeEncode, derivePsk, encodeFrame, encodeHeader, encodeTlv, frameMac, keyIdOf, parseTlv, proofOf,
  sessionKeyOf, str, u16, type Frame,
} from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const V = JSON.parse(readFileSync(fileURLToPath(new URL("../../../../docs/protocol/test-vectors/vectors.json", import.meta.url)), "utf8"));
const hex = (h: string) => new Uint8Array(Buffer.from(h, "hex"));
const toHex = (b: Uint8Array) => Buffer.from(b).toString("hex");

test("kdf vectors (independent Python reference)", () => {
  for (const k of V.auth.kdf) {
    const code = hex(k.code_hex);
    assert.equal(codeEncode(code), k.code_text);
    assert.deepEqual(codeDecode(k.code_text), code);
    const psk = derivePsk(code);
    assert.equal(toHex(psk), k.psk_hex);
    assert.equal(toHex(keyIdOf(psk)), k.key_id_hex);
    const cn = hex(k.cn_hex), dn = hex(k.dn_hex);
    assert.equal(toHex(proofOf(psk, "pair", cn, dn, new TextEncoder().encode(k.label))), k.pair_proof_hex);
    assert.equal(toHex(proofOf(psk, "auth", cn, dn)), k.auth_proof_hex);
    assert.equal(toHex(sessionKeyOf(psk, cn, dn)), k.session_key_hex);
  }
});

test("code decoding is lenient with case, separators and look-alikes, and strict about the rest", () => {
  for (const c of V.auth.code_decode) {
    const got = codeDecode(c.text);
    assert.deepEqual(got && toHex(got), c.code_hex ?? null, c.text);
  }
  assert.equal(codeDecode("too short"), null);
  assert.equal(codeDecode("0000-0000-0000-000U"), null); // U is not in the alphabet
});

const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;

suite("pairing over TCP", () => {
  let dir: string;
  let root: string;
  let proc: ChildProcess | undefined;
  let port = 0;
  let keysFile = "";
  const CODE = "7QK3-M9XD-4WPZ-A2HB";
  const code = codeDecode(CODE)!;
  const psk = derivePsk(code);
  const opts = () => ({ host: "127.0.0.1", port });
  let n = 0;

  /** (Re)starts the agent; `fresh` gives it an empty key file, otherwise it reloads the previous one. */
  async function start(o: { fresh?: boolean; pairCode?: string | null } = {}) {
    await stop();
    if (o.fresh ?? true) keysFile = join(dir, `console-keys-${++n}.bin`);
    ({ proc, port } = await startAgent([
      "--root", root, "--mode", "DEVELOPMENT", "--auth", "required", "--keys-file", keysFile,
      ...(o.pairCode === null ? [] : ["--pair-code", o.pairCode ?? CODE]),
    ]));
  }
  async function stop() {
    if (!proc) return;
    const p = proc;
    proc = undefined;
    p.kill();
    await new Promise((r) => p.once("exit", r));
  }
  async function pairedClient(): Promise<NdpClient> {
    const c = await NdpClient.connect(opts());
    await c.hello();
    await c.pair(code, "test-mac");
    c.close();
    const d = await NdpClient.connect(opts());
    await d.hello();
    await d.authenticate(psk);
    return d;
  }

  before(() => {
    dir = mkdtempSync(join(tmpdir(), "ndp-auth-"));
    root = join(dir, "sd");
    mkdirSync(join(root, "3ds/nintendo-dev-agent"), { recursive: true });
    writeFileSync(join(root, "hello.txt"), "Hello from Nintendo 3DS\n");
  });
  after(async () => {
    await stop();
    rmSync(dir, { recursive: true, force: true });
  });

  test("HELLO announces the requirement, the identity and the open window", async () => {
    await start();
    const c = await NdpClient.connect(opts());
    try {
      const info = await c.hello();
      assert.equal(info.auth, "required");
      assert.equal(info.deviceId?.length, 16);
      assert.equal(info.pairedKeys, 0);
      assert.equal(info.pairingOpen, true);
    } finally {
      c.close();
    }
  });

  test("nothing but HELLO/PAIR/AUTH works before authentication", async () => {
    await start();
    const c = await NdpClient.connect(opts());
    try {
      await c.hello();
      for (const p of [() => c.stat("/hello.txt"), () => c.list("/"), () => c.mkdir("/3ds/nintendo-dev-agent/x")])
        await assert.rejects(p(), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
      assert.equal((await c.hello()).auth, "required"); // still usable (for pairing)
    } finally {
      c.close();
    }
  });

  test("pair, then authenticate on a new connection: reads and writes work sealed", async () => {
    await start();
    const c = await pairedClient();
    try {
      assert.equal(c.authenticated, true);
      assert.equal((await c.stat("/hello.txt")).size, 24);
      assert.equal(Buffer.from((await c.readBytes("/hello.txt")).data).toString(), "Hello from Nintendo 3DS\n");
      const big = randomBytes(700_000); // many sealed DATA frames each way
      const sha = createHash("sha256").update(big).digest();
      const w = await c.write("/3ds/nintendo-dev-agent/big.bin", { size: big.length, sha256: new Uint8Array(sha), chunks: [big] });
      assert.equal(w.written, big.length);
      const back = await c.readBytes("/3ds/nintendo-dev-agent/big.bin", { maxBytes: 1_000_000 });
      assert.equal(Buffer.compare(Buffer.from(back.data), big), 0);
      await c.mkdir("/3ds/nintendo-dev-agent/d");
      assert.ok((await c.list("/3ds/nintendo-dev-agent")).some((e) => e.name === "d"));
    } finally {
      c.close();
    }
  });

  test("the pairing window closes after a successful pairing (one computer per opening)", async () => {
    await start();
    (await pairedClient()).close();
    const c = await NdpClient.connect(opts());
    try {
      const info = await c.hello();
      assert.equal(info.pairingOpen, false);
      assert.equal(info.pairedKeys, 1);
      await assert.rejects(c.pair(code, "second"), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
    } finally {
      c.close();
    }
  });

  test("a wrong code is refused; five wrong attempts close the connection and the window", async () => {
    await start();
    const bad = codeDecode("0000-0000-0000-0000")!;
    const c = await NdpClient.connect(opts());
    try {
      await c.hello();
      for (let i = 0; i < 4; i++)
        await assert.rejects(c.pair(bad, "evil"), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
      await assert.rejects(c.pair(bad, "evil"), (e) => e instanceof NdpTransportError || e instanceof NdpRemoteError);
      await assert.rejects(c.hello(), NdpTransportError);
    } finally {
      c.close();
    }
    const c2 = await NdpClient.connect(opts());
    try {
      assert.equal((await c2.hello()).pairingOpen, false, "the window closes after too many failures (spec §4.3)");
      await assert.rejects(c2.pair(code, "late"), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
    } finally {
      c2.close();
    }
  });

  test("authentication with a wrong key fails; five failures close the connection", async () => {
    await start();
    (await pairedClient()).close();
    const wrong = derivePsk(codeDecode("0000-0000-0000-0000")!);
    const c = await NdpClient.connect(opts());
    try {
      await c.hello();
      for (let i = 0; i < 4; i++)
        await assert.rejects(c.authenticate(wrong), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
      await assert.rejects(c.authenticate(wrong), (e) => e instanceof NdpTransportError || e instanceof NdpRemoteError);
      await assert.rejects(c.hello(), NdpTransportError);
    } finally {
      c.close();
    }
    // the legitimate computer is not locked out
    const ok = await NdpClient.connect(opts());
    try {
      await ok.hello();
      await ok.authenticate(psk);
      assert.equal((await ok.stat("/hello.txt")).type, "file");
    } finally {
      ok.close();
    }
  });

  test("a second HELLO after AUTH is refused (a new connection is required)", async () => {
    await start();
    const c = await pairedClient();
    try {
      await assert.rejects(c.hello(), (e) => e instanceof NdpRemoteError && e.status === Status.BAD_REQUEST);
      assert.equal((await c.stat("/hello.txt")).type, "file"); // and the session survives the refusal
    } finally {
      c.close();
    }
  });

  test("pairings survive an agent restart (key file), the window does not", async () => {
    await start();
    (await pairedClient()).close();
    await start({ fresh: false, pairCode: null });
    const c = await NdpClient.connect(opts());
    try {
      const info = await c.hello();
      assert.equal(info.pairedKeys, 1);
      assert.equal(info.pairingOpen, false);
      await c.authenticate(psk);
      assert.equal((await c.stat("/hello.txt")).type, "file");
    } finally {
      c.close();
    }
  });

  test("the console keeps its identity across restarts", async () => {
    await start();
    const a = await NdpClient.connect(opts());
    const ida = (await a.hello()).deviceId!;
    a.close();
    await start({ fresh: false, pairCode: null });
    const b = await NdpClient.connect(opts());
    const idb = (await b.hello()).deviceId!;
    b.close();
    assert.equal(toHex(ida), toHex(idb));
  });

  test("a corrupted key file is ignored (fail closed: no pairings), not trusted", async () => {
    await start();
    (await pairedClient()).close();
    await stop();
    const raw = readFileSync(keysFile);
    raw[30] = (raw[30] ?? 0) ^ 0xff;
    writeFileSync(keysFile, raw);
    rmSync(`${keysFile}.bak`, { force: true });
    await start({ fresh: false, pairCode: null });
    const c = await NdpClient.connect(opts());
    try {
      await c.hello();
      await assert.rejects(c.authenticate(psk), (e) => e instanceof NdpRemoteError && e.status === Status.UNAUTHORIZED);
    } finally {
      c.close();
    }
  });

  // ---- raw wire tests: what an attacker on the LAN could try -------------------------------------
  function rawClient(): Promise<{ sock: Socket; frames: () => Frame[]; closed: () => boolean; send: (b: Uint8Array) => void }> {
    return new Promise((resolve, reject) => {
      const sock = tcpConnect({ host: "127.0.0.1", port });
      const dec = new FrameDecoder(65536);
      const got: Frame[] = [];
      let closed = false;
      sock.on("data", (d: Buffer) => got.push(...dec.push(new Uint8Array(d))));
      sock.on("close", () => (closed = true));
      sock.on("error", reject);
      sock.once("connect", () => resolve({ sock, frames: () => got, closed: () => closed, send: (b) => sock.write(b) }));
    });
  }
  const settle = (ms = 150) => new Promise((r) => setTimeout(r, ms));

  async function rawAuthenticated() {
    await start();
    (await pairedClient()).close();
    const r = await rawClient();
    const cn = randomBytes(16);
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 1, command: Command.HELLO, payload: encodeTlv([
      [Tag.PROTOCOL, u16(1)], [Tag.PROTOCOL_MAX, u16(1)], [Tag.BRIDGE_NAME, str("raw")], [Tag.NONCE, cn]]) }));
    await settle();
    const dn = parseTlv(r.frames()[0]!.payload).first(Tag.NONCE)!;
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 2, command: Command.AUTH, payload: encodeTlv([
      [Tag.KEY_ID, keyIdOf(psk)], [Tag.PROOF, proofOf(psk, "auth", new Uint8Array(cn), dn)]]) }));
    await settle();
    const res = r.frames()[1]!;
    assert.equal(res.header.kind, Kind.RES);
    assert.ok(res.mac, "the AUTH response is the first sealed frame");
    const key = sessionKeyOf(psk, new Uint8Array(cn), dn);
    const sealed = (ctr: bigint, id: number, cmd: number, payload: Uint8Array) => {
      const header = encodeHeader({ version: 1, kind: Kind.REQ, flags: Flag.MAC, requestId: id, command: cmd, status: 0, payloadLength: payload.length });
      return encodeFrame({ kind: Kind.REQ, requestId: id, command: cmd, payload, mac: frameMac(key, ctr, header, payload) });
    };
    return { r, key, sealed, statReq: encodeTlv([[Tag.PATH, str("/hello.txt")]]) };
  }

  test("wire: a valid sealed request is answered with a sealed response", async () => {
    const { r, sealed, statReq } = await rawAuthenticated();
    r.send(sealed(0n, 3, Command.FS_STAT, statReq));
    await settle();
    const f = r.frames()[2]!;
    assert.equal(f.header.kind, Kind.RES);
    assert.ok(f.mac);
    assert.equal(r.closed(), false);
    r.sock.destroy();
  });

  test("wire: a request without MAC after AUTH closes the connection, unanswered", async () => {
    const { r, statReq } = await rawAuthenticated();
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 3, command: Command.FS_STAT, payload: statReq }));
    await settle();
    assert.equal(r.frames().length, 2, "no reply");
    assert.equal(r.closed(), true);
  });

  test("wire: a replayed frame (old counter) closes the connection", async () => {
    const { r, sealed, statReq } = await rawAuthenticated();
    const first = sealed(0n, 3, Command.FS_STAT, statReq);
    r.send(first);
    await settle();
    assert.equal(r.frames().length, 3);
    r.send(first); // the exact same bytes again
    await settle();
    assert.equal(r.frames().length, 3, "the replay got no answer");
    assert.equal(r.closed(), true);
  });

  test("wire: a tampered payload closes the connection", async () => {
    const { r, sealed, statReq } = await rawAuthenticated();
    const f = sealed(0n, 3, Command.FS_STAT, statReq);
    f[24] = (f[24] ?? 0) ^ 0x01; // flip a bit inside the payload
    r.send(f);
    await settle();
    assert.equal(r.frames().length, 2);
    assert.equal(r.closed(), true);
  });

  test("wire: a frame sealed with another key closes the connection", async () => {
    const { r, statReq } = await rawAuthenticated();
    const key = new Uint8Array(randomBytes(32));
    const header = encodeHeader({ version: 1, kind: Kind.REQ, flags: Flag.MAC, requestId: 3, command: Command.FS_STAT, status: 0, payloadLength: statReq.length });
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 3, command: Command.FS_STAT, payload: statReq, mac: frameMac(key, 0n, header, statReq) }));
    await settle();
    assert.equal(r.frames().length, 2);
    assert.equal(r.closed(), true);
  });

  test("wire: AUTH with the right key_id but a forged proof is refused (and gets no sealed session)", async () => {
    await start();
    (await pairedClient()).close();
    const r = await rawClient();
    const cn = new Uint8Array(randomBytes(16));
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 1, command: Command.HELLO, payload: encodeTlv([
      [Tag.PROTOCOL, u16(1)], [Tag.PROTOCOL_MAX, u16(1)], [Tag.BRIDGE_NAME, str("raw")], [Tag.NONCE, cn]]) }));
    await settle();
    const dn = parseTlv(r.frames()[0]!.payload).first(Tag.NONCE)!;
    const forged = proofOf(psk, "auth", cn, dn);
    forged[0] = (forged[0] ?? 0) ^ 1;
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 2, command: Command.AUTH, payload: encodeTlv([[Tag.KEY_ID, keyIdOf(psk)], [Tag.PROOF, forged]]) }));
    await settle();
    const res = r.frames()[1]!;
    assert.equal(res.header.kind, Kind.ERR);
    assert.equal(res.header.status, Status.UNAUTHORIZED);
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 3, command: Command.FS_STAT, payload: encodeTlv([[Tag.PATH, str("/hello.txt")]]) }));
    await settle();
    assert.equal(r.frames()[2]?.header.status, Status.UNAUTHORIZED, "still unauthenticated");
    r.sock.destroy();
  });

  test("wire: a MAC on a frame before AUTH closes the connection", async () => {
    await start();
    const r = await rawClient();
    r.send(encodeFrame({ kind: Kind.REQ, requestId: 1, command: Command.PING, payload: new Uint8Array(0), mac: new Uint8Array(16) }));
    await settle();
    assert.equal(r.closed(), true);
  });

  /** A fake console that completes the handshake with valid MACs, then answers the next request `how`. */
  async function forgedReplyIsRejected(how: "unsealed" | "badmac" | "wrongcounter" | "honest") {
    const server = (await import("node:net")).createServer((sock) => {
      const dec = new FrameDecoder(65536);
      let cn: Uint8Array | null = null;
      const dn = new Uint8Array(randomBytes(16));
      let key: Uint8Array = new Uint8Array(32);
      let ctr = 0n;
      const sealedRes = (id: number, cmd: number, payload: Uint8Array, counter: bigint, k: Uint8Array) => {
        const header = encodeHeader({ version: 1, kind: Kind.RES, flags: Flag.MAC, requestId: id, command: cmd, status: 0, payloadLength: payload.length });
        return encodeFrame({ kind: Kind.RES, requestId: id, command: cmd, payload, mac: frameMac(k, counter, header, payload) });
      };
      sock.on("error", () => undefined);
      sock.on("data", (d: Buffer) => {
        for (const f of dec.push(new Uint8Array(d))) {
          const id = f.header.requestId;
          if (f.header.command === Command.HELLO) {
            cn = parseTlv(f.payload).first(Tag.NONCE)!;
            sock.write(encodeFrame({ kind: Kind.RES, requestId: id, command: Command.HELLO, payload: encodeTlv([
              [Tag.PROTOCOL, u16(1)], [Tag.PLATFORM, str("fake")], [Tag.AGENT_VERSION, str("0")], [Tag.NONCE, dn],
              [Tag.AUTH, str("required")], [Tag.MODE, str("READ_ONLY")], [Tag.MAX_FRAME, new Uint8Array([0, 0, 1, 0])],
              [Tag.DEVICE_ID, new Uint8Array(16)]]) }));
          } else if (f.header.command === Command.AUTH) {
            key = sessionKeyOf(psk, cn!, dn);
            sock.write(sealedRes(id, Command.AUTH, new Uint8Array(0), ctr++, key));
          } else {
            const body = encodeTlv([[Tag.TYPE, new Uint8Array([1])], [Tag.SIZE, new Uint8Array(8)], [Tag.MTIME, new Uint8Array(8)]]);
            if (how === "unsealed") sock.write(encodeFrame({ kind: Kind.RES, requestId: id, command: f.header.command, payload: body }));
            else if (how === "badmac") sock.write(sealedRes(id, f.header.command, body, ctr, new Uint8Array(randomBytes(32))));
            else if (how === "wrongcounter") sock.write(sealedRes(id, f.header.command, body, ctr + 5n, key));
            else sock.write(sealedRes(id, f.header.command, body, ctr++, key));
          }
        }
      });
    });
    await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
    const p = (server.address() as { port: number }).port;
    const c = await NdpClient.connect({ host: "127.0.0.1", port: p });
    try {
      await c.hello();
      await c.authenticate(psk);
      if (how === "honest") assert.equal((await c.stat("/x")).type, "file"); // the fake itself is sound
      else await assert.rejects(c.stat("/x"), (e) => e instanceof Error && /authentication failed/.test(e.message));
    } finally {
      c.close();
      server.close();
    }
  }
  test("wire: the fake console used below is itself valid (control)", () => forgedReplyIsRejected("honest"));
  test("wire: a client rejects an UNSEALED reply after AUTH (MITM)", () => forgedReplyIsRejected("unsealed"));
  test("wire: a client rejects a reply with a bad MAC", () => forgedReplyIsRejected("badmac"));
  test("wire: a client rejects a reply with an out-of-order counter", () => forgedReplyIsRejected("wrongcounter"));

  test("NdpClient.open pairs-aware: explains what to do when unpaired, and authenticates with the stored key", async () => {
    await start();
    const store = new KeyStore(join(dir, "ks", "keys.json"));
    await assert.rejects(NdpClient.open(opts(), store), (e) => e instanceof NdpRemoteError && /not paired.*ndev pair/.test(e.message));
    const c = await NdpClient.connect(opts());
    const info = await c.hello();
    const { psk: p, keyId } = await c.pair(code, "test-mac");
    c.close();
    store.save({ deviceId: toHex(info.deviceId!), psk: toHex(p), keyId: toHex(keyId), label: "test-mac", pairedAt: new Date().toISOString() });
    const { client } = await NdpClient.open(opts(), store);
    try {
      assert.equal(client.authenticated, true);
      assert.equal((await client.stat("/hello.txt")).type, "file");
    } finally {
      client.close();
    }
    assert.equal(store.find(info.deviceId!)?.lastHost, "127.0.0.1");
    if (process.platform !== "win32") assert.equal(statSync(store.path).mode & 0o777, 0o600, "the key file is private");
  });

  test("NdpClient.open explains a forgotten pairing (console reset)", async () => {
    await start();
    const store = new KeyStore(join(dir, "ks2", "keys.json"));
    const c = await NdpClient.connect(opts());
    const info = await c.hello();
    c.close();
    store.save({ deviceId: toHex(info.deviceId!), psk: toHex(psk), keyId: toHex(keyIdOf(psk)), label: "x", pairedAt: "now" });
    await assert.rejects(NdpClient.open(opts(), store), (e) => e instanceof NdpRemoteError && /rejected the stored pairing key/.test(e.message));
  });

  test("CLI: ndev pair (code on stdin) stores a private key; ndev ls then works; unpair removes it", async () => {
    await start();
    const keys = join(dir, "cli-keys.json");
    const env = { ...process.env, NDEV_KEYS_FILE: keys };
    const runCli = (args: string[], stdin?: string) =>
      new Promise<{ code: number | null; out: string; err: string }>((resolve) => {
        const p = spawn(process.execPath, [CLI, ...args], { env, stdio: ["pipe", "pipe", "pipe"] });
        let out = "", err = "";
        p.stdout.on("data", (d) => (out += d));
        p.stderr.on("data", (d) => (err += d));
        p.on("close", (code) => resolve({ code, out, err }));
        p.stdin.end(stdin ?? "");
      });
    const target = `127.0.0.1:${port}`;
    const before = await runCli(["ls", target, "/"]);
    assert.notEqual(before.code, 0);
    assert.match(before.err, /not paired/);
    const bad = await runCli(["pair", target, "--code-stdin", "--label", "cli-test"], "0000-0000-0000-0000\n");
    assert.notEqual(bad.code, 0);
    const good = await runCli(["pair", target, "--code-stdin", "--label", "cli-test"], `${CODE.toLowerCase()}\n`);
    assert.equal(good.code, 0, good.err);
    assert.match(good.out, /Paired/);
    assert.equal(statSync(keys).mode & 0o777, 0o600);
    const ls = await runCli(["ls", target, "/"]);
    assert.equal(ls.code, 0, ls.err);
    assert.match(ls.out, /hello\.txt/);
    assert.match((await runCli(["pairings"])).out, /cli-test/);
    assert.match((await runCli(["unpair", target])).out, /deleted/);
    assert.notEqual((await runCli(["ls", target, "/"])).code, 0);
  });
});
