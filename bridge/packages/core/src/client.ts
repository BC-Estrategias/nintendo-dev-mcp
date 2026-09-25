import { createHash, randomBytes } from "node:crypto";
import { connect as tcpConnect, type Socket } from "node:net";
import { performance } from "node:perf_hooks";
import {
  Command, DEFAULT_CHUNK, DEFAULT_MAX_FRAME, DEFAULT_PORT, FsType, Kind, MIN_CHUNK, PROTOCOL_VERSION, Status, Tag,
  type Mode, MODES,
} from "./constants.ts";
import { NdpProtocolError, NdpRemoteError, NdpTransportError } from "./errors.ts";
import { encodeFrame, FrameDecoder, type Frame } from "./frame.ts";
import { normalizePath } from "./path.ts";
import { encodeTlv, parseTlv, str, u8, u16, u32, u64, type TlvField } from "./tlv.ts";

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

export interface FsEntry {
  name: string;
  type: "file" | "dir";
  size: number;
}

export interface FsStat {
  type: "file" | "dir";
  size: number;
  /** Seconds since the Unix epoch, or null when the device does not know. */
  mtime: number | null;
}

export interface ReadOptions {
  offset?: number;
  /** Bytes to read (0 or undefined = to the end of the file). */
  length?: number;
  chunk?: number;
  /** Ask the agent for a SHA-256 of the bytes it sent and verify it locally (default true). */
  verify?: boolean;
  onProgress?: (bytes: number, total: number) => void;
}

export interface ReadResult {
  /** File size on the device (not the number of bytes read). */
  totalSize: number;
  bytes: number;
  sha256: Uint8Array | null;
  verified: boolean;
  ms: number;
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

  #exclusive<T>(fn: () => Promise<T>): Promise<T> {
    const result = this.#chain.then(fn, fn);
    this.#chain = result.catch(() => undefined);
    return result;
  }

  #sendRequest(command: number, payload: Uint8Array): number {
    if (this.#failure) throw this.#failure;
    const id = (this.#nextId = (this.#nextId + 1) >>> 0 || 1);
    this.#socket.write(encodeFrame({ kind: Kind.REQ, requestId: id, command, payload }));
    return id;
  }

  /** Waits for the RES that answers `id`; an ERR frame rejects with NdpRemoteError. */
  async #awaitResponse(id: number): Promise<Frame> {
    for (;;) {
      const f = await this.#nextFrame(this.#requestTimeoutMs);
      if (f.header.requestId !== id) continue; // stale frame from an abandoned request
      if (f.header.kind === Kind.ERR) throw this.#remoteError(f);
      if (f.header.kind !== Kind.RES)
        throw new NdpProtocolError(f.header.status, `unexpected frame kind ${f.header.kind}`);
      return f;
    }
  }

  #remoteError(f: Frame): NdpRemoteError {
    const tlv = parseTlv(f.payload);
    return new NdpRemoteError(f.header.status, tlv.str(Tag.DETAIL), tlv.u32(Tag.OS_RESULT));
  }

  /** Sends a REQ and resolves with the matching RES; an ERR frame rejects with NdpRemoteError. */
  request(command: number, payload: Uint8Array = new Uint8Array(0)): Promise<Frame> {
    return this.#exclusive(() => this.#awaitResponse(this.#sendRequest(command, payload)));
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

  async stat(path: string): Promise<FsStat> {
    const res = await this.request(Command.FS_STAT, encodeTlv([[Tag.PATH, str(normalizePath(path))]]));
    const t = parseTlv(res.payload);
    const type = t.u8(Tag.TYPE);
    const size = t.u64(Tag.SIZE);
    const mtime = t.u64(Tag.MTIME);
    if ((type !== FsType.FILE && type !== FsType.DIR) || size === undefined || mtime === undefined)
      throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_STAT response");
    return { type: type === FsType.DIR ? "dir" : "file", size: Number(size), mtime: mtime === 0n ? null : Number(mtime) };
  }

  /** Lists a directory, following the agent's pages. Entries are in the device's order. */
  async list(path: string): Promise<FsEntry[]> {
    const p = normalizePath(path);
    const out: FsEntry[] = [];
    const decoder = new TextDecoder("utf-8", { fatal: false });
    let cursor = 0;
    for (;;) {
      const fields: TlvField[] = [[Tag.PATH, str(p)]];
      if (cursor) fields.push([Tag.CURSOR, u32(cursor)]);
      const t = parseTlv((await this.request(Command.FS_LIST, encodeTlv(fields))).payload);
      for (const e of t.all(Tag.ENTRY)) {
        if (e.length < 10) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_LIST entry");
        const dv = new DataView(e.buffer, e.byteOffset, e.byteLength);
        out.push({
          type: e[0] === FsType.DIR ? "dir" : "file",
          size: Number(dv.getBigUint64(1, true)),
          name: decoder.decode(e.subarray(9)),
        });
      }
      const more = t.u8(Tag.LIST_MORE);
      const next = t.u32(Tag.NEXT_CURSOR);
      if (more === undefined || next === undefined) throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_LIST response");
      if (!more) return out;
      if (next <= cursor) throw new NdpProtocolError(Status.BAD_REQUEST, "FS_LIST cursor did not advance");
      cursor = next;
    }
  }

  /**
   * Streams a file (or a range) from the device, calling `onChunk` for each DATA frame. Holds the
   * connection for the whole transfer. If anything fails after the RES, the connection is closed
   * (the remaining frames of the stream cannot be skipped safely).
   */
  read(path: string, opts: ReadOptions, onChunk: (chunk: Uint8Array) => void | Promise<void>): Promise<ReadResult> {
    const verify = opts.verify ?? true;
    const fields: TlvField[] = [[Tag.PATH, str(normalizePath(path))]];
    if (opts.offset) fields.push([Tag.OFFSET, u64(BigInt(opts.offset))]);
    if (opts.length) fields.push([Tag.LENGTH, u64(BigInt(opts.length))]);
    fields.push([Tag.CHUNK, u32(Math.max(MIN_CHUNK, opts.chunk ?? DEFAULT_CHUNK))]);
    if (verify) fields.push([Tag.WANT_HASH, u8(1)]);
    const payload = encodeTlv(fields);

    return this.#exclusive(async () => {
      const t0 = performance.now();
      const id = this.#sendRequest(Command.FS_READ, payload);
      const res = parseTlv((await this.#awaitResponse(id)).payload);
      const totalSize = res.u64(Tag.TOTAL_SIZE);
      const willSend = res.u64(Tag.WILL_SEND);
      if (totalSize === undefined || willSend === undefined) {
        this.close();
        throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_READ response");
      }
      const hash = verify ? createHash("sha256") : null;
      let bytes = 0;
      try {
        for (;;) {
          const f = await this.#nextFrame(this.#requestTimeoutMs);
          if (f.header.requestId !== id) continue;
          if (f.header.kind === Kind.DATA) {
            bytes += f.payload.length;
            hash?.update(f.payload);
            await onChunk(f.payload);
            opts.onProgress?.(bytes, Number(willSend));
          } else if (f.header.kind === Kind.END) {
            if (BigInt(bytes) !== willSend)
              throw new NdpProtocolError(Status.IO_ERROR, `received ${bytes} bytes, expected ${willSend}`);
            const remote = verify ? parseTlv(f.payload).first(Tag.SHA256) : undefined;
            const local = hash ? new Uint8Array(hash.digest()) : null;
            if (verify) {
              if (!remote || !local || Buffer.compare(remote, local) !== 0)
                throw new NdpProtocolError(Status.HASH_MISMATCH, "SHA-256 mismatch between agent and bridge");
            }
            return { totalSize: Number(totalSize), bytes, sha256: local, verified: verify, ms: performance.now() - t0 };
          } else if (f.header.kind === Kind.ERR) {
            throw this.#remoteError(f);
          } else {
            throw new NdpProtocolError(Status.BAD_FRAME, `unexpected frame kind ${f.header.kind} during transfer`);
          }
        }
      } catch (e) {
        // An ERR frame ends the stream in an orderly way; anything else leaves it in an unknown state.
        if (!(e instanceof NdpRemoteError)) this.close();
        throw e;
      }
    });
  }

  /** Reads into memory. `maxBytes` bounds the request itself (the agent never sends more). */
  async readBytes(path: string, opts: ReadOptions & { maxBytes?: number } = {}): Promise<ReadResult & { data: Uint8Array; truncated: boolean }> {
    const maxBytes = opts.maxBytes ?? 4 * 1024 * 1024;
    const chunks: Uint8Array[] = [];
    const length = opts.length ? Math.min(opts.length, maxBytes) : maxBytes;
    const r = await this.read(path, { ...opts, length }, (c) => void chunks.push(c.slice()));
    const data = new Uint8Array(r.bytes);
    let o = 0;
    for (const c of chunks) {
      data.set(c, o);
      o += c.length;
    }
    return { ...r, data, truncated: (opts.offset ?? 0) + r.bytes < r.totalSize };
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
