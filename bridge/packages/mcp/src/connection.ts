import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join } from "node:path";
import { DEFAULT_PORT, KeyStore, NdpClient, NdpRemoteError, NdpTransportError, discover, type FoundDevice } from "@ndev/core";


export interface ConnectionOptions {
  /** Explicit IP/host; "auto" or undefined = env NDEV_HOST, then the last known device, then a LAN scan. */
  host?: string | undefined;
  port?: number | undefined;
  requestTimeoutMs?: number | undefined;
  /** Where the last working host is remembered (null = do not persist). */
  cacheFile?: string | null | undefined;
  /** Pairing keys (default: ~/.config/nintendo-dev/keys.json, or NDEV_KEYS_FILE). */
  keys?: KeyStore | undefined;
}

export class DeviceNotFoundError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "DeviceNotFoundError";
  }
}

const DEFAULT_CACHE = join(homedir(), ".config", "nintendo-dev", "last-host.json");

/** One lazily opened, automatically re-opened connection to the console. */
export class DeviceConnection {
  readonly #opts: ConnectionOptions;
  #client: NdpClient | null = null;
  #host: string | null = null;
  readonly #keys: KeyStore;

  constructor(opts: ConnectionOptions = {}) {
    this.#opts = opts;
    this.#keys = opts.keys ?? new KeyStore();
  }

  get port(): number {
    return this.#opts.port ?? DEFAULT_PORT;
  }
  get host(): string | null {
    return this.#host;
  }

  close(): void {
    this.#client?.close();
    this.#client = null;
  }

  #readCache(): { host: string; port: number } | null {
    const file = this.#opts.cacheFile === undefined ? DEFAULT_CACHE : this.#opts.cacheFile;
    if (!file) return null;
    try {
      const j = JSON.parse(readFileSync(file, "utf8")) as { host?: string; port?: number };
      return j.host ? { host: j.host, port: j.port ?? DEFAULT_PORT } : null;
    } catch {
      return null;
    }
  }

  #writeCache(host: string): void {
    const file = this.#opts.cacheFile === undefined ? DEFAULT_CACHE : this.#opts.cacheFile;
    if (!file) return;
    try {
      mkdirSync(dirname(file), { recursive: true });
      writeFileSync(file, JSON.stringify({ host, port: this.port, at: new Date().toISOString() }));
    } catch {
      /* best effort */
    }
  }

  async #tryConnect(host: string, attempts: number): Promise<NdpClient> {
    const { client } = await NdpClient.open(
      { host, port: this.port, attempts, ...(this.#opts.requestTimeoutMs ? { requestTimeoutMs: this.#opts.requestTimeoutMs } : {}) },
      this.#keys,
    );
    return client;
  }

  /** Finds the console: explicit host, env, last known host, then a LAN scan (DHCP changes the IP). */
  async #locate(): Promise<NdpClient> {
    const explicit = this.#opts.host && this.#opts.host !== "auto" ? this.#opts.host : process.env.NDEV_HOST;
    if (explicit) {
      const c = await this.#tryConnect(explicit, 3);
      this.#host = explicit;
      return c;
    }
    const cached = this.#readCache();
    if (cached) {
      try {
        const c = await this.#tryConnect(cached.host, 1);
        this.#host = cached.host;
        return c;
      } catch (e) {
        if (e instanceof NdpRemoteError) throw e; // reachable but refusing (not paired / key rejected): scanning will not help
        /* the address changed: scan */
      }
    }
    const found = await discover({ port: this.port });
    if (found.length === 0)
      throw new DeviceNotFoundError(
        `no Nintendo Dev agent found on this network (port ${this.port}). Open the "Nintendo Dev Agent" app on the console (same Wi-Fi as this computer) or set the host explicitly.`,
      );
    if (found.length > 1)
      throw new DeviceNotFoundError(`several agents found (${found.map((f) => f.host).join(", ")}); set the host explicitly.`);
    const host = (found[0] as FoundDevice).host;
    const c = await this.#tryConnect(host, 2);
    this.#host = host;
    this.#writeCache(host);
    return c;
  }

  async get(): Promise<NdpClient> {
    if (this.#client && !this.#client.isClosed) return this.#client;
    this.#client?.close();
    this.#client = await this.#locate(); // only a discovered address is remembered (see #locate)
    return this.#client;
  }

  /**
   * Runs `fn` on a live connection. Safe (idempotent) operations are retried once on a transport
   * failure; non-idempotent ones (writes, deletes) never are: their outcome would be ambiguous.
   */
  async run<T>(fn: (c: NdpClient) => Promise<T>, opts: { idempotent: boolean }): Promise<T> {
    let client = await this.get();
    try {
      return await fn(client);
    } catch (e) {
      if (!opts.idempotent || !(e instanceof NdpTransportError)) throw e;
      this.close();
      client = await this.get();
      return await fn(client);
    }
  }
}
