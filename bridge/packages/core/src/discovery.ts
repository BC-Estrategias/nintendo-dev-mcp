import { connect } from "node:net";
import { networkInterfaces } from "node:os";
import { DEFAULT_PORT } from "./constants.ts";
import { NdpClient, type HelloInfo } from "./client.ts";

export interface FoundDevice {
  host: string;
  port: number;
  agent: HelloInfo;
}

/** "a.b.c" prefixes of the /24 networks this machine is on (private IPv4 only, no loopback). */
export function localSubnets(): string[] {
  const out = new Set<string>();
  for (const list of Object.values(networkInterfaces())) {
    for (const i of list ?? []) {
      if (i.family !== "IPv4" || i.internal) continue;
      const [a, b, c] = i.address.split(".");
      out.add(`${a}.${b}.${c}`);
    }
  }
  return [...out];
}

/** Expands "192.168.15.0/24", "192.168.15" or a single IP into host addresses. */
export function expandHosts(spec: string): string[] {
  const cidr = /^(\d+\.\d+\.\d+)\.\d+\/24$/.exec(spec) ?? /^(\d+\.\d+\.\d+)\.?$/.exec(spec);
  if (cidr) return Array.from({ length: 254 }, (_, i) => `${cidr[1]}.${i + 1}`);
  return [spec];
}

function portOpen(host: string, port: number, timeoutMs: number): Promise<boolean> {
  return new Promise((resolve) => {
    const s = connect({ host, port });
    const done = (ok: boolean) => {
      s.destroy();
      resolve(ok);
    };
    s.setTimeout(timeoutMs, () => done(false));
    s.once("connect", () => done(true));
    s.once("error", () => done(false));
  });
}

/**
 * Looks for NDP agents: a TCP probe of `port` on every candidate host, then a real HELLO to
 * confirm it is our agent (something else may listen on the port). The console's DHCP address
 * changes, so this replaces guessing IPs.
 */
export async function discover(opts: { port?: number; hosts?: string[]; timeoutMs?: number; concurrency?: number } = {}): Promise<FoundDevice[]> {
  const port = opts.port ?? DEFAULT_PORT;
  const timeoutMs = opts.timeoutMs ?? 500;
  const candidates = opts.hosts ?? localSubnets().flatMap((p) => expandHosts(p));
  const open: string[] = [];
  let next = 0;
  const workers = Array.from({ length: Math.min(opts.concurrency ?? 64, Math.max(1, candidates.length)) }, async () => {
    for (;;) {
      const i = next++;
      if (i >= candidates.length) return;
      const host = candidates[i] as string;
      if (await portOpen(host, port, timeoutMs)) open.push(host);
    }
  });
  await Promise.all(workers);

  const found: FoundDevice[] = [];
  for (const host of open) {
    try {
      const c = await NdpClient.connect({ host, port, connectTimeoutMs: 2000, attempts: 1, requestTimeoutMs: 2000 });
      try {
        found.push({ host, port, agent: await c.hello() });
      } finally {
        c.close();
      }
    } catch {
      /* not an NDP agent */
    }
  }
  return found.sort((a, b) => a.host.localeCompare(b.host, undefined, { numeric: true }));
}
