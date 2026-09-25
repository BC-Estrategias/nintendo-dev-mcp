#!/usr/bin/env node
import { DEFAULT_PORT, NdpClient, NdpRemoteError, NdpTransportError } from "@ndev/core";

const USAGE = `ndev — Nintendo Dev Bridge (CLI)

Usage:
  ndev hello <host[:port]>            connect and print the agent's HELLO
  ndev ping  <host[:port]> [-c N] [-i MS]
                                      HELLO + N pings (default 4), MS ms between pings (default 0)

The default port is ${DEFAULT_PORT}.`;

function parseTarget(s: string): { host: string; port: number } {
  const i = s.lastIndexOf(":");
  if (i > 0 && !s.includes("]")) {
    const port = Number(s.slice(i + 1));
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error(`invalid port in "${s}"`);
    return { host: s.slice(0, i), port };
  }
  return { host: s, port: DEFAULT_PORT };
}

async function main(argv: string[]): Promise<number> {
  const [cmd, target, ...rest] = argv;
  if (!cmd || cmd === "-h" || cmd === "--help") {
    console.log(USAGE);
    return cmd ? 0 : 2;
  }
  if ((cmd !== "hello" && cmd !== "ping") || !target) {
    console.error(USAGE);
    return 2;
  }
  let count = 4;
  let intervalMs = 0;
  for (let i = 0; i < rest.length; i += 2) {
    const flag = rest[i];
    const n = Number(rest[i + 1]);
    if (flag === "-c" && Number.isInteger(n) && n >= 1 && n <= 1000) count = n;
    else if (flag === "-i" && Number.isInteger(n) && n >= 0 && n <= 60000) intervalMs = n;
    else {
      console.error("options: -c N (1..1000), -i MS (0..60000)");
      return 2;
    }
  }
  const { host, port } = parseTarget(target);
  const client = await NdpClient.connect({ host, port });
  try {
    const info = await client.hello();
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
  } finally {
    client.close();
  }
}

main(process.argv.slice(2)).then(
  (code) => process.exit(code),
  (e: unknown) => {
    if (e instanceof NdpTransportError || e instanceof NdpRemoteError || e instanceof Error) console.error(`error: ${e.message}`);
    else console.error(`error: ${String(e)}`);
    process.exit(1);
  },
);
