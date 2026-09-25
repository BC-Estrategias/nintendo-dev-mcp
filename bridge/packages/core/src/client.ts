import { randomBytes } from "node:crypto";
import { connect as tcpConnect, type Socket } from "node:net";
import { performance } from "node:perf_hooks";
import { Command, DEFAULT_MAX_FRAME, DEFAULT_PORT, Kind, PROTOCOL_VERSION, Tag, type Mode, MODES } from "./constants.ts";
import { NdpProtocolError, NdpRemoteError, NdpTransportError } from "./errors.ts";
import { encodeFrame, FrameDecoder, type Frame } from "./frame.ts";
import { encodeTlv, parseTlv, str, u16, u64 } from "./tlv.ts";

export interface ConnectOptions {
  host: string;
  port?: number;
  /** Per-attempt timeout. */
  connectTimeoutMs?: number;
  /** Attempts for transient failures (host unreachable / timeout). ECONNREFUSED is never retried. Default 3. */
  attempts?: number;
  requestTimeoutMs?: number;
  bridgeName?: string;
}

export interface HelloInfo {
  protocol: number;
  platform: string;
  agentVersion: string;
  deviceNonce: Uint8Array;
  auth: string;
  mode: Mode;
  maxFrame: number;
}

const TRANSIENT_CONNECT_CODES = new Set(["EHOSTUNREACH", "EHOSTDOWN", "ENETUNREACH", "ETIMEDOUT"]);

/** True for connect failures worth retrying. ECONNREFUSED (nothing listening) is deliberately not one. */
export function isTransientConnectError(code: string | undefined): boolean {
  return code !== undefined && TRANSIENT_CONNECT_CODES.has(code);
}

interface Waiter {
  resolve: (f: Frame) => void;
  reject: (e: Error) => void;
  timer: NodeJS.Timeout;
}

/** One connection, one request at a time (spec §1). */
export class NdpClient {
  readonly #socket: Socket;
  readonly #requestTimeoutMs: number;
  readonly #bridgeName: string;
  #decoder = new FrameDecoder(DEFAULT_MAX_FRAME);
  #frames: Frame[] = [];
  #waiter: Waiter | null = null;
  #failure: Error | null = null;
  #nextId = 0;
  #chain: Promise<unknown> = Promise.resolve();
  #info: HelloInfo | null = null;

  private constructor(socket: Socket, opts: ConnectOptions) {
    this.#socket = socket;
    this.#requestTimeoutMs = opts.requestTimeoutMs ?? 5000;
    this.#bridgeName = opts.bridgeName ?? "ndev/0.1.0";
    socket.on("data", (chunk) => {
      const b = typeof chunk === "string" ? Buffer.from(chunk) : chunk;
      this.#onData(new Uint8Array(b.buffer, b.byteOffset, b.length));
    });
    socket.on("error", (e) => this.#fail(new NdpTransportError(`socket error: ${e.message}`)));
    socket.on("close", () => this.#fail(new NdpTransportError("connection closed")));
  }

  /**
   * Connects, retrying transient failures with a growing pause. Measured on a real 3DS: the first
   * connection after a quiet period can fail with EHOSTUNREACH (ARP not resolved while the console's
   * Wi-Fi radio is in power-save) and succeed on the next attempt.
   */
  static async connect(opts: ConnectOptions): Promise<NdpClient> {
    const attempts = Math.max(1, opts.attempts ?? 3);
    for (let i = 1; ; i++) {
      try {
        return await NdpClient.#connectOnce(opts);
      } catch (e) {
        if (i >= attempts || !(e instanceof NdpTransportError) || !isTransientConnectError(e.code)) throw e;
        await new Promise((r) => setTimeout(r, 250 * 2 ** (i - 1)));
      }
    }
  }

  static #connectOnce(opts: ConnectOptions): Promise<NdpClient> {
    const port = opts.port ?? DEFAULT_PORT;
    const timeoutMs = opts.connectTimeoutMs ?? 5000;
    return new Promise((resolve, reject) => {
      const socket = tcpConnect({ host: opts.host, port });
      socket.setNoDelay(true);
      const timer = setTimeout(() => {
        socket.destroy();
        reject(new NdpTransportError(`connect to ${opts.host}:${port} timed out after ${timeoutMs} ms`, "ETIMEDOUT"));
      }, timeoutMs);
      socket.once("connect", () => {
        clearTimeout(timer);
        socket.removeAllListeners("error");
        resolve(new NdpClient(socket, opts));
      });
      socket.once("error", (e) => {
        clearTimeout(timer);
        reject(new NdpTransportError(`cannot connect to ${opts.host}:${port}: ${e.message}`, (e as NodeJS.ErrnoException).code));
      });
    });
  }

  get info(): HelloInfo | null {
    return this.#info;
  }

  close(): void {
    this.#socket.destroy();
  }

  #onData(chunk: Uint8Array): void {
    let frames: Frame[];
    try {
      frames = this.#decoder.push(chunk);
    } catch (e) {
      this.#fail(e instanceof Error ? e : new NdpTransportError(String(e)));
      this.#socket.destroy();
      return;
    }
    this.#frames.push(...frames);
    this.#deliver();
  }

  #deliver(): void {
    if (this.#waiter && this.#frames.length) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.resolve(this.#frames.shift() as Frame);
    }
  }

  #fail(e: Error): void {
    if (!this.#failure) this.#failure = e;
    if (this.#waiter) {
      const w = this.#waiter;
      this.#waiter = null;
      clearTimeout(w.timer);
      w.reject(this.#failure);
    }
  }

  #nextFrame(timeoutMs: number): Promise<Frame> {
    if (this.#frames.length) return Promise.resolve(this.#frames.shift() as Frame);
    if (this.#failure) return Promise.reject(this.#failure);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#waiter = null;
        reject(new NdpTransportError(`no response within ${timeoutMs} ms`));
      }, timeoutMs);
      this.#waiter = { resolve, reject, timer };
    });
  }

  /** Sends a REQ and resolves with the matching RES; an ERR frame rejects with NdpRemoteError. */
  request(command: number, payload: Uint8Array = new Uint8Array(0)): Promise<Frame> {
    const run = async (): Promise<Frame> => {
      if (this.#failure) throw this.#failure;
      const id = (this.#nextId = (this.#nextId + 1) >>> 0 || 1);
      this.#socket.write(encodeFrame({ kind: Kind.REQ, requestId: id, command, payload }));
      for (;;) {
        const f = await this.#nextFrame(this.#requestTimeoutMs);
        if (f.header.requestId !== id) continue; // stale frame from an abandoned request
        if (f.header.kind === Kind.ERR) {
          const tlv = parseTlv(f.payload);
          throw new NdpRemoteError(f.header.status, tlv.str(Tag.DETAIL), tlv.u32(Tag.OS_RESULT));
        }
        if (f.header.kind !== Kind.RES)
          throw new NdpProtocolError(f.header.status, `unexpected frame kind ${f.header.kind}`);
        return f;
      }
    };
    const result = this.#chain.then(run, run);
    this.#chain = result.catch(() => undefined);
    return result;
  }

  async hello(): Promise<HelloInfo> {
    const payload = encodeTlv([
      [Tag.PROTOCOL, u16(PROTOCOL_VERSION)],
      [Tag.PROTOCOL_MAX, u16(PROTOCOL_VERSION)],
      [Tag.BRIDGE_NAME, str(this.#bridgeName)],
      [Tag.NONCE, new Uint8Array(randomBytes(16))],
    ]);
    const res = await this.request(Command.HELLO, payload);
    const t = parseTlv(res.payload);
    const protocol = t.u16(Tag.PROTOCOL);
    const platform = t.str(Tag.PLATFORM);
    const agentVersion = t.str(Tag.AGENT_VERSION);
    const nonce = t.first(Tag.NONCE);
    const auth = t.str(Tag.AUTH);
    const mode = t.str(Tag.MODE);
    const maxFrame = t.u32(Tag.MAX_FRAME);
    if (
      protocol === undefined || platform === undefined || agentVersion === undefined || nonce === undefined ||
      nonce.length !== 16 || auth === undefined || mode === undefined || maxFrame === undefined ||
      !(MODES as readonly string[]).includes(mode)
    )
      throw new NdpProtocolError(9, "malformed HELLO response");
    this.#info = { protocol, platform, agentVersion, deviceNonce: nonce, auth, mode: mode as Mode, maxFrame };
    this.#decoder = new FrameDecoder(maxFrame);
    return this.#info;
  }

  /** Round-trip time in milliseconds; verifies the echoed nonce. */
  async ping(): Promise<number> {
    const nonce = BigInt(`0x${randomBytes(8).toString("hex")}`);
    const t0 = performance.now();
    const res = await this.request(Command.PING, encodeTlv([[Tag.PING_NONCE, u64(nonce)]]));
    const rtt = performance.now() - t0;
    if (parseTlv(res.payload).u64(Tag.PING_NONCE) !== nonce) throw new NdpProtocolError(9, "PING nonce mismatch");
    return rtt;
  }
}
