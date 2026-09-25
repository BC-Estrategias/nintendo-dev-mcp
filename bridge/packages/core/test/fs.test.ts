// FS_LIST / FS_STAT / FS_READ end to end: TypeScript client (and CLI) <-> C core over real TCP,
// serving a temporary directory as the "SD card".
import { strict as assert } from "node:assert";
import { execFile } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { type ChildProcess } from "node:child_process";
import { NdpClient, NdpRemoteError, NdpTransportError, PathInvalidError, Status } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const run = promisify(execFile);
const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;

suite("filesystem over TCP", () => {
  let root: string;
  let proc: ChildProcess;
  let port: number;
  let big: Buffer;
  const HELLO = Buffer.from("Hello from Nintendo 3DS\n");
  const connect = () => NdpClient.connect({ host: "127.0.0.1", port });

  before(async () => {
    root = mkdtempSync(join(tmpdir(), "ndp-fs-int-"));
    mkdirSync(join(root, "3ds/nintendo-dev-agent/config"), { recursive: true });
    mkdirSync(join(root, "dir"));
    mkdirSync(join(root, "emptydir"));
    writeFileSync(join(root, "hello.txt"), HELLO);
    writeFileSync(join(root, "3ds/nintendo-dev-agent/agent.log"), "line 1\nline 2\n");
    writeFileSync(join(root, "3ds/nintendo-dev-agent/config/key"), "TOP-SECRET");
    big = randomBytes(5 * 1024 * 1024 + 123);
    writeFileSync(join(root, "big.bin"), big);
    writeFileSync(join(root, "unicode-café-日本.txt"), "ok");
    for (let i = 0; i < 250; i++) writeFileSync(join(root, "dir", `f${String(i).padStart(3, "0")}.txt`), "x");
    symlinkSync("/etc/hosts", join(root, "escape.txt"));
    ({ proc, port } = await startAgent(["--root", root]));
  });
  after(() => {
    proc?.kill();
    rmSync(root, { recursive: true, force: true });
  });

  test("stat", async () => {
    const c = await connect();
    try {
      await c.hello();
      const f = await c.stat("/hello.txt");
      assert.deepEqual({ type: f.type, size: f.size }, { type: "file", size: HELLO.length });
      assert.ok(f.mtime !== null && f.mtime > 1_600_000_000);
      assert.equal((await c.stat("/dir")).type, "dir");
      assert.equal((await c.stat("/")).type, "dir");
    } finally {
      c.close();
    }
  });

  test("list, including paging over 250 entries and non-ASCII names", async () => {
    const c = await connect();
    try {
      await c.hello();
      const root_ = await c.list("/");
      const names = root_.map((e) => e.name).sort();
      assert.deepEqual(names, ["3ds", "big.bin", "dir", "emptydir", "hello.txt", "unicode-café-日本.txt"]);
      assert.equal(root_.find((e) => e.name === "hello.txt")?.size, HELLO.length);
      assert.equal(root_.find((e) => e.name === "dir")?.type, "dir");
      assert.ok(!names.includes("escape.txt"), "symlinks are invisible");
      const d = await c.list("/dir");
      assert.equal(d.length, 250);
      assert.equal(new Set(d.map((e) => e.name)).size, 250);
      assert.deepEqual(await c.list("/emptydir"), []);
    } finally {
      c.close();
    }
  });

  test("read small file, byte-exact, with SHA-256 verified against the agent", async () => {
    const c = await connect();
    try {
      await c.hello();
      const r = await c.readBytes("/hello.txt");
      assert.equal(Buffer.from(r.data).toString(), HELLO.toString());
      assert.equal(r.verified, true);
      assert.equal(r.truncated, false);
      assert.equal(r.totalSize, HELLO.length);
    } finally {
      c.close();
    }
  });

  test("read a 5 MiB file in streaming, byte-exact and hash-verified", async () => {
    const c = await connect();
    try {
      await c.hello();
      const h = createHash("sha256");
      let n = 0;
      let frames = 0;
      const r = await c.read("/big.bin", { chunk: 32768 }, (chunk) => {
        h.update(chunk);
        n += chunk.length;
        frames++;
      });
      assert.equal(n, big.length);
      assert.equal(h.digest("hex"), createHash("sha256").update(big).digest("hex"));
      assert.equal(r.verified, true);
      assert.equal(frames, Math.ceil(big.length / 32768));
    } finally {
      c.close();
    }
  });

  test("ranges, truncation and tail", async () => {
    const c = await connect();
    try {
      await c.hello();
      const mid = await c.readBytes("/big.bin", { offset: 1000, length: 5000 });
      assert.deepEqual(Buffer.from(mid.data), big.subarray(1000, 6000));
      const capped = await c.readBytes("/big.bin", { maxBytes: 1000 });
      assert.equal(capped.bytes, 1000);
      assert.equal(capped.truncated, true);
      assert.equal(capped.totalSize, big.length);
      const end = await c.readBytes("/big.bin", { offset: big.length - 10 });
      assert.deepEqual(Buffer.from(end.data), big.subarray(big.length - 10));
      assert.equal(end.truncated, false);
      const past = await c.readBytes("/big.bin", { offset: big.length });
      assert.equal(past.bytes, 0);
      await assert.rejects(c.readBytes("/big.bin", { offset: big.length + 1 }), (e: unknown) => e instanceof NdpRemoteError && e.status === Status.BAD_REQUEST);
    } finally {
      c.close();
    }
  });

  test("errors: missing, directory, protected zone, symlink, invalid path", async () => {
    const c = await connect();
    try {
      await c.hello();
      const code = (p: Promise<unknown>) => p.then(() => "ok", (e) => (e instanceof NdpRemoteError ? e.statusName : `local:${(e as Error).name}`));
      assert.equal(await code(c.stat("/nope")), "NOT_FOUND");
      assert.equal(await code(c.readBytes("/nope")), "NOT_FOUND");
      assert.equal(await code(c.readBytes("/dir")), "BAD_REQUEST");
      assert.equal(await code(c.list("/hello.txt")), "BAD_REQUEST");
      assert.equal(await code(c.readBytes("/3ds/nintendo-dev-agent/config/key")), "PROTECTED_PATH");
      assert.equal(await code(c.list("/3ds/nintendo-dev-agent/config")), "PROTECTED_PATH");
      assert.equal(await code(c.readBytes("/escape.txt")), "NOT_FOUND");
      assert.equal(await code(c.stat("relative")), "local:PathInvalidError");
      assert.equal(await code(c.stat("/a/../b")), "local:PathInvalidError");
      // the connection is still healthy after all those errors
      assert.equal((await c.readBytes("/3ds/nintendo-dev-agent/agent.log")).data.length, 14);
    } finally {
      c.close();
    }
  });

  test("the agent rejects a path the client-side check would have refused (defense in depth)", async () => {
    const c = await connect();
    try {
      await c.hello();
      const { Command, Tag, encodeTlv, str } = await import("../src/index.ts");
      for (const bad of ["/a/../b", "relative", "/a:b", "/NINTEN~1/x", "/a\\b"]) {
        await assert.rejects(
          c.request(Command.FS_STAT, encodeTlv([[Tag.PATH, str(bad)]])),
          (e: unknown) => e instanceof NdpRemoteError && e.status === Status.PATH_INVALID,
          bad,
        );
      }
      await assert.rejects(c.request(Command.FS_STAT), (e: unknown) => e instanceof NdpRemoteError && e.status === Status.BAD_REQUEST);
    } finally {
      c.close();
    }
  });

  test("a consumer error mid-transfer closes the connection; the agent keeps serving", async () => {
    const c = await connect();
    await c.hello();
    let n = 0;
    await assert.rejects(
      c.read("/big.bin", {}, () => {
        if (++n === 3) throw new Error("consumer failed");
      }),
      /consumer failed/,
    );
    await assert.rejects(c.ping(), NdpTransportError);
    c.close();
    const d = await connect();
    try {
      await d.hello();
      assert.equal((await d.readBytes("/hello.txt")).data.length, HELLO.length);
    } finally {
      d.close();
    }
  });

  test("dropping the connection mid-transfer, and a replacing connection, leave the agent healthy", async () => {
    for (let i = 0; i < 5; i++) {
      const c = await connect();
      await c.hello();
      const p = c.read("/big.bin", {}, () => {}).catch(() => {});
      await new Promise((r) => setTimeout(r, 5));
      c.close();
      await p;
    }
    const a = await connect();
    await a.hello();
    const p = a.read("/big.bin", {}, () => {}).catch((e) => e);
    await new Promise((r) => setTimeout(r, 5));
    const b = await connect(); // replaces `a` while it is streaming
    try {
      await b.hello();
      assert.equal((await b.readBytes("/hello.txt")).data.length, HELLO.length);
      assert.ok((await p) instanceof Error, "the replaced client's transfer fails");
    } finally {
      a.close();
      b.close();
    }
  });

  test("requests are serialized on one connection (concurrent reads do not interleave)", async () => {
    const c = await connect();
    try {
      await c.hello();
      const [a, b, l] = await Promise.all([c.readBytes("/hello.txt"), c.readBytes("/big.bin", { maxBytes: 200000 }), c.list("/dir")]);
      assert.equal(a.bytes, HELLO.length);
      assert.equal(b.bytes, 200000);
      assert.equal(l.length, 250);
    } finally {
      c.close();
    }
  });

  test("CLI: ls, stat, cat --tail, get", async () => {
    const target = `127.0.0.1:${port}`;
    const cli = (...args: string[]) => run(process.execPath, [CLI, ...args], { maxBuffer: 16 * 1024 * 1024, encoding: "buffer" });
    const ls = await cli("ls", target, "/");
    assert.match(ls.stdout.toString(), /d\s+0\s+3ds\//);
    assert.match(ls.stdout.toString(), /-\s+24\s+hello\.txt/);
    const stat = await cli("stat", target, "/hello.txt");
    assert.match(stat.stdout.toString(), /size\s+24 bytes/);
    const tail = await cli("cat", target, "/3ds/nintendo-dev-agent/agent.log", "--tail", "7");
    assert.equal(tail.stdout.toString(), "line 2\n");
    const local = join(root, "downloaded.bin");
    const get = await cli("get", target, "/big.bin", local);
    assert.match(get.stdout.toString(), /verified against the agent/);
    assert.deepEqual(readFileSync(local), big);
    await assert.rejects(cli("cat", target, "/nope"), (e: { stderr: Buffer }) => /NOT_FOUND/.test(e.stderr.toString()));
    await assert.rejects(cli("cat", target, "/3ds/nintendo-dev-agent/config/key"), (e: { stderr: Buffer }) => /PROTECTED_PATH/.test(e.stderr.toString()));
  });
});

const nofs = SKIP ? describe.skip : describe;
nofs("agent started without --root", () => {
  test("FS commands are unsupported", async () => {
    const { proc, port } = await startAgent();
    try {
      const c = await NdpClient.connect({ host: "127.0.0.1", port });
      await c.hello();
      await assert.rejects(c.stat("/x"), (e: unknown) => e instanceof NdpRemoteError && e.status === Status.UNSUPPORTED_COMMAND);
      c.close();
    } finally {
      proc.kill();
    }
  });
});

void PathInvalidError;
