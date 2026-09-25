import { NdpProtocolError } from "./errors.ts";
import { Status } from "./constants.ts";

export type TlvField = readonly [tag: number, value: Uint8Array];

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder("utf-8", { fatal: false });

export function u16(v: number): Uint8Array {
  const b = new Uint8Array(2);
  new DataView(b.buffer).setUint16(0, v, true);
  return b;
}
export function u8(v: number): Uint8Array {
  return new Uint8Array([v]);
}
export function u32(v: number): Uint8Array {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v, true);
  return b;
}
export function u64(v: bigint): Uint8Array {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, v, true);
  return b;
}
export function str(s: string): Uint8Array {
  return textEncoder.encode(s);
}

/** Encodes `[tag u16][len u16][value]` fields in order. */
export function encodeTlv(fields: readonly TlvField[]): Uint8Array {
  let total = 0;
  for (const [, v] of fields) {
    if (v.length > 0xffff) throw new RangeError("TLV value too long");
    total += 4 + v.length;
  }
  const out = new Uint8Array(total);
  const dv = new DataView(out.buffer);
  let o = 0;
  for (const [tag, v] of fields) {
    dv.setUint16(o, tag, true);
    dv.setUint16(o + 2, v.length, true);
    out.set(v, o + 4);
    o += 4 + v.length;
  }
  return out;
}

/** Parsed TLV payload. `first` follows the spec rule: the first occurrence of a tag wins. */
export class TlvMap {
  readonly fields: ReadonlyArray<{ tag: number; value: Uint8Array }>;
  constructor(fields: Array<{ tag: number; value: Uint8Array }>) {
    this.fields = fields;
  }
  first(tag: number): Uint8Array | undefined {
    return this.fields.find((f) => f.tag === tag)?.value;
  }
  u8(tag: number): number | undefined {
    const v = this.first(tag);
    return v && v.length === 1 ? (v[0] as number) : undefined;
  }
  all(tag: number): Uint8Array[] {
    return this.fields.filter((f) => f.tag === tag).map((f) => f.value);
  }
  u16(tag: number): number | undefined {
    const v = this.first(tag);
    return v && v.length === 2 ? new DataView(v.buffer, v.byteOffset, 2).getUint16(0, true) : undefined;
  }
  u32(tag: number): number | undefined {
    const v = this.first(tag);
    return v && v.length === 4 ? new DataView(v.buffer, v.byteOffset, 4).getUint32(0, true) : undefined;
  }
  u64(tag: number): bigint | undefined {
    const v = this.first(tag);
    return v && v.length === 8 ? new DataView(v.buffer, v.byteOffset, 8).getBigUint64(0, true) : undefined;
  }
  str(tag: number): string | undefined {
    const v = this.first(tag);
    return v ? textDecoder.decode(v) : undefined;
  }
}

/** Parses a TLV payload; a length that overruns the payload or a tail shorter than 4 bytes is malformed. */
export function parseTlv(payload: Uint8Array): TlvMap {
  const dv = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  const fields: Array<{ tag: number; value: Uint8Array }> = [];
  let i = 0;
  while (i < payload.length) {
    if (payload.length - i < 4) throw new NdpProtocolError(Status.BAD_REQUEST, "truncated TLV header");
    const tag = dv.getUint16(i, true);
    const len = dv.getUint16(i + 2, true);
    if (len > payload.length - i - 4) throw new NdpProtocolError(Status.BAD_REQUEST, "TLV length overruns payload");
    fields.push({ tag, value: payload.subarray(i + 4, i + 4 + len) });
    i += 4 + len;
  }
  return new TlvMap(fields);
}
