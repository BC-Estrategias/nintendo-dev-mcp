// Integration: TypeScript client <-> the C core running inside the host agent (real TCP).
import { strict as assert } from "node:assert";
import { spawn, type ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
import { connect } from "node:net";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import {
  Command, DEFAULT_MAX_FRAME, Kind, NdpClient, NdpRemoteError, NdpTransportError, Status, Tag, encodeFrame,
  encodeTlv, isTransientConnectError,
} from "../src/index.ts";

const AGENT =
  process.env.NDP_HOST_AGENT ??
  fileURLToPath(new URL("../../../../build/agent/host/ndp-host-agent", import.meta.url));

// Set NDP_SKIP_INTEGRATION=1 where the C host agent is not built (e.g. Windows CI).
const SKIP = Boolean(process.env.NDP_SKIP_INTEGRATION);
const suite = SKIP ? describe.skip : describe;
const it = SKIP ? test.skip : test;

if (!SKIP && !existsSync(AGENT)) {
  throw new Error(
    `host agent not found at ${AGENT}\nBuild it first:  cmake -S agent -B build/agent && cmake --build build/agent\n` +
      `(or set NDP_HOST_AGENT=/path/to/ndp-host-agent)`,
  );
}

function startAgent(args: string[] = []): Promise<{ proc: ChildProcess; port: number }> {
  return new Promise((resolve, reject) => {
    const proc = spawn(AGENT, ["--port", "0", ...args], { stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    const timer = setTimeout(() => reject(new Error("agent did not start")), 5000);
    proc.stdout!.on("data", (d) => {
      out += d;
      const m = /LISTENING [\d.]+:(\d+)/.exec(out);
      if (m) {
        clearTimeout(timer);
        resolve({ proc, port: Number(m[1]) });
      }
    });
    proc.on("error", reject);
  });
}

suite("host agent over TCP", () => {
  let proc: ChildProcess;
  let port: number;
  before(async () => {
    if (!SKIP) ({ proc, port } = await startAgent());
  });
  after(() => proc?.kill());

  test("HELLO then PING", async () => {
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      const info = await c.hello();
      assert.equal(info.protocol, 1);
      assert.equal(info.platform, "host");
      assert.equal(info.agentVersion, "0.1.1");
      assert.equal(info.mode, "READ_ONLY");
      assert.equal(info.auth, "none");
      assert.equal(info.maxFrame, DEFAULT_MAX_FRAME);
      assert.equal(info.deviceNonce.length, 16);
      const rtt = await c.ping();
      assert.ok(rtt >= 0 && rtt < 1000);
      for (let i = 0; i < 20; i++) await c.ping(); // repeated requests on one connection
    } finally {
      c.close();
    }
  });

  test("device nonce is fresh per HELLO", async () => {
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      const a = await c.hello();
      const b = await c.hello();
      assert.notDeepEqual(a.deviceNonce, b.deviceNonce);
    } finally {
      c.close();
    }
  });

  test("PING before HELLO -> ERR HELLO_REQUIRED", async () => {
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await assert.rejects(c.ping(), (e: unknown) => e instanceof NdpRemoteError && e.status === Status.HELLO_REQUIRED);
      await c.hello(); // the connection stays usable after an ERR
      await c.ping();
    } finally {
      c.close();
    }
  });

  test("unknown command -> ERR UNSUPPORTED_COMMAND", async () => {
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await c.hello();
      await assert.rejects(c.request(0x7777), (e: unknown) => e instanceof NdpRemoteError && e.status === Status.UNSUPPORTED_COMMAND);
    } finally {
      c.close();
    }
  });

  test("garbage on the wire closes the connection", async () => {
    const sock = connect({ host: "127.0.0.1", port });
    await new Promise<void>((r) => sock.once("connect", () => r()));
    sock.write(Buffer.from("GET / HTTP/1.1\r\n\r\nxxxxxxxxxxxx"));
    await new Promise<void>((r) => sock.once("close", () => r()));
  });

  test("oversized payload is rejected without waiting for it", async () => {
    const sock = connect({ host: "127.0.0.1", port });
    await new Promise<void>((r) => sock.once("connect", () => r()));
    const hdr = encodeFrame({ kind: Kind.REQ, requestId: 1, command: Command.PING });
    new DataView(hdr.buffer).setUint32(16, 10_000_000, true); // payload_len claims 10 MB, none is sent
    sock.write(hdr);
    await new Promise<void>((r) => sock.once("close", () => r()));
  });

  test("agent serves a new client after the previous one disconnects", async () => {
    for (let i = 0; i < 3; i++) {
      const c = await NdpClient.connect({ host: "127.0.0.1", port });
      await c.hello();
      await c.ping();
      c.close();
    }
  });

  test("a new connection replaces the previous client (no lock-out by a stale connection)", async () => {
    const a = await NdpClient.connect({ host: "127.0.0.1", port });
    await a.hello();
    const b = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await b.hello();
      await b.ping();
      await assert.rejects(a.ping(), NdpTransportError);
    } finally {
      a.close();
      b.close();
    }
  });

  test("a client that vanishes mid-frame does not break the agent", async () => {
    const sock = connect({ host: "127.0.0.1", port });
    await new Promise<void>((r) => sock.once("connect", () => r()));
    sock.write(encodeFrame({ kind: Kind.REQ, requestId: 1, command: Command.PING }).subarray(0, 11)); // half a header
    await new Promise((r) => setTimeout(r, 50));
    sock.destroy();
    await new Promise((r) => setTimeout(r, 50));
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await c.hello();
      await c.ping();
    } finally {
      c.close();
    }
  });

  test("a frame split into single bytes still works", async () => {
    const sock = connect({ host: "127.0.0.1", port });
    await new Promise<void>((r) => sock.once("connect", () => r()));
    sock.setNoDelay(true);
    const chunks: Buffer[] = [];
    sock.on("data", (d) => chunks.push(typeof d === "string" ? Buffer.from(d) : d));
    const hello = encodeFrame({
      kind: Kind.REQ, requestId: 5, command: Command.HELLO,
      payload: encodeTlv([[Tag.PROTOCOL, new Uint8Array([1, 0])], [Tag.PROTOCOL_MAX, new Uint8Array([1, 0])], [Tag.NONCE, new Uint8Array(16)]]),
    });
    for (const b of hello) {
      sock.write(Buffer.from([b]));
      await new Promise((r) => setTimeout(r, 1));
    }
    await new Promise((r) => setTimeout(r, 100));
    const got = Buffer.concat(chunks);
    assert.ok(got.length > 20, "response received");
    assert.equal(got.readUInt32LE(8), 5, "request_id echoed");
    assert.equal(got[5], Kind.RES);
    sock.destroy();
  });
});

it("idle clients are dropped after the idle timeout", async () => {
  const { proc, port } = await startAgent(["--idle-ms", "300"]);
  try {
    const a = await NdpClient.connect({ host: "127.0.0.1", port });
    await a.hello();
    await new Promise((r) => setTimeout(r, 900));
    await assert.rejects(a.ping(), NdpTransportError);
    a.close();
    const b = await NdpClient.connect({ host: "127.0.0.1", port });
    await b.hello();
    b.close();
  } finally {
    proc.kill();
  }
});

it("connecting to a closed port fails fast (ECONNREFUSED is not retried)", async () => {
  const t0 = Date.now();
  await assert.rejects(
    NdpClient.connect({ host: "127.0.0.1", port: 1, connectTimeoutMs: 1000 }),
    (e: unknown) => e instanceof NdpTransportError && e.code === "ECONNREFUSED",
  );
  assert.ok(Date.now() - t0 < 200, "no retry pauses for a refused connection");
});

it("transient connect errors are classified for retry", () => {
  for (const c of ["EHOSTUNREACH", "EHOSTDOWN", "ENETUNREACH", "ETIMEDOUT"]) assert.equal(isTransientConnectError(c), true, c);
  for (const c of ["ECONNREFUSED", "EACCES", undefined]) assert.equal(isTransientConnectError(c), false, String(c));
});

it("request times out when the agent never answers", async () => {
  const { createServer } = await import("node:net");
  const silent = createServer(() => {});
  await new Promise<void>((r) => silent.listen(0, "127.0.0.1", () => r()));
  const p = (silent.address() as { port: number }).port;
  const c = await NdpClient.connect({ host: "127.0.0.1", port: p, requestTimeoutMs: 200 });
  try {
    await assert.rejects(c.hello(), (e: unknown) => e instanceof NdpTransportError && /no response/.test(e.message));
  } finally {
    c.close();
    silent.close();
  }
});
