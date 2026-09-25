import { spawn, type ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";

export const SKIP = Boolean(process.env.NDP_SKIP_INTEGRATION);

export const AGENT =
  process.env.NDP_HOST_AGENT ??
  fileURLToPath(new URL("../../../../build/agent/host/ndp-host-agent", import.meta.url));

if (!SKIP && !existsSync(AGENT)) {
  throw new Error(
    `host agent not found at ${AGENT}\nBuild it first:  cmake -S agent -B build/agent && cmake --build build/agent\n` +
      `(or set NDP_HOST_AGENT=/path/to/ndp-host-agent, or NDP_SKIP_INTEGRATION=1)`,
  );
}

export function startAgent(args: string[] = []): Promise<{ proc: ChildProcess; port: number; webPort: number | undefined; log: () => string }> {
  return new Promise((resolve, reject) => {
    const proc = spawn(AGENT, ["--port", "0", "-v", ...args], { stdio: ["ignore", "pipe", "pipe"] });
    let out = "";
    let err = "";
    proc.stderr!.on("data", (d) => (err += d));
    const timer = setTimeout(() => reject(new Error("agent did not start")), 5000);
    proc.stdout!.on("data", (d) => {
      out += d;
      const m = /LISTENING [\d.]+:(\d+)/.exec(out);
      const w = /WEB [\d.]+:(\d+)/.exec(out);
      // when a web port was requested the WEB line follows LISTENING: wait for it too
      if (m && (w || !args.includes("--web-port"))) {
        clearTimeout(timer);
        resolve({ proc, port: Number(m[1]), webPort: w ? Number(w[1]) : undefined, log: () => err });
      }
    });
    proc.on("error", reject);
  });
}
