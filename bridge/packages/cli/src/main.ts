#!/usr/bin/env node
import { createWriteStream } from "node:fs";
import { once } from "node:events";
import { DEFAULT_PORT, NdpClient, NdpRemoteError, NdpTransportError, PathInvalidError } from "@ndev/core";

const USAGE = `ndev — Nintendo Dev Bridge (CLI)

Usage:
  ndev hello <host[:port]>                        connect and print the agent's HELLO
  ndev ping  <host[:port]> [-c N] [-i MS]         HELLO + N pings (default 4), MS ms between pings
  ndev ls    <host[:port]> <path>                 list a directory on the device's SD card
  ndev stat  <host[:port]> <path>                 type, size and mtime
  ndev cat   <host[:port]> <path> [--tail N] [--max N] [--offset N]
                                                  write the file (or a range) to stdout
  ndev get   <host[:port]> <remote> <local> [--no-verify]
                                                  download a file; verifies SHA-256 unless --no-verify

Paths are absolute on the SD card ("/3ds/nintendo-dev-agent/agent.log"). The default port is ${DEFAULT_PORT}.`;

function parseTarget(s: string): { host: string; port: number } {
  const i = s.lastIndexOf(":");
  if (i > 0 && !s.includes("]")) {
    const port = Number(s.slice(i + 1));
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error(`invalid port in "${s}"`);
    return { host: s.slice(0, i), port };
  }
  return { host: s, port: DEFAULT_PORT };
}

function human(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}

/** Parses `--flag value` / `--switch` pairs; unknown flags are an error. */
function parseFlags(args: string[], valued: string[], switches: string[] = []): { flags: Map<string, number>; on: Set<string>; rest: string[] } {
  const flags = new Map<string, number>();
  const on = new Set<string>();
  const rest: string[] = [];
  for (let i = 0; i < args.length; i++) {
    const a = args[i] as string;
    if (valued.includes(a)) {
      const n = Number(args[++i]);
      if (!Number.isInteger(n) || n < 0) throw new Error(`${a} expects a non-negative integer`);
      flags.set(a, n);
    } else if (switches.includes(a)) on.add(a);
    else if (a.startsWith("-") && a.length > 1) throw new Error(`unknown option ${a}`);
    else rest.push(a);
  }
  return { flags, on, rest };
}

async function withClient<T>(target: string, fn: (c: NdpClient, host: string, port: number) => Promise<T>): Promise<T> {
  const { host, port } = parseTarget(target);
  const client = await NdpClient.connect({ host, port });
  try {
    await client.hello();
    return await fn(client, host, port);
  } finally {
    client.close();
  }
}

async function main(argv: string[]): Promise<number> {
  const [cmd, target, ...rest] = argv;
  if (!cmd || cmd === "-h" || cmd === "--help") {
    console.log(USAGE);
    return cmd ? 0 : 2;
  }
  if (!["hello", "ping", "ls", "stat", "cat", "get"].includes(cmd) || !target) {
    console.error(USAGE);
    return 2;
  }

  if (cmd === "hello" || cmd === "ping") {
    const { flags } = parseFlags(rest, ["-c", "-i"]);
    const count = flags.get("-c") ?? 4;
    const intervalMs = flags.get("-i") ?? 0;
    if (count < 1 || count > 1000) throw new Error("-c expects an integer between 1 and 1000");
    return withClient(target, async (client, host, port) => {
      const info = client.info!;
      console.log(`Connected to ${host}:${port}`);
      console.log(
        `Agent: ${info.platform}  v${info.agentVersion}  protocol ${info.protocol}  mode ${info.mode}  ` +
          `auth ${info.auth}  max_frame ${info.maxFrame}`,
      );
      if (cmd === "ping") {
        const rtts: number[] = [];
        for (let seq = 1; seq <= count; seq++) {
          const rtt = await client.ping();
          rtts.push(rtt);
          console.log(`PONG seq=${seq} time=${rtt.toFixed(1)} ms`);
          if (intervalMs && seq < count) await new Promise((r) => setTimeout(r, intervalMs));
        }
        const avg = rtts.reduce((a, b) => a + b, 0) / rtts.length;
        console.log(`min/avg/max = ${Math.min(...rtts).toFixed(1)}/${avg.toFixed(1)}/${Math.max(...rtts).toFixed(1)} ms`);
      }
      return 0;
    });
  }

  if (cmd === "ls" || cmd === "stat") {
    const { rest: pos } = parseFlags(rest, []);
    const path = pos[0];
    if (!path) throw new Error(`${cmd} needs a <path>`);
    return withClient(target, async (client) => {
      if (cmd === "stat") {
        const st = await client.stat(path);
        console.log(`${path}\n  type  ${st.type}\n  size  ${st.size} bytes (${human(st.size)})\n  mtime ${st.mtime === null ? "unknown" : new Date(st.mtime * 1000).toISOString()}`);
        return 0;
      }
      const entries = await client.list(path);
      entries.sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name) : a.type === "dir" ? -1 : 1));
      for (const e of entries) console.log(`${e.type === "dir" ? "d" : "-"} ${String(e.size).padStart(10)}  ${e.name}${e.type === "dir" ? "/" : ""}`);
      console.error(`${entries.length} entries`);
      return 0;
    });
  }

  if (cmd === "cat") {
    const { flags, rest: pos } = parseFlags(rest, ["--tail", "--max", "--offset"]);
    const path = pos[0];
    if (!path) throw new Error("cat needs a <path>");
    return withClient(target, async (client) => {
      let offset = flags.get("--offset") ?? 0;
      const tail = flags.get("--tail");
      if (tail !== undefined) {
        const size = (await client.stat(path)).size;
        offset = Math.max(0, size - tail);
      }
      const r = await client.readBytes(path, { offset, maxBytes: flags.get("--max") ?? 8 * 1024 * 1024 });
      process.stdout.write(r.data);
      if (r.truncated) console.error(`\n[truncated: read ${r.bytes} of ${r.totalSize} bytes; use --max/--offset/--tail or 'get']`);
      return 0;
    });
  }

  // get
  const { on, rest: pos } = parseFlags(rest, [], ["--no-verify"]);
  const [remote, local] = pos;
  if (!remote || !local) throw new Error("get needs <remote> <local>");
  return withClient(target, async (client) => {
    const out = createWriteStream(local);
    let failed: Error | undefined;
    out.on("error", (e) => (failed = e));
    try {
      const r = await client.read(remote, { verify: !on.has("--no-verify") }, async (chunk) => {
        if (failed) throw failed;
        if (!out.write(chunk)) await once(out, "drain");
      });
      out.end();
      await once(out, "close");
      const kib = r.ms > 0 ? (r.bytes / 1024 / (r.ms / 1000)).toFixed(0) : "-";
      console.log(`${remote} -> ${local}: ${r.bytes} bytes in ${r.ms.toFixed(0)} ms (${kib} KiB/s)`);
      console.log(r.verified && r.sha256 ? `sha256 ${Buffer.from(r.sha256).toString("hex")} (verified against the agent)` : "not verified");
      return 0;
    } catch (e) {
      out.destroy();
      throw e;
    }
  });
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (e: unknown) => {
    if (e instanceof NdpRemoteError) console.error(`error: ${e.statusName}${e.detail ? ` — ${e.detail}` : ""}`);
    else if (e instanceof NdpTransportError || e instanceof PathInvalidError || e instanceof Error) console.error(`error: ${e.message}`);
    else console.error(`error: ${String(e)}`);
    process.exit(1);
  },
);
