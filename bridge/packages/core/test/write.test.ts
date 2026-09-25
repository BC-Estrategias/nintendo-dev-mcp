// FS_WRITE / FS_MKDIR end to end: TypeScript client (and CLI) <-> C core over real TCP.
import { strict as assert } from "node:assert";
import { execFile, type ChildProcess } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { NdpClient, NdpRemoteError, NdpTransportError, Status } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const run = promisify(execFile);
const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;
const DIR = "/3ds/nintendo-dev-agent";
const code = (p: Promise<unknown>) => p.then(() => "ok", (e) => (e instanceof NdpRemoteError ? e.statusName : `local:${(e as Error).name}`));
const leftovers = (dir: string) => readdirSync(dir).filter((n) => /ndp-(tmp|old)/.test(n));

suite("writes over TCP (DEVELOPMENT mode)", () => {
  let root: string;
  let agentDir: string;
  let proc: ChildProcess;
  let port: number;
  const connect = () => NdpClient.connect({ host: "127.0.0.1", port });

  before(async () => {
    root = mkdtempSync(join(tmpdir(), "ndp-write-int-"));
    agentDir = join(root, DIR);
    mkdirSync(agentDir, { recursive: true });
    mkdirSync(join(agentDir, "config"));
    mkdirSync(join(root, "luma"));
    mkdirSync(join(root, "other"));
    ({ proc, port } = await startAgent(["--root", root, "--mode", "DEVELOPMENT"]));
  });
  after(() => {
    proc?.kill();
    rmSync(root, { recursive: true, force: true });
  });

  test("the acceptance test: create from-codex.txt, byte-exact, verified by the device", async () => {
    const c = await connect();
    try {
      const info = await c.hello();
      assert.equal(info.mode, "DEVELOPMENT");
      const text = "Codex was here.";
      const r = await c.writeBytes(`${DIR}/from-codex.txt`, new TextEncoder().encode(text));
      assert.equal(r.written, text.length);
      assert.equal(r.replaced, false);
      assert.equal(readFileSync(join(agentDir, "from-codex.txt"), "utf8"), text);
      assert.equal(Buffer.from(r.sha256).toString("hex"), createHash("sha256").update(text).digest("hex"));
      assert.deepEqual(leftovers(agentDir), []);
      assert.equal(Buffer.from((await c.readBytes(`${DIR}/from-codex.txt`)).data).toString(), text, "read back through the protocol");
    } finally {
      c.close();
    }
  });

  test("never overwrites silently; overwrite replaces; backup keeps the previous version", async () => {
    const c = await connect();
    try {
      await c.hello();
      const enc = (s: string) => new TextEncoder().encode(s);
      await c.writeBytes(`${DIR}/doc.txt`, enc("one"));
      assert.equal(await code(c.writeBytes(`${DIR}/doc.txt`, enc("two"))), "EXISTS");
      assert.equal(readFileSync(join(agentDir, "doc.txt"), "utf8"), "one", "untouched after the refusal");
      const r = await c.writeBytes(`${DIR}/doc.txt`, enc("two"), { overwrite: true });
      assert.equal(r.replaced, true);
      assert.equal(readFileSync(join(agentDir, "doc.txt"), "utf8"), "two");
      assert.equal(existsSync(join(agentDir, "doc.txt.bak")), false);
      await c.writeBytes(`${DIR}/doc.txt`, enc("three"), { overwrite: true, backup: true });
      assert.equal(readFileSync(join(agentDir, "doc.txt"), "utf8"), "three");
      assert.equal(readFileSync(join(agentDir, "doc.txt.bak"), "utf8"), "two");
      assert.deepEqual(leftovers(agentDir), []);
    } finally {
      c.close();
    }
  });

  test("a 5 MiB file uploads byte-exact with progress, in the device's temp-then-rename way", async () => {
    const big = randomBytes(5 * 1024 * 1024 + 77);
    const local = join(root, "local-big.bin");
    writeFileSync(local, big);
    const c = await connect();
    try {
      await c.hello();
      let last = 0;
      const r = await c.writeFile(`${DIR}/big.bin`, local, { onProgress: (n) => (last = n) });
      assert.equal(r.written, big.length);
      assert.equal(last, big.length);
      assert.deepEqual(readFileSync(join(agentDir, "big.bin")), big);
      assert.deepEqual(leftovers(agentDir), []);
      // and back
      assert.equal((await c.readBytes(`${DIR}/big.bin`, { maxBytes: 64 })).truncated, true);
    } finally {
      c.close();
    }
  });

  test("a wrong expected hash is refused by the device and leaves nothing behind", async () => {
    const c = await connect();
    try {
      await c.hello();
      const data = new TextEncoder().encode("payload");
      const wrong = new Uint8Array(32).fill(7);
      assert.equal(await code(c.write(`${DIR}/wrong.txt`, { size: data.length, sha256: wrong, chunks: [data] })), "HASH_MISMATCH");
      assert.equal(existsSync(join(agentDir, "wrong.txt")), false);
      assert.deepEqual(leftovers(agentDir), []);
      await c.writeBytes(`${DIR}/after.txt`, data); // the connection is still fine
    } finally {
      c.close();
    }
  });

  test("policy: outside the write root, protected zones, missing parent, directories, reserved names", async () => {
    const c = await connect();
    try {
      await c.hello();
      const d = new TextEncoder().encode("x");
      assert.equal(await code(c.writeBytes("/other/x.txt", d)), "PROTECTED_PATH");
      assert.equal(await code(c.writeBytes("/luma/x.txt", d)), "PROTECTED_PATH");
      assert.equal(await code(c.writeBytes("/boot.firm", d)), "PROTECTED_PATH");
      assert.equal(await code(c.writeBytes(`${DIR}/config/key`, d)), "PROTECTED_PATH");
      assert.equal(await code(c.writeBytes(`${DIR}/nodir/x.txt`, d)), "NOT_FOUND");
      assert.equal(await code(c.writeBytes(`${DIR}/config`, d, { overwrite: true })), "PROTECTED_PATH");
      assert.equal(await code(c.writeBytes(`${DIR}/x.ndp-tmp`, d)), "BAD_REQUEST");
      assert.equal(await code(c.writeBytes(`${DIR}/x.bak`, d)), "BAD_REQUEST");
      assert.equal(await code(c.writeBytes("relative.txt", d)), "local:PathInvalidError");
      assert.equal(existsSync(join(root, "other/x.txt")), false);
      assert.equal(existsSync(join(root, "luma/x.txt")), false);
      // mkdir
      await c.mkdir(`${DIR}/inbox`);
      assert.equal(await code(c.mkdir(`${DIR}/inbox`)), "EXISTS");
      assert.equal(await code(c.mkdir(`${DIR}/a/b/c`)), "NOT_FOUND");
      assert.equal(await code(c.mkdir("/other/d")), "PROTECTED_PATH");
      await c.writeBytes(`${DIR}/inbox/note.txt`, d);
      assert.equal(readFileSync(join(agentDir, "inbox/note.txt"), "utf8"), "x");
    } finally {
      c.close();
    }
  });

  test("dropping the connection mid-upload leaves no temp file; the agent keeps serving", async () => {
    for (let i = 0; i < 5; i++) {
      const c = await connect();
      await c.hello();
      const big = randomBytes(2 * 1024 * 1024);
      const sha256 = new Uint8Array(createHash("sha256").update(big).digest());
      let n = 0;
      const p = c
        .write(`${DIR}/dropped${i}.bin`, {
          size: big.length,
          sha256,
          chunks: (async function* () {
            for (let o = 0; o < big.length; o += 32768) {
              yield big.subarray(o, o + 32768);
              if (++n === 3) c.close(); // vanish after a few chunks
              await new Promise((r) => setTimeout(r, 1));
            }
          })(),
        })
        .catch(() => "failed");
      assert.equal(await p, "failed");
    }
    await new Promise((r) => setTimeout(r, 100));
    assert.deepEqual(leftovers(agentDir), []);
    for (let i = 0; i < 5; i++) assert.equal(existsSync(join(agentDir, `dropped${i}.bin`)), false);
    const d = await connect();
    try {
      await d.hello();
      await d.writeBytes(`${DIR}/still-works.txt`, new TextEncoder().encode("ok"));
    } finally {
      d.close();
    }
  });

  test("a replacing connection mid-upload aborts and cleans up the temp file", async () => {
    const a = await connect();
    await a.hello();
    const big = randomBytes(3 * 1024 * 1024);
    const sha256 = new Uint8Array(createHash("sha256").update(big).digest());
    const p = a
      .write(`${DIR}/replaced.bin`, {
        size: big.length,
        sha256,
        chunks: (async function* () {
          for (let o = 0; o < big.length; o += 32768) {
            yield big.subarray(o, o + 32768);
            await new Promise((r) => setTimeout(r, 2));
          }
        })(),
      })
      .catch((e) => e);
    await new Promise((r) => setTimeout(r, 30));
    const b = await connect();
    try {
      await b.hello();
      assert.ok((await p) instanceof Error, "the replaced client's upload fails");
      assert.deepEqual(leftovers(agentDir), []);
      assert.equal(existsSync(join(agentDir, "replaced.bin")), false);
    } finally {
      a.close();
      b.close();
    }
  });

  test("requests on one connection are serialized (concurrent writes do not interleave)", async () => {
    const c = await connect();
    try {
      await c.hello();
      const enc = new TextEncoder();
      const results = await Promise.all([1, 2, 3, 4].map((i) => c.writeBytes(`${DIR}/par${i}.txt`, enc.encode(`file ${i}`))));
      assert.deepEqual(results.map((r) => r.written), [6, 6, 6, 6]);
      for (let i = 1; i <= 4; i++) assert.equal(readFileSync(join(agentDir, `par${i}.txt`), "utf8"), `file ${i}`);
    } finally {
      c.close();
    }
  });

  test("CLI: put --text, put file, refuse overwrite, --replace, mkdir", async () => {
    const target = `127.0.0.1:${port}`;
    const cli = (...args: string[]) => run(process.execPath, [CLI, ...args], { encoding: "buffer", maxBuffer: 16 * 1024 * 1024 });
    const out = await cli("put", target, "--text", "Codex was here.", `${DIR}/cli.txt`);
    assert.match(out.stdout.toString(), /verified by the device/);
    assert.equal(readFileSync(join(agentDir, "cli.txt"), "utf8"), "Codex was here.");
    await assert.rejects(cli("put", target, "--text", "again", `${DIR}/cli.txt`), (e: { stderr: Buffer }) => /EXISTS/.test(e.stderr.toString()));
    await cli("put", target, "--text", "again", `${DIR}/cli.txt`, "--replace", "--backup");
    assert.equal(readFileSync(join(agentDir, "cli.txt"), "utf8"), "again");
    assert.equal(readFileSync(join(agentDir, "cli.txt.bak"), "utf8"), "Codex was here.");
    const local = join(root, "upload-me.bin");
    const data = randomBytes(300_000);
    writeFileSync(local, data);
    await cli("put", target, local, `${DIR}/uploaded.bin`);
    assert.deepEqual(readFileSync(join(agentDir, "uploaded.bin")), data);
    await cli("mkdir", target, `${DIR}/fromcli`);
    assert.equal(existsSync(join(agentDir, "fromcli")), true);
    await assert.rejects(cli("put", target, "--text", "x", "/luma/nope.txt"), (e: { stderr: Buffer }) => /PROTECTED_PATH/.test(e.stderr.toString()));
  });
});

suite("READ_ONLY mode forbids every write", () => {
  test("write and mkdir are refused; nothing is created", async () => {
    const root = mkdtempSync(join(tmpdir(), "ndp-write-ro-"));
    mkdirSync(join(root, DIR), { recursive: true });
    const { proc, port } = await startAgent(["--root", root, "--mode", "READ_ONLY"]);
    try {
      const c = await NdpClient.connect({ host: "127.0.0.1", port });
      const info = await c.hello();
      assert.equal(info.mode, "READ_ONLY");
      assert.equal(await code(c.writeBytes(`${DIR}/a.txt`, new TextEncoder().encode("x"))), "FORBIDDEN_MODE");
      assert.equal(await code(c.mkdir(`${DIR}/d`)), "FORBIDDEN_MODE");
      assert.deepEqual(readdirSync(join(root, DIR)), []);
      c.close();
    } finally {
      proc.kill();
      rmSync(root, { recursive: true, force: true });
    }
  });
});

void NdpTransportError;
void Status;
