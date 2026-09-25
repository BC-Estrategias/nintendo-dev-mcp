// The folders the console's owner opened (spec §11): navigation ("traversal"), ACCESS_INFO, the on-disk list, and the
// CLI/MCP-facing behaviour — TypeScript client <-> C agent (host build) over real TCP.
import { strict as assert } from "node:assert";
import { execFile } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import { type ChildProcess } from "node:child_process";
import { NdpClient, NdpRemoteError, Status } from "../src/index.ts";
import { SKIP, startAgent } from "./helpers.ts";

const run = promisify(execFile);
const CLI = fileURLToPath(new URL("../../cli/src/main.ts", import.meta.url));
const V = JSON.parse(readFileSync(fileURLToPath(new URL("../../../../docs/protocol/test-vectors/vectors.json", import.meta.url)), "utf8"));

/** The "NDPA" file the console writes (spec §11.2), built independently here. */
function accessFile(entries: Array<[string, number]>): Buffer {
  const parts: Buffer[] = [Buffer.from([0x4e, 0x44, 0x50, 0x41, 1, entries.length, 0, 0])];
  for (const [p, lv] of entries) parts.push(Buffer.from([lv, Buffer.byteLength(p)]), Buffer.from(p));
  const body = Buffer.concat(parts);
  return Buffer.concat([body, createHash("sha256").update(body).digest()]);
}

test("the TypeScript file builder produces the reference bytes", () => {
  for (const sc of V.access) assert.equal(accessFile(sc.entries).toString("hex"), sc.file_hex, sc.name);
});

const suite = SKIP ? describe.skip : describe;

suite("owner-chosen folders over TCP", () => {
  let dir: string;
  let root: string;
  let procs: ChildProcess[] = [];

  const names = (es: Array<{ name: string }>) => es.map((e) => e.name).sort();

  before(() => {
    dir = mkdtempSync(join(tmpdir(), "ndp-access-"));
    root = join(dir, "sd");
    for (const d of ["roms/gba", "roms/nds", "luma/plugins", "luma/payloads", "3ds/nintendo-dev-agent", "3ds/other", "cias"]) mkdirSync(join(root, d), { recursive: true });
    writeFileSync(join(root, "roms/gba/game.gba"), "GBA");
    writeFileSync(join(root, "roms/nds/x.nds"), "NDS");
    writeFileSync(join(root, "roms/note.txt"), "n");
    writeFileSync(join(root, "luma/secret"), "S");
    writeFileSync(join(root, "top.txt"), "t");
    writeFileSync(join(root, "cias/a.cia"), "C");
  });
  after(() => {
    for (const p of procs) p.kill();
    rmSync(dir, { recursive: true, force: true });
  });

  async function agentFor(entries: Array<[string, number]>) {
    const f = join(dir, `access-${procs.length}.bin`);
    writeFileSync(f, accessFile(entries));
    const a = await startAgent(["--root", root, "--mode", "DEVELOPMENT", "--access-file", f]);
    procs.push(a.proc);
    return a;
  }

  test("nothing configured: only the agent's own folder is open (secure default)", async () => {
    const { port } = await agentFor([]);
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await c.hello();
      assert.deepEqual(await c.accessInfo(), { mode: "DEVELOPMENT", readRoots: ["/3ds/nintendo-dev-agent"], writeRoots: ["/3ds/nintendo-dev-agent"] });
      assert.deepEqual(names(await c.list("/")), ["3ds"], "the only way in");
      assert.deepEqual(names(await c.list("/3ds")), ["nintendo-dev-agent"]);
      await assert.rejects(c.readBytes("/top.txt"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
      await assert.rejects(c.stat("/luma"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
    } finally {
      c.close();
    }
  });

  test("deep folders: navigate down to them, read inside, nothing beside", async () => {
    const { port } = await agentFor([["/roms/gba", 1], ["/cias", 2]]);
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await c.hello();
      const info = await c.accessInfo();
      assert.deepEqual(info.readRoots, ["/3ds/nintendo-dev-agent", "/roms/gba", "/cias"]);
      assert.deepEqual(info.writeRoots, ["/3ds/nintendo-dev-agent", "/cias"]);
      assert.deepEqual(names(await c.list("/")), ["3ds", "cias", "roms"]);
      assert.deepEqual(names(await c.list("/roms")), ["gba"]);
      assert.equal((await c.stat("/roms")).type, "dir");
      assert.deepEqual(names(await c.list("/roms/gba")), ["game.gba"]);
      assert.equal(Buffer.from((await c.readBytes("/roms/gba/game.gba")).data).toString(), "GBA");
      for (const bad of ["/roms/nds", "/roms/note.txt", "/luma", "/top.txt", "/3ds/other"])
        await assert.rejects(c.stat(bad), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH, bad);
      await assert.rejects(c.readBytes("/roms/note.txt"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
      await assert.rejects(c.list("/roms/nds"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
      // read-only folder: no writes; writable folder: writes work
      await assert.rejects(c.mkdir("/roms/gba/new"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
      const data = Buffer.from("cia!");
      const w = await c.write("/cias/b.cia", { size: data.length, sha256: new Uint8Array(createHash("sha256").update(data).digest()), chunks: [data] });
      assert.equal(w.written, 4);
      // the write root also works with the trash: delete moves into <write root>/.ndp-trash
      const d = await c.delete("/cias/b.cia");
      assert.ok(d.trashPath.startsWith("/cias/.ndp-trash/"), d.trashPath);
    } finally {
      c.close();
    }
  });

  test("dot-dot and case tricks do not widen the view", async () => {
    const { port } = await agentFor([["/roms/gba", 1]]);
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    try {
      await c.hello();
      await assert.rejects(c.list("/roms/gba/../nds"), (e) => e instanceof Error);
      assert.deepEqual(names(await c.list("/ROMS/GBA")), ["game.gba"], "FAT is case-insensitive: the same folder");
      await assert.rejects(c.list("/ROMS/NDS"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
    } finally {
      c.close();
    }
  });

  test("a corrupt access file falls back to the workspace only (fail closed)", async () => {
    const f = join(dir, "bad-access.bin");
    const good = accessFile([["/roms", 2]]);
    good[9] = (good[9] ?? 0) ^ 1;
    writeFileSync(f, good);
    const a = await startAgent(["--root", root, "--mode", "DEVELOPMENT", "--access-file", f]);
    procs.push(a.proc);
    const c = await NdpClient.connect({ host: "127.0.0.1", port: a.port });
    try {
      await c.hello();
      assert.deepEqual((await c.accessInfo()).readRoots, ["/3ds/nintendo-dev-agent"]);
    } finally {
      c.close();
    }
  });

  test("/luma stays protected, except its plugins and titles folders (spec §11)", async () => {
    const { port } = await agentFor([["/luma", 1], ["/luma/plugins", 2]]);
    const c = await NdpClient.connect({ host: "127.0.0.1", port });
    const put = (p: string) => {
      const d = Buffer.from("x");
      return c.write(p, { size: 1, sha256: new Uint8Array(createHash("sha256").update(d).digest()), chunks: [d] });
    };
    try {
      await c.hello();
      assert.equal((await c.list("/luma")).length, 3, "readable");
      assert.equal((await put("/luma/plugins/game.3gx")).written, 1);
      const d = await c.delete("/luma/plugins/game.3gx");
      assert.ok(d.trashPath.startsWith("/luma/plugins/.ndp-trash/"), d.trashPath);
      for (const bad of ["/luma/config.ini", "/luma/payloads/x.firm", "/luma/pluginsx"])
        await assert.rejects(put(bad), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH, bad);
      await assert.rejects(c.mkdir("/luma/newdir"), (e) => e instanceof NdpRemoteError && e.status === Status.PROTECTED_PATH);
    } finally {
      c.close();
    }
  });

  test("CLI: ndev access", async () => {
    const { port } = await agentFor([["/roms/gba", 1], ["/cias", 2]]);
    const { stdout } = await run(process.execPath, [CLI, "access", `127.0.0.1:${port}`]);
    assert.match(stdout, /mode DEVELOPMENT/);
    assert.match(stdout, /\/roms\/gba\n/);
    assert.match(stdout, /\/cias {3}\(also writable\)/);
  });
});
