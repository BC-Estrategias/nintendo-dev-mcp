#!/usr/bin/env node
import { serveStdio } from "@modelcontextprotocol/server/stdio";
import { createServer, type ServerOptions } from "./server.ts";

const USAGE = `ndev-mcp — MCP server (stdio) for a Nintendo 3DS running the Nintendo Dev Agent

Options:
  --host <ip|auto>        console address (default: $NDEV_HOST, then the last known one, then a LAN scan)
  --port <n>              agent port (default 6464)
  --local-root <dir>      local folder the tools may read/write (repeatable; default: the working directory)
  --audit-file <file>     JSON-lines audit log (default ~/.config/nintendo-dev/audit.log)
  --no-audit              disable the audit log
  --cache-file <file|none>  where the last known console address is remembered (default ~/.config/nintendo-dev/last-host.json)
  --timeout-ms <n>        per-request timeout (default 5000)
`;

const opts: ServerOptions = { localRoots: [] };
const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const a = args[i] as string;
  const val = () => {
    const v = args[++i];
    if (v === undefined) throw new Error(`${a} expects a value`);
    return v;
  };
  if (a === "--host") opts.host = val();
  else if (a === "--port") opts.port = Number(val());
  else if (a === "--local-root") opts.localRoots!.push(val());
  else if (a === "--audit-file") opts.auditFile = val();
  else if (a === "--no-audit") opts.auditFile = null;
  else if (a === "--timeout-ms") opts.requestTimeoutMs = Number(val());
  else if (a === "--cache-file") {
    const v = val();
    opts.cacheFile = v === "none" ? null : v;
  }
  else if (a === "-h" || a === "--help") {
    process.stderr.write(USAGE);
    process.exit(0);
  } else {
    process.stderr.write(`unknown option ${a}\n\n${USAGE}`);
    process.exit(2);
  }
}

// stdout carries the protocol: everything human-readable goes to stderr.
const handle = serveStdio(() => createServer(opts).server);
const stop = () => void handle.close().finally(() => process.exit(0));
process.on("SIGINT", stop);
process.on("SIGTERM", stop);
