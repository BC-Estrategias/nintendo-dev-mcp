import { createHmac } from "node:crypto";
import { FLAGS_KNOWN, Flag, HEADER_SIZE, MAC_SIZE, MAGIC, PROTOCOL_VERSION, Status } from "./constants.ts";
import { NdpProtocolError } from "./errors.ts";

export interface Header {
  version: number;
  kind: number;
  flags: number;
  requestId: number;
  command: number;
  status: number;
  payloadLength: number;
}

export interface Frame {
  header: Header;
  payload: Uint8Array;
  /** 16-byte MAC when `flags & Flag.MAC`, else null. */
  mac: Uint8Array | null;
}

export interface FrameInit {
  kind: number;
  requestId: number;
  command: number;
  status?: number;
  flags?: number;
  version?: number;
  payload?: Uint8Array;
  /** When given, `Flag.MAC` is set and these 16 bytes are appended. */
  mac?: Uint8Array;
}

export function encodeHeader(h: Header): Uint8Array {
  const out = new Uint8Array(HEADER_SIZE);
  const dv = new DataView(out.buffer);
  out.set(MAGIC, 0);
  out[4] = h.version;
  out[5] = h.kind;
  dv.setUint16(6, h.flags, true);
  dv.setUint32(8, h.requestId, true);
  dv.setUint16(12, h.command, true);
  dv.setUint16(14, h.status, true);
  dv.setUint32(16, h.payloadLength, true);
  return out;
}

/** Parses and validates (magic, flags) a 20-byte header. */
export function decodeHeader(b: Uint8Array): Header {
  if (b.length < HEADER_SIZE) throw new NdpProtocolError(Status.BAD_FRAME, "short header");
  for (let i = 0; i < 4; i++) if (b[i] !== MAGIC[i]) throw new NdpProtocolError(Status.BAD_FRAME, "bad magic");
  const dv = new DataView(b.buffer, b.byteOffset, HEADER_SIZE);
  const flags = dv.getUint16(6, true);
  if (flags & ~FLAGS_KNOWN) throw new NdpProtocolError(Status.BAD_FRAME, `unknown flags 0x${flags.toString(16)}`);
  return {
    version: b[4] as number,
    kind: b[5] as number,
    flags,
    requestId: dv.getUint32(8, true),
    command: dv.getUint16(12, true),
    status: dv.getUint16(14, true),
    payloadLength: dv.getUint32(16, true),
  };
}

export function encodeFrame(init: FrameInit): Uint8Array {
  const payload = init.payload ?? new Uint8Array(0);
  const mac = init.mac;
  if (mac && mac.length !== MAC_SIZE) throw new RangeError("MAC must be 16 bytes");
  const flags = (init.flags ?? 0) | (mac ? Flag.MAC : 0);
  const header = encodeHeader({
    version: init.version ?? PROTOCOL_VERSION,
    kind: init.kind,
    flags,
    requestId: init.requestId,
    command: init.command,
    status: init.status ?? 0,
    payloadLength: payload.length,
  });
  const out = new Uint8Array(HEADER_SIZE + payload.length + (mac ? MAC_SIZE : 0));
  out.set(header, 0);
  out.set(payload, HEADER_SIZE);
  if (mac) out.set(mac, HEADER_SIZE + payload.length);
  return out;
}

/** mac = HMAC-SHA256(key, u64le(counter) || header || payload)[0:16]  (spec §4). */
export function frameMac(key: Uint8Array, counter: bigint, header: Uint8Array, payload: Uint8Array): Uint8Array {
  const ctr = new Uint8Array(8);
  new DataView(ctr.buffer).setBigUint64(0, counter, true);
  const h = createHmac("sha256", key);
  h.update(ctr);
  h.update(header.subarray(0, HEADER_SIZE));
  h.update(payload);
  return new Uint8Array(h.digest().subarray(0, MAC_SIZE));
}

/**
 * Streaming decoder (spec §11). `push` returns every complete frame contained in the bytes fed so far.
 * Errors (bad magic/flags, oversized payload) throw NdpProtocolError; the stream is then unusable.
 */
export class FrameDecoder {
  readonly maxPayload: number;
  #buf: Uint8Array = new Uint8Array(0);
  #failed: NdpProtocolError | null = null;

  constructor(maxPayload: number) {
    this.maxPayload = maxPayload;
  }

  push(chunk: Uint8Array): Frame[] {
    if (this.#failed) throw this.#failed;
    if (chunk.length) {
      const merged = new Uint8Array(this.#buf.length + chunk.length);
      merged.set(this.#buf, 0);
      merged.set(chunk, this.#buf.length);
      this.#buf = merged;
    }
    const frames: Frame[] = [];
    try {
      for (;;) {
        if (this.#buf.length < HEADER_SIZE) break;
        const header = decodeHeader(this.#buf);
        if (header.payloadLength > this.maxPayload)
          throw new NdpProtocolError(Status.TOO_LARGE, `payload ${header.payloadLength} > max ${this.maxPayload}`);
        const macLen = header.flags & Flag.MAC ? MAC_SIZE : 0;
        const total = HEADER_SIZE + header.payloadLength + macLen;
        if (this.#buf.length < total) break;
        frames.push({
          header,
          payload: this.#buf.slice(HEADER_SIZE, HEADER_SIZE + header.payloadLength),
          mac: macLen ? this.#buf.slice(HEADER_SIZE + header.payloadLength, total) : null,
        });
        this.#buf = this.#buf.subarray(total);
      }
    } catch (e) {
      if (e instanceof NdpProtocolError) this.#failed = e;
      throw e;
    }
    return frames;
  }
}
