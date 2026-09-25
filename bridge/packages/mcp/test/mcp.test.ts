// The MCP server end to end: a scripted stdio client (using the exact handshakes that Claude Code 2.1.177
// and Codex 0.134 send) -> ndev-mcp -> TCP -> the C agent core running in the host agent.
import { strict as assert } from "node:assert";
import { spawn, type ChildProcess } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { createInterface } from "node:readline";
import { randomBytes } from "node:crypto";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, test } from "node:test";
import { fileURLToPath } from "node:url";
import { createServer } from "node:net";
import { Command, FrameDecoder, Kind, Tag, encodeFrame, encodeTlv, str, u16, u32 } from "@ndev/core";
import { SKIP, startAgent } from "../../core/test/helpers.ts";

const MAIN = fileURLToPath(new URL("../src/main.ts", import.meta.url));
const suite = SKIP ? describe.skip : describe;
const DIR = "/3ds/nintendo-dev-agent";

const HANDSHAKES = {
  claude: { protocolVersion: "2025-11-25", capabilities: { roots: {}, elicitation: {} }, clientInfo: { name: "claude-code", version: "2.1.177" } },
  codex: { protocolVersion: "2025-06-18", capabilities: { elicitation: {} }, clientInfo: { name: "codex-mcp-client", version: "0.134.0" } },
} as const;

interface ToolReply {
  isError?: boolean;
  content: Array<{ type: string; text: string }>;
  structuredContent?: Record<string, any>;
}

class McpDriver {
  readonly proc: ChildProcess;
  #pending = new Map<number, (m: any) => void>();
  #id = 0;
  stderr = "";
  constructor(args: string[]) {
    this.proc = spawn(process.execPath, [MAIN, ...args], { stdio: ["pipe", "pipe", "pipe"] });
    this.proc.stderr!.on("data", (d) => (this.stderr += d));
    createInterface({ input: this.proc.stdout! }).on("line", (l) => {
      try {
        const m = JSON.parse(l);
        this.#pending.get(m.id)?.(m);
      } catch {
        assert.fail(`non-JSON on the protocol stream: ${l.slice(0, 200)}`);
      }
    });
  }
  rpc(method: string, params: unknown, timeoutMs = 60_000): Promise<any> {
    const id = this.#id++;
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => reject(new Error(`timeout: ${method}`)), timeoutMs);
      this.#pending.set(id, (m) => {
        clearTimeout(t);
        resolve(m);
      });
      this.proc.stdin!.write(`${JSON.stringify({ jsonrpc: "2.0", id, method, params })}\n`);
    });
  }
  async init(era: keyof typeof HANDSHAKES = "claude") {
    const r = await this.rpc("initialize", HANDSHAKES[era]);
    this.proc.stdin!.write(`${JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" })}\n`);
    return r.result;
  }
  async call(name: string, args: Record<string, unknown> = {}): Promise<ToolReply> {
    const r = await this.rpc("tools/call", { name, arguments: args });
    if (r.error) throw new Error(`protocol error for ${name}: ${JSON.stringify(r.error)}`);
    return r.result as ToolReply;
  }
  close() {
    this.proc.kill();
  }
}

const text = (r: ToolReply) => r.content.map((c) => c.text).join("\n");

suite("ndev-mcp over stdio", () => {
  let root: string;
  let agentDir: string;
  let localRoot: string;
  let outside: string;
  let agent: { proc: ChildProcess; port: number };
  let ro: { proc: ChildProcess; port: number };
  let auditFile: string;
  let mcp: McpDriver;
  const HELLO = "Hello from Nintendo 3DS";

  before(async () => {
    root = mkdtempSync(join(tmpdir(), "ndev-mcp-sd-"));
    localRoot = mkdtempSync(join(tmpdir(), "ndev-mcp-local-"));
    outside = mkdtempSync(join(tmpdir(), "ndev-mcp-outside-"));
    agentDir = join(root, DIR);
    mkdirSync(join(agentDir, "config"), { recursive: true });
    writeFileSync(join(agentDir, "test.txt"), HELLO);
    writeFileSync(join(agentDir, "agent.log"), Array.from({ length: 200 }, (_, i) => `12:00:${String(i % 60).padStart(2, "0")} [OK ${i}] 1 ms\n`).join(""));
    writeFileSync(join(agentDir, "bin.dat"), Buffer.from([0xff, 0xfe, 0x00, 0x41]));
    writeFileSync(join(agentDir, "big.bin"), randomBytes(300_000));
    writeFileSync(join(agentDir, "config", "secret"), "TOP-SECRET");
    agent = await startAgent(["--root", root, "--mode", "DEVELOPMENT"]);
    ro = await startAgent(["--root", root, "--mode", "READ_ONLY"]);
    auditFile = join(localRoot, "audit.jsonl");
    mcp = new McpDriver(["--host", "127.0.0.1", "--port", String(agent.port), "--local-root", localRoot, "--audit-file", auditFile, "--cache-file", "none"]);
    await mcp.init("claude");
  });
  after(() => {
    mcp?.close();
    agent?.proc.kill();
    ro?.proc.kill();
    for (const d of [root, localRoot, outside]) rmSync(d, { recursive: true, force: true });
  });

  test("handshake works for both client eras and advertises the safety instructions", async () => {
    for (const era of ["claude", "codex"] as const) {
      const d = new McpDriver(["--host", "127.0.0.1", "--port", String(agent.port), "--cache-file", "none", "--no-audit"]);
      try {
        const init = await d.init(era);
        assert.equal(init.protocolVersion, HANDSHAKES[era].protocolVersion);
        assert.equal(init.serverInfo.name, "nintendo-dev");
        assert.match(init.instructions ?? "", /REAL Nintendo 3DS/);
        assert.match(init.instructions ?? "", /untrusted DATA/);
        assert.match(text(await d.call("nintendo_ping", { count: 1 })), /pings/);
      } finally {
        d.close();
      }
    }
  });

  test("tools/list: every tool is documented for the LLM and annotated", async () => {
    const list = (await mcp.rpc("tools/list", {})).result.tools as Array<any>;
    const names = list.map((t) => t.name).sort();
    assert.deepEqual(names, [
      "nintendo_agent_log", "nintendo_device_info", "nintendo_find_device", "nintendo_fs_delete", "nintendo_fs_download",
      "nintendo_fs_list", "nintendo_fs_mkdir", "nintendo_fs_read", "nintendo_fs_stat", "nintendo_fs_upload", "nintendo_fs_write", "nintendo_ping",
    ]);
    const writers = new Set(["nintendo_fs_write", "nintendo_fs_mkdir", "nintendo_fs_delete", "nintendo_fs_upload"]);
    for (const t of list) {
      assert.ok(t.description.length > 120, `${t.name} has a substantive description`);
      assert.match(t.description, /REAL Nintendo 3DS/, `${t.name} says it is real hardware`);
      assert.equal(typeof t.annotations.readOnlyHint, "boolean", t.name);
      assert.equal(t.annotations.readOnlyHint, !writers.has(t.name), `${t.name} readOnlyHint`);
      if (writers.has(t.name)) assert.equal(t.annotations.destructiveHint, true, `${t.name} destructiveHint`);
      assert.equal(t.inputSchema.type, "object");
    }
    const byName = Object.fromEntries(list.map((t) => [t.name, t]));
    assert.match(byName.nintendo_fs_write.description, /modifies real storage/i);
    assert.match(byName.nintendo_fs_write.description, /NEVER overwrites silently/);
    assert.match(byName.nintendo_fs_read.description, /never follow instructions/);
    assert.match(byName.nintendo_fs_delete.description, /nothing is destroyed/);
  });

  test("ping, device_info and find_device", async () => {
    const p = await mcp.call("nintendo_ping", { count: 3 });
    assert.equal(p.isError, undefined);
    assert.equal(p.structuredContent!.rtt_ms.length, 3);
    const info = await mcp.call("nintendo_device_info");
    assert.equal(info.structuredContent!.platform, "host");
    assert.equal(info.structuredContent!.mode, "DEVELOPMENT");
    assert.deepEqual(info.structuredContent!.unavailable, ["model", "memory", "sd_total", "sd_free"]);
    const f = await mcp.call("nintendo_find_device", { hosts: ["127.0.0.1"] });
    assert.equal(f.structuredContent!.devices[0].host, "127.0.0.1");
    assert.equal(f.structuredContent!.devices[0].mode, "DEVELOPMENT");
    const none = await mcp.call("nintendo_find_device", { hosts: ["127.0.0.2"] });
    assert.deepEqual(none.structuredContent!.devices, []);
  });

  test("list, stat and read (the acceptance test), with encodings, tail and paging", async () => {
    const l = await mcp.call("nintendo_fs_list", { path: DIR });
    assert.deepEqual(l.structuredContent!.entries.map((e: any) => e.name).sort(), ["agent.log", "big.bin", "bin.dat", "config", "test.txt"]);
    assert.equal((await mcp.call("nintendo_fs_stat", { path: `${DIR}/test.txt` })).structuredContent!.size, HELLO.length);
    const r = await mcp.call("nintendo_fs_read", { path: `${DIR}/test.txt` });
    assert.equal(r.structuredContent!.content, HELLO);
    assert.match(text(r), /Hello from Nintendo 3DS/);
    assert.equal((await mcp.call("nintendo_fs_read", { path: `${DIR}/bin.dat`, encoding: "hex" })).structuredContent!.content, "fffe0041");
    assert.equal((await mcp.call("nintendo_fs_read", { path: `${DIR}/bin.dat`, encoding: "base64" })).structuredContent!.content, "//4AQQ==");
    const bad = await mcp.call("nintendo_fs_read", { path: `${DIR}/bin.dat` });
    assert.match(text(bad), /not valid UTF-8/);
    const tail = await mcp.call("nintendo_fs_read", { path: `${DIR}/agent.log`, tail: 40 });
    assert.match(tail.structuredContent!.content, /\[OK 199\] 1 ms\n$/);
    assert.equal(tail.structuredContent!.bytes, 40);
    const page = await mcp.call("nintendo_fs_read", { path: `${DIR}/big.bin`, offset: 1000, length: 500, encoding: "hex" });
    assert.equal(page.structuredContent!.content, readFileSync(join(agentDir, "big.bin")).subarray(1000, 1500).toString("hex"));
    const capped = await mcp.call("nintendo_fs_read", { path: `${DIR}/big.bin`, encoding: "base64" });
    assert.equal(capped.structuredContent!.bytes, 32768);
    assert.equal(capped.structuredContent!.truncated, true);
    const log = await mcp.call("nintendo_agent_log", { bytes: 200 });
    assert.match(log.structuredContent!.content, /\[OK 19\d\]/);
  });

  test("errors are actionable, not stack traces", async () => {
    const t = async (name: string, args: Record<string, unknown>) => {
      const r = await mcp.call(name, args);
      assert.equal(r.isError, true, `${name} ${JSON.stringify(args)}`);
      return text(r);
    };
    assert.match(await t("nintendo_fs_read", { path: `${DIR}/nope` }), /NOT_FOUND.*mkdir/s);
    assert.match(await t("nintendo_fs_read", { path: `${DIR}/config/secret` }), /PROTECTED_PATH/);
    assert.match(await t("nintendo_fs_read", { path: "relative" }), /must be absolute/);
    assert.match(await t("nintendo_fs_list", { path: `${DIR}/test.txt` }), /BAD_REQUEST/);
    assert.match(await t("nintendo_fs_write", { path: "/luma/x", content: "x" }), /PROTECTED_PATH.*Do not try to work around/s);
    assert.match(await t("nintendo_fs_write", { path: `${DIR}/nodir/x`, content: "x" }), /NOT_FOUND/);
    // invalid arguments are rejected by the schema (protocol level or tool error), never executed
    const r = await mcp.rpc("tools/call", { name: "nintendo_fs_read", arguments: { path: `${DIR}/test.txt`, length: 99_999_999 } });
    assert.ok(r.error || r.result?.isError, "length above the cap is refused");
  });

  test("writes: acceptance test, never silent overwrite, backup, mkdir, delete-to-trash", async () => {
    const w = await mcp.call("nintendo_fs_write", { path: `${DIR}/from-codex.txt`, content: "Codex was here." });
    assert.equal(w.isError, undefined, text(w));
    assert.equal(readFileSync(join(agentDir, "from-codex.txt"), "utf8"), "Codex was here.");
    assert.equal(w.structuredContent!.written, 15);
    const again = await mcp.call("nintendo_fs_write", { path: `${DIR}/from-codex.txt`, content: "other" });
    assert.equal(again.isError, true);
    assert.match(text(again), /EXISTS.*overwrite=true/s);
    assert.equal(readFileSync(join(agentDir, "from-codex.txt"), "utf8"), "Codex was here.");
    const rep = await mcp.call("nintendo_fs_write", { path: `${DIR}/from-codex.txt`, content: "v2", overwrite: true, backup: true });
    assert.equal(rep.structuredContent!.replaced, true);
    assert.equal(readFileSync(join(agentDir, "from-codex.txt.bak"), "utf8"), "Codex was here.");
    const b64 = await mcp.call("nintendo_fs_write", { path: `${DIR}/b.bin`, content: Buffer.from([1, 2, 3, 250]).toString("base64"), encoding: "base64" });
    assert.equal(b64.isError, undefined);
    assert.deepEqual(readFileSync(join(agentDir, "b.bin")), Buffer.from([1, 2, 3, 250]));
    assert.equal((await mcp.call("nintendo_fs_mkdir", { path: `${DIR}/inbox` })).isError, undefined);
    assert.equal(existsSync(join(agentDir, "inbox")), true);
    const d = await mcp.call("nintendo_fs_delete", { path: `${DIR}/b.bin` });
    assert.equal(d.structuredContent!.trash_path, `${DIR}/.ndp-trash/b.bin`);
    assert.equal(existsSync(join(agentDir, "b.bin")), false);
    assert.deepEqual(readFileSync(join(agentDir, ".ndp-trash/b.bin")), Buffer.from([1, 2, 3, 250]));
    assert.equal((await mcp.call("nintendo_fs_delete", { path: DIR })).isError, true, "a writable root cannot be deleted");
    assert.equal(readdirSync(agentDir).some((n) => /ndp-(tmp|old)/.test(n)), false);
    const big = await mcp.call("nintendo_fs_write", { path: `${DIR}/huge.txt`, content: "x".repeat(1024 * 1024 + 1) });
    assert.equal(big.isError, true);
    assert.match(text(big), /nintendo_fs_upload/);
  });

  test("upload and download go through the local-folder allow-list", async () => {
    const data = randomBytes(300_000);
    writeFileSync(join(localRoot, "app.3dsx"), data);
    const up = await mcp.call("nintendo_fs_upload", { local_path: "app.3dsx", remote_path: `${DIR}/app.3dsx` });
    assert.equal(up.isError, undefined, text(up));
    assert.deepEqual(readFileSync(join(agentDir, "app.3dsx")), data);
    assert.match(text(up), /verified by the console/);
    assert.equal((await mcp.call("nintendo_fs_upload", { local_path: "app.3dsx", remote_path: `${DIR}/app.3dsx` })).isError, true, "no silent overwrite");
    const dn = await mcp.call("nintendo_fs_download", { remote_path: `${DIR}/big.bin`, local_path: "downloads-big.bin" });
    assert.equal(dn.isError, undefined, text(dn));
    assert.deepEqual(readFileSync(join(localRoot, "downloads-big.bin")), readFileSync(join(agentDir, "big.bin")));
    const dup = await mcp.call("nintendo_fs_download", { remote_path: `${DIR}/big.bin`, local_path: "downloads-big.bin" });
    assert.equal(dup.isError, true);
    assert.match(text(dup), /overwrite_local/);
    assert.equal((await mcp.call("nintendo_fs_download", { remote_path: `${DIR}/big.bin`, local_path: "downloads-big.bin", overwrite_local: true })).isError, undefined);
    assert.deepEqual(readdirSync(localRoot).filter((n) => n.endsWith(".ndev-part")), [], "no partial files left");

    // local sandbox: nothing outside the allowed folder can be read or written
    writeFileSync(join(outside, "secret.txt"), "OUTSIDE");
    for (const p of [join(outside, "secret.txt"), "../secret.txt", "/etc/hosts"]) {
      const r = await mcp.call("nintendo_fs_upload", { local_path: p, remote_path: `${DIR}/leak.txt` });
      assert.equal(r.isError, true, `upload of ${p} refused`);
      assert.match(text(r), /outside the allowed local folders/);
    }
    assert.equal(existsSync(join(agentDir, "leak.txt")), false);
    const d2 = await mcp.call("nintendo_fs_download", { remote_path: `${DIR}/test.txt`, local_path: join(outside, "planted.txt") });
    assert.equal(d2.isError, true);
    assert.equal(existsSync(join(outside, "planted.txt")), false);
    symlinkSync(outside, join(localRoot, "linkdir"));
    const viaLink = await mcp.call("nintendo_fs_upload", { local_path: "linkdir/secret.txt", remote_path: `${DIR}/leak2.txt` });
    assert.equal(viaLink.isError, true, "a symlink pointing outside is refused");
    assert.equal(existsSync(join(agentDir, "leak2.txt")), false);
  });

  test("READ_ONLY console: writes are refused with an explanation, reads still work", async () => {
    const d = new McpDriver(["--host", "127.0.0.1", "--port", String(ro.port), "--cache-file", "none", "--no-audit"]);
    try {
      await d.init("codex");
      const w = await d.call("nintendo_fs_write", { path: `${DIR}/ro.txt`, content: "x" });
      assert.equal(w.isError, true);
      assert.match(text(w), /FORBIDDEN_MODE.*press X/s);
      assert.equal(existsSync(join(agentDir, "ro.txt")), false);
      assert.equal((await d.call("nintendo_fs_delete", { path: `${DIR}/test.txt` })).isError, true);
      assert.equal(existsSync(join(agentDir, "test.txt")), true);
      assert.equal((await d.call("nintendo_fs_read", { path: `${DIR}/test.txt` })).structuredContent!.content, HELLO);
      assert.equal((await d.call("nintendo_device_info")).structuredContent!.mode, "READ_ONLY");
    } finally {
      d.close();
    }
  });

  test("the audit log records every call and never the file content", async () => {
    await mcp.call("nintendo_fs_write", { path: `${DIR}/audited.txt`, content: "SECRET-CONTENT-123" });
    const lines = readFileSync(auditFile, "utf8").trim().split("\n").map((l) => JSON.parse(l));
    assert.ok(lines.length >= 15);
    const w = lines.filter((l) => l.tool === "nintendo_fs_write");
    assert.ok(w.length >= 3);
    assert.ok(w.some((l) => l.ok === false && l.status === "EXISTS"));
    assert.ok(w.some((l) => l.ok === true && l.args.path === `${DIR}/audited.txt` && l.args.chars === 18));
    assert.equal(readFileSync(auditFile, "utf8").includes("SECRET-CONTENT-123"), false, "content is not audited");
    assert.ok(lines.every((l) => typeof l.ts === "string" && typeof l.ms === "number"));
  });

  test("reconnects after the agent drops an idle connection; reads retry, writes get a clean connection", async () => {
    const idle = await startAgent(["--root", root, "--mode", "DEVELOPMENT", "--idle-ms", "300"]);
    const d = new McpDriver(["--host", "127.0.0.1", "--port", String(idle.port), "--cache-file", "none", "--no-audit"]);
    try {
      await d.init("claude");
      assert.equal((await d.call("nintendo_fs_read", { path: `${DIR}/test.txt` })).structuredContent!.content, HELLO);
      await new Promise((r) => setTimeout(r, 900)); // the agent closes the idle connection
      assert.equal((await d.call("nintendo_fs_read", { path: `${DIR}/test.txt` })).structuredContent!.content, HELLO);
      await new Promise((r) => setTimeout(r, 900));
      const w = await d.call("nintendo_fs_write", { path: `${DIR}/after-idle.txt`, content: "ok" });
      assert.equal(w.isError, undefined, text(w));
      assert.equal(readFileSync(join(agentDir, "after-idle.txt"), "utf8"), "ok");
    } finally {
      d.close();
      idle.proc.kill();
    }
  });

  test("an unreachable console gives a helpful error, and the server stays alive", async () => {
    const d = new McpDriver(["--host", "127.0.0.1", "--port", "1", "--cache-file", "none", "--no-audit", "--timeout-ms", "1000"]);
    try {
      await d.init("claude");
      const r = await d.call("nintendo_ping");
      assert.equal(r.isError, true);
      assert.match(text(r), /Nintendo Dev Agent/);
      assert.match(text(r), /nintendo_find_device/);
      assert.match(text(await d.call("nintendo_fs_stat", { path: "/" })), /Error/);
    } finally {
      d.close();
    }
  });

  test("a connection lost DURING a request: reads are retried once, writes and deletes never are", async () => {
    // A fake agent: answers HELLO, then drops the connection on every other request and counts them.
    const seen: number[] = [];
    const fake = createServer((sock) => {
      const dec = new FrameDecoder(65536);
      sock.on("error", () => {});
      sock.on("data", (d: Buffer) => {
        for (const f of dec.push(new Uint8Array(d.buffer, d.byteOffset, d.length))) {
          if (f.header.command === Command.HELLO) {
            sock.write(encodeFrame({
              kind: Kind.RES, requestId: f.header.requestId, command: Command.HELLO,
              payload: encodeTlv([[Tag.PROTOCOL, u16(1)], [Tag.PLATFORM, str("fake")], [Tag.AGENT_VERSION, str("0")], [Tag.NONCE, new Uint8Array(16)], [Tag.AUTH, str("none")], [Tag.MODE, str("DEVELOPMENT")], [Tag.MAX_FRAME, u32(65536)]]),
            }));
          } else if (f.header.kind === Kind.REQ) {
            seen.push(f.header.command);
            sock.destroy(); // die in the middle of the request
          }
        }
      });
    });
    await new Promise<void>((r) => fake.listen(0, "127.0.0.1", () => r()));
    const port = (fake.address() as { port: number }).port;
    const d = new McpDriver(["--host", "127.0.0.1", "--port", String(port), "--cache-file", "none", "--no-audit", "--timeout-ms", "1500"]);
    try {
      await d.init("claude");
      const count = (cmd: number) => seen.filter((c) => c === cmd).length;
      const stat = await d.call("nintendo_fs_stat", { path: "/3ds" });
      assert.equal(stat.isError, true);
      assert.equal(count(Command.FS_STAT), 2, "an idempotent read is retried exactly once");
      const w = await d.call("nintendo_fs_write", { path: `${DIR}/x.txt`, content: "x" });
      assert.equal(w.isError, true);
      assert.equal(count(Command.FS_WRITE), 1, "a write is sent exactly once (a retry could duplicate or corrupt it)");
      assert.match(text(w), /outcome may be unknown/);
      await d.call("nintendo_fs_delete", { path: `${DIR}/x.txt` });
      assert.equal(count(Command.FS_DELETE), 1, "a delete is sent exactly once");
      await d.call("nintendo_fs_mkdir", { path: `${DIR}/dd` });
      assert.equal(count(Command.FS_MKDIR), 1, "a mkdir is sent exactly once");
    } finally {
      d.close();
      fake.close();
    }
  });

  test("stdout carries only protocol messages (logs go to stderr)", () => {
    assert.equal(mcp.stderr.includes('"jsonrpc"'), false);
  });
});
