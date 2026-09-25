import { createHash, randomBytes } from "node:crypto";
import { createReadStream } from "node:fs";
import { stat as statLocal } from "node:fs/promises";
import { connect as tcpConnect, type Socket } from "node:net";
import { performance } from "node:perf_hooks";
import {
  Command, DEFAULT_CHUNK, DEFAULT_MAX_FRAME, DEFAULT_PORT, Flag, FsType, Kind, MIN_CHUNK, PROTOCOL_VERSION, Status, Tag,
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

export interface WriteOptions {
  /** Replace an existing file (default false: fails with EXISTS). */
  overwrite?: boolean;
  /** With overwrite: keep the previous version as `<name>.bak`. */
  backup?: boolean;
  chunk?: number;
  onProgress?: (bytes: number, total: number) => void;
}

export interface WriteResult {
  written: number;
  sha256: Uint8Array;
  replaced: boolean;
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

  /** Writes with backpressure. Never waits on a dead socket: a close/error while waiting rejects. */
  async #writeFrame(bytes: Uint8Array): Promise<void> {
    if (this.#failure) throw this.#failure;
    if (this.#socket.destroyed) throw new NdpTransportError("connection closed");
    if (this.#socket.write(bytes)) return;
    await new Promise<void>((resolve, reject) => {
      const done = (err?: Error) => {
        this.#socket.off("drain", onDrain);
        this.#socket.off("close", onClose);
        this.#socket.off("error", onError);
        if (err) reject(err);
        else resolve();
      };
      const onDrain = () => done();
      const onClose = () => done(this.#failure ?? new NdpTransportError("connection closed"));
      const onError = (e: Error) => done(this.#failure ?? new NdpTransportError(`socket error: ${e.message}`));
      this.#socket.once("drain", onDrain);
      this.#socket.once("close", onClose);
      this.#socket.once("error", onError);
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
  async read(path: string, opts: ReadOptions, onChunk: (chunk: Uint8Array) => void | Promise<void>): Promise<ReadResult> {
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

  async mkdir(path: string): Promise<void> {
    await this.request(Command.FS_MKDIR, encodeTlv([[Tag.PATH, str(normalizePath(path))]]));
  }

  /**
   * Uploads `size` bytes produced by `chunks` to `path` (temp file + verify + rename on the device).
   * `sha256` (the digest of the whole content) is sent up front so the device can refuse a corrupt
   * transfer before touching anything; the digest it computes is checked again here.
   */
  async write(
    path: string,
    source: { size: number; sha256: Uint8Array; chunks: AsyncIterable<Uint8Array> | Iterable<Uint8Array> },
    opts: WriteOptions = {},
  ): Promise<WriteResult> {
    const fields: TlvField[] = [
      [Tag.PATH, str(normalizePath(path))],
      [Tag.SIZE, u64(BigInt(source.size))],
      [Tag.SHA256, source.sha256],
    ];
    if (opts.overwrite) fields.push([Tag.OVERWRITE, u8(1)]);
    if (opts.backup) fields.push([Tag.BACKUP, u8(1)]);
    const payload = encodeTlv(fields);

    return this.#exclusive(async () => {
      const t0 = performance.now();
      const id = this.#sendRequest(Command.FS_WRITE, payload);
      const ready = parseTlv((await this.#awaitResponse(id)).payload); // ERR here: nothing was created
      const maxChunk = Math.min(ready.u32(Tag.MAX_CHUNK) ?? DEFAULT_CHUNK, DEFAULT_MAX_FRAME);
      const chunkSize = Math.max(MIN_CHUNK, Math.min(opts.chunk ?? DEFAULT_CHUNK, maxChunk));
      let sent = 0;

      const early = (): NdpRemoteError | undefined => {
        // the agent aborted (I/O error, ...): stop sending instead of streaming into the void
        const i = this.#frames.findIndex((f) => f.header.requestId === id && f.header.kind === Kind.ERR);
        return i >= 0 ? this.#remoteError(this.#frames.splice(i, 1)[0] as Frame) : undefined;
      };
      const send = async (bytes: Uint8Array) => {
        const frame = encodeFrame({
          kind: Kind.DATA, requestId: id, command: Command.FS_WRITE, payload: bytes,
          flags: sent + bytes.length < source.size ? Flag.MORE : 0,
        });
        await this.#writeFrame(frame);
        sent += bytes.length;
        opts.onProgress?.(sent, source.size);
      };

      for await (const block of source.chunks) {
        for (let o = 0; o < block.length; o += chunkSize) {
          const err = early();
          if (err) throw err;
          await send(block.subarray(o, Math.min(block.length, o + chunkSize)));
        }
      }
      if (sent !== source.size) {
        this.close(); // we promised `size` bytes and cannot deliver them: the stream is unusable
        throw new NdpProtocolError(Status.BAD_REQUEST, `source produced ${sent} bytes, declared ${source.size}`);
      }
      const err = early();
      if (err) throw err;
      await this.#writeFrame(encodeFrame({ kind: Kind.END, requestId: id, command: Command.FS_WRITE }));

      const res = parseTlv((await this.#awaitResponse(id)).payload);
      const written = res.u64(Tag.WRITTEN);
      const digest = res.first(Tag.SHA256);
      if (written === undefined || !digest || digest.length !== 32)
        throw new NdpProtocolError(Status.BAD_REQUEST, "malformed FS_WRITE response");
      if (Buffer.compare(digest, source.sha256) !== 0 || Number(written) !== source.size)
        throw new NdpProtocolError(Status.HASH_MISMATCH, "the device reports different content than was sent");
      return { written: Number(written), sha256: new Uint8Array(digest), replaced: res.u8(Tag.REPLACED) === 1, ms: performance.now() - t0 };
    });
  }

  writeBytes(path: string, data: Uint8Array, opts: WriteOptions = {}): Promise<WriteResult> {
    const sha256 = new Uint8Array(createHash("sha256").update(data).digest());
    return this.write(path, { size: data.length, sha256, chunks: [data] }, opts);
  }

  /** Uploads a local file (hashed first so the device can verify it before committing). */
  async writeFile(path: string, localPath: string, opts: WriteOptions = {}): Promise<WriteResult> {
    const size = (await statLocal(localPath)).size;
    const hash = createHash("sha256");
    for await (const chunk of createReadStream(localPath)) hash.update(chunk as Buffer);
    const sha256 = new Uint8Array(hash.digest());
    return this.write(path, { size, sha256, chunks: createReadStream(localPath, { highWaterMark: 64 * 1024 }) as AsyncIterable<Uint8Array> }, opts);
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
