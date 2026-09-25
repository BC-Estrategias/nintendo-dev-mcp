// The console's web page: HTTP serving, WebSocket upgrade rules, WebSocket framing, and the independence of the page's
// NDP session from the CLI/MCP one — all against the C server (host build) over real TCP.
import { strict as assert } from "node:assert";
import { execFile, type ChildProcess } from "node:child_process";
import { promisify } from "node:util";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { connect as tcpConnect, type Socket } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { gunzipSync } from "node:zlib";
import { createHash, randomBytes } from "node:crypto";
import { Command, FrameDecoder, Kind, NdpClient, Status, Tag, encodeFrame, encodeTlv, parseTlv, str, u16, type Frame } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const suite = SKIP ? describe.skip : describe;
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

interface HttpResponse { status: number; headers: Map<string, string>; body: Buffer; raw: Buffer }

/** Sends `text` on a fresh socket and reads until the server closes. */
function rawHttp(port: number, text: string | Buffer): Promise<HttpResponse> {
  return new Promise((resolve, reject) => {
    const s = tcpConnect({ host: "127.0.0.1", port });
    const chunks: Buffer[] = [];
    s.on("data", (d: Buffer) => chunks.push(d));
    s.on("error", reject);
    s.on("close", () => {
      const raw = Buffer.concat(chunks);
      const i = raw.indexOf("\r\n\r\n");
      const head = raw.subarray(0, i).toString("latin1").split("\r\n");
      const headers = new Map<string, string>();
      for (const l of head.slice(1)) headers.set(l.slice(0, l.indexOf(":")).toLowerCase(), l.slice(l.indexOf(":") + 1).trim());
      resolve({ status: Number(/HTTP\/1\.1 (\d+)/.exec(head[0] ?? "")?.[1]), headers, body: raw.subarray(i + 4), raw });
    });
    s.once("connect", () => s.write(text));
    setTimeout(() => s.destroy(), 4000).unref();
  });
}
const get = (port: number, path: string, extra = "", host = `127.0.0.1:${port}`, method = "GET") =>
  rawHttp(port, `${method} ${path} HTTP/1.1\r\nHost: ${host}\r\n${extra}\r\n`);

/** A minimal WebSocket client written by hand, so the tests control every byte on the wire. */
class Ws {
  readonly #s: Socket;
  #buf = Buffer.alloc(0);
  frames: Array<{ opcode: number; payload: Buffer }> = [];
  closed = false;
  headers = new Map<string, string>();
  status = 0;
  #onFrame: (() => void) | null = null;

  private constructor(s: Socket) {
    this.#s = s;
  }

  static open(port: number, opts: { origin?: string | null; host?: string; version?: string; key?: string } = {}): Promise<Ws | HttpResponse> {
    const key = opts.key ?? randomBytes(16).toString("base64");
    const host = opts.host ?? `127.0.0.1:${port}`;
    const origin = opts.origin === undefined ? `http://${host}` : opts.origin;
    const req = `GET /ws HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n${origin ? `Origin: ${origin}\r\n` : ""}Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: ${opts.version ?? "13"}\r\n\r\n`;
    return new Promise((resolve, reject) => {
      const s = tcpConnect({ host: "127.0.0.1", port });
      const ws = new Ws(s);
      let head = false;
      s.on("data", (d: Buffer) => {
        ws.#buf = Buffer.concat([ws.#buf, d]);
        if (!head) {
          const i = ws.#buf.indexOf("\r\n\r\n");
          if (i < 0) return;
          head = true;
          const lines = ws.#buf.subarray(0, i).toString("latin1").split("\r\n");
          ws.status = Number(/HTTP\/1\.1 (\d+)/.exec(lines[0] ?? "")?.[1]);
          for (const l of lines.slice(1)) ws.headers.set(l.slice(0, l.indexOf(":")).toLowerCase(), l.slice(l.indexOf(":") + 1).trim());
          ws.#buf = ws.#buf.subarray(i + 4);
          if (ws.status === 101) {
            const want = createHash("sha1").update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest("base64");
            assert.equal(ws.headers.get("sec-websocket-accept"), want, "Sec-WebSocket-Accept");
            resolve(ws);
          }
        }
        ws.#parse();
      });
      s.on("error", reject);
      s.on("close", () => {
        ws.closed = true;
        ws.#onFrame?.();
        if (!head) resolve({ status: 0, headers: new Map(), body: Buffer.alloc(0), raw: ws.#buf });
      });
      s.once("connect", () => s.write(req));
      // a refused upgrade is a normal HTTP answer followed by close
      s.on("end", () => {
        if (ws.status !== 101 && head) resolve({ status: ws.status, headers: ws.headers, body: ws.#buf, raw: ws.#buf });
      });
    });
  }

  #parse(): void {
    for (;;) {
      if (this.status !== 101 || this.#buf.length < 2) return;
      let n = this.#buf[1]! & 0x7f, off = 2;
      if (n === 126) { if (this.#buf.length < 4) return; n = this.#buf.readUInt16BE(2); off = 4; }
      else if (n === 127) { if (this.#buf.length < 10) return; n = Number(this.#buf.readBigUInt64BE(2)); off = 10; }
      if (this.#buf.length < off + n) return;
      this.frames.push({ opcode: this.#buf[0]! & 0x0f, payload: Buffer.from(this.#buf.subarray(off, off + n)) });
      this.#buf = this.#buf.subarray(off + n);
      this.#onFrame?.();
    }
  }

  /** One WebSocket frame, masked like a browser does (or not, to test the refusal). */
  send(opcode: number, payload: Uint8Array = new Uint8Array(0), o: { fin?: boolean; mask?: boolean } = {}): void {
    const fin = o.fin ?? true, mask = o.mask ?? true;
    const n = payload.length;
    const head = n < 126 ? Buffer.from([(fin ? 0x80 : 0) | opcode, (mask ? 0x80 : 0) | n])
      : n <= 0xffff ? Buffer.concat([Buffer.from([(fin ? 0x80 : 0) | opcode, (mask ? 0x80 : 0) | 126]), Buffer.from([n >> 8, n & 255])])
      : (() => { const b = Buffer.alloc(10); b[0] = (fin ? 0x80 : 0) | opcode; b[1] = (mask ? 0x80 : 0) | 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
    if (!mask) { this.#s.write(Buffer.concat([head, payload])); return; }
    const m = randomBytes(4);
    const body = Buffer.from(payload).map((c, i) => c ^ m[i % 4]!);
    this.#s.write(Buffer.concat([head, m, body]));
  }
  sendRaw(b: Uint8Array): void { this.#s.write(b); }

  async waitFrames(count: number, timeoutMs = 3000): Promise<void> {
    const t0 = Date.now();
    while (this.frames.length < count && !this.closed) {
      await new Promise<void>((r) => { this.#onFrame = r; setTimeout(r, 50); });
      if (Date.now() - t0 > timeoutMs) break;
    }
  }
  async waitClosed(timeoutMs = 3000): Promise<void> {
    const t0 = Date.now();
    while (!this.closed && Date.now() - t0 < timeoutMs) await sleep(20);
  }
  destroy(): void { this.#s.destroy(); }
}

const helloFrame = (id = 1) => encodeFrame({ kind: Kind.REQ, requestId: id, command: Command.HELLO, payload: encodeTlv([
  [Tag.PROTOCOL, u16(1)], [Tag.PROTOCOL_MAX, u16(1)], [Tag.BRIDGE_NAME, str("web-test")], [Tag.NONCE, new Uint8Array(randomBytes(16))]]) });
const pingFrame = (id: number) => encodeFrame({ kind: Kind.REQ, requestId: id, command: Command.PING, payload: encodeTlv([[Tag.PING_NONCE, new Uint8Array(8)]]) });

/** Decodes the NDP frames carried by the server's binary messages. */
function ndpFrames(ws: Ws): Frame[] {
  const dec = new FrameDecoder(65536);
  const out: Frame[] = [];
  for (const f of ws.frames) if (f.opcode === 2) out.push(...dec.push(new Uint8Array(f.payload)));
  return out;
}

suite("the console web page (HTTP + WebSocket)", () => {
  let dir: string;
  let proc: ChildProcess;
  let port: number;
  let web: number;

  before(async () => {
    dir = mkdtempSync(join(tmpdir(), "ndp-web-"));
    mkdirSync(join(dir, "sd/3ds/nintendo-dev-agent"), { recursive: true });
    writeFileSync(join(dir, "sd/3ds/nintendo-dev-agent/test.txt"), "hello");
    const a = await startAgent(["--root", join(dir, "sd"), "--web-port", "0"]);
    ({ proc, port } = a);
    web = a.webPort!;
  });
  after(() => {
    proc?.kill();
    rmSync(dir, { recursive: true, force: true });
  });

  test("GET / serves the gzip-compressed page with strict headers; HEAD sends none of the body", async () => {
    const r = await get(web, "/");
    assert.equal(r.status, 200);
    assert.equal(r.headers.get("content-encoding"), "gzip");
    assert.match(r.headers.get("content-type") ?? "", /text\/html/);
    assert.equal(Number(r.headers.get("content-length")), r.body.length);
    assert.match(gunzipSync(r.body).toString(), /<title>Nintendo Dev Agent<\/title>/);
    const csp = r.headers.get("content-security-policy") ?? "";
    assert.match(csp, /default-src 'none'/);
    assert.match(csp, /frame-ancestors 'none'/);
    assert.equal(r.headers.get("x-content-type-options"), "nosniff");
    assert.equal(r.headers.get("connection"), "close");
    const h = await get(web, "/index.html", "", undefined, "HEAD");
    assert.equal(h.status, 200);
    assert.equal(h.body.length, 0);
    assert.equal(h.headers.get("content-length"), String(r.body.length));
  });

  test("only the page is served: nothing from the SD card, no other methods, no bodies", async () => {
    assert.equal((await get(web, "/nope")).status, 404);
    assert.equal((await get(web, "/3ds/nintendo-dev-agent/test.txt")).status, 404);
    assert.equal((await get(web, "/..%2f..%2fetc/passwd")).status, 404);
    assert.equal((await get(web, "/", "", undefined, "POST")).status, 405);
    assert.equal((await get(web, "/", "", undefined, "DELETE")).status, 405);
    assert.equal((await get(web, "/favicon.ico")).status, 204);
    assert.equal((await rawHttp(web, `POST / HTTP/1.1\r\nHost: 127.0.0.1:${web}\r\nContent-Length: 4\r\n\r\nabcd`)).status, 413);
    assert.equal((await rawHttp(web, `GET / HTTP/2.0\r\nHost: 127.0.0.1:${web}\r\n\r\n`)).status, 505);
    assert.equal((await rawHttp(web, "GET /\r\n\r\n")).status, 400);
    assert.equal((await rawHttp(web, `GET / HTTP/1.1\r\nHost: 127.0.0.1:${web}\r\nX: ${"a".repeat(3000)}\r\n\r\n`)).status, 431);
  });

  test("DNS-rebinding defence: the Host must be the address the client connected to", async () => {
    assert.equal((await get(web, "/", "", "evil.example.com")).status, 403);
    assert.equal((await get(web, "/", "", "127.0.0.1.evil.com")).status, 403);
    assert.equal((await get(web, "/", "", `localhost:${web}`)).status, 200, "localhost is fine on a loopback listener");
    assert.equal((await rawHttp(web, "GET / HTTP/1.1\r\n\r\n")).status, 403, "no Host header");
  });

  test("the WebSocket upgrade: right version and key, same-origin only", async () => {
    assert.equal((await get(web, "/ws")).status, 426, "a plain GET of /ws asks for an upgrade");
    const bad = (await Ws.open(web, { version: "8" })) as HttpResponse;
    assert.equal(bad.status, 400);
    const badKey = (await Ws.open(web, { key: "short" })) as HttpResponse;
    assert.equal(badKey.status, 400);
    const cross = (await Ws.open(web, { origin: "http://evil.example.com" })) as HttpResponse;
    assert.equal(cross.status, 403, "another site's page cannot open the socket");
    const https = (await Ws.open(web, { origin: `https://127.0.0.1:${web}` })) as HttpResponse;
    assert.equal(https.status, 403);
    const nullOrigin = (await Ws.open(web, { origin: "null" })) as HttpResponse;
    assert.equal(nullOrigin.status, 403);
    const rebound = (await Ws.open(web, { host: "evil.example.com" })) as HttpResponse;
    assert.equal(rebound.status, 403);
    const ok = await Ws.open(web, { origin: null }); // a non-browser client sends no Origin
    assert.ok(ok instanceof Ws);
    (ok as Ws).destroy();
  });

  test("NDP over WebSocket: HELLO and PING, one frame per message, frames split and glued arbitrarily", async () => {
    const ws = (await Ws.open(web)) as Ws;
    try {
      ws.send(2, helloFrame(1));
      await ws.waitFrames(1);
      let f = ndpFrames(ws);
      assert.equal(f[0]?.header.kind, Kind.RES);
      assert.equal(f[0]?.header.requestId, 1);
      assert.equal(parseTlv(f[0]!.payload).str(Tag.PLATFORM), "host");
      // one NDP frame split over three WebSocket frames (a fragmented message)
      const p = pingFrame(2);
      ws.send(2, p.subarray(0, 7), { fin: false });
      ws.send(0, p.subarray(7, 20), { fin: false });
      ws.send(0, p.subarray(20), { fin: true });
      await ws.waitFrames(2);
      // two NDP frames in one message, one split across two separate messages
      const a = pingFrame(3), b = pingFrame(4);
      ws.send(2, Buffer.concat([a, b.subarray(0, 5)]));
      ws.send(2, b.subarray(5));
      await ws.waitFrames(4);
      f = ndpFrames(ws);
      assert.deepEqual(f.map((x) => x.header.requestId), [1, 2, 3, 4]);
      assert.ok(f.every((x) => x.header.kind === Kind.RES));
      // a WebSocket ping is answered with a pong carrying the same bytes
      ws.send(9, Buffer.from("beat"));
      await ws.waitFrames(5);
      const pong = ws.frames.find((x) => x.opcode === 10);
      assert.equal(pong?.payload.toString(), "beat");
    } finally {
      ws.destroy();
    }
  });

  test("protocol violations close the WebSocket with the right code (and nothing else breaks)", async () => {
    const closeCode = (ws: Ws) => ws.frames.find((x) => x.opcode === 8)?.payload.readUInt16BE(0);
    const violate = async (send: (ws: Ws) => void): Promise<number | undefined> => {
      const ws = (await Ws.open(web)) as Ws;
      try {
        send(ws);
        await ws.waitClosed();
        return closeCode(ws);
      } finally {
        ws.destroy();
      }
    };
    assert.equal(await violate((ws) => ws.send(2, helloFrame(1), { mask: false })), 1002, "unmasked frame");
    assert.equal(await violate((ws) => ws.send(1, Buffer.from("text"))), 1003, "text message");
    assert.equal(await violate((ws) => ws.send(3, Buffer.from("x"))), 1002, "reserved opcode");
    assert.equal(await violate((ws) => ws.send(0, Buffer.from("x"))), 1002, "continuation without a start");
    assert.equal(await violate((ws) => ws.send(9, Buffer.alloc(126))), 1002, "oversized control frame");
    assert.equal(await violate((ws) => ws.sendRaw(Buffer.concat([Buffer.from([0x82, 0xff]), (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(1n << 40n); return b; })(), Buffer.alloc(4)]))), 1009, "huge frame");
    { // bytes that are not an NDP frame: the server just drops the connection
      const g = (await Ws.open(web)) as Ws;
      g.send(2, Buffer.from("garbage that is not an NDP frame at all..."));
      await g.waitClosed();
      assert.equal(g.closed, true);
      g.destroy();
    }
    // a clean close handshake: the server echoes the code
    const ws = (await Ws.open(web)) as Ws;
    ws.send(8, Buffer.from([0x03, 0xe8]));
    await ws.waitClosed();
    assert.equal(closeCode(ws), 1000);
    ws.destroy();
    // the agent is still healthy
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      assert.equal((await c.hello()).platform, "host");
    } finally {
      c.close();
    }
  });

  test("the page and the CLI/MCP are independent clients: neither kicks the other; a new page replaces the old page only", async () => {
    const raw = await NdpClient.connect({ host: "127.0.0.1", port });
    await raw.hello();
    const ws1 = (await Ws.open(web)) as Ws;
    ws1.send(2, helloFrame(1));
    await ws1.waitFrames(1);
    assert.equal(ndpFrames(ws1).length, 1);
    // the raw client still works while the page is connected
    assert.equal((await raw.stat("/3ds/nintendo-dev-agent/test.txt")).size, 5);
    // a second page connection replaces the first one
    const ws2 = (await Ws.open(web)) as Ws;
    await ws1.waitClosed();
    assert.equal(ws1.closed, true, "the older page connection was closed");
    ws2.send(2, helloFrame(1));
    await ws2.waitFrames(1);
    assert.equal(ndpFrames(ws2).length, 1);
    // and the raw client was untouched by all of it
    assert.equal((await raw.stat("/3ds/nintendo-dev-agent/test.txt")).size, 5);
    // a new raw connection replaces the old raw one but not the page
    const raw2 = await NdpClient.connect({ host: "127.0.0.1", port });
    await raw2.hello();
    await sleep(100);
    assert.equal(raw.isClosed, true);
    ws2.send(2, pingFrame(9));
    await ws2.waitFrames(2);
    assert.equal(ndpFrames(ws2).length, 2, "the page still answers");
    raw2.close();
    ws2.destroy();
  });

  test("page slots: many simultaneous slow requests cannot exhaust the agent (extra ones are dropped, real ones still work)", async () => {
    const hold: Socket[] = [];
    for (let i = 0; i < 6; i++) {
      const s = tcpConnect({ host: "127.0.0.1", port: web });
      s.on("error", () => undefined);
      hold.push(s); // connects and says nothing
    }
    await sleep(200);
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      assert.equal((await c.hello()).platform, "host", "the NDP port is unaffected");
    } finally {
      c.close();
    }
    for (const s of hold) s.destroy();
    await sleep(100);
    assert.equal((await get(web, "/")).status, 200, "and the page works again after they left");
    // no socket may be leaked when the slots were full (when lsof is around to look)
    try {
      const { stdout } = await promisify(execFile)("lsof", ["-nP", "-a", "-p", String(proc.pid), "-iTCP"]);
      const open = stdout.split("\n").filter((l) => /ESTABLISHED|CLOSE_WAIT|FIN_WAIT/.test(l)).length;
      assert.ok(open <= 1, `no socket may be left open on the agent, found ${open}`);
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code === "ENOENT") return; // no lsof
      if (e instanceof assert.AssertionError) throw e;
    }
  });
});
