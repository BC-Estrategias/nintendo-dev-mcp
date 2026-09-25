#!/usr/bin/env python3
"""Extracts exheader.bin (0x800) and code.bin (the uncompressed .code) from an unencrypted CXI made by makerom."""
import struct, sys

cxi, out = sys.argv[1], sys.argv[2]
b = open(cxi, "rb").read()
assert b[0x100:0x104] == b"NCCH", "not an NCCH/CXI"
flags = b[0x188:0x190]
assert flags[7] & 4, "NoCrypto flag not set: the CXI is encrypted, Luma needs it decrypted"
exh_size = struct.unpack_from("<I", b, 0x180)[0]
assert exh_size in (0x400, 0x800), exh_size
exheader = b[0x200:0x200 + 0x800]
exefs_off = struct.unpack_from("<I", b, 0x1A0)[0] * 0x200
exefs_size = struct.unpack_from("<I", b, 0x1A4)[0] * 0x200
exefs = b[exefs_off:exefs_off + exefs_size]
code = None
for i in range(10):
    name, off, size = struct.unpack_from("<8sII", exefs, i * 16)
    if name.rstrip(b"\0") == b".code":
        code = exefs[0x200 + off:0x200 + off + size]
assert code is not None, ".code not found in ExeFS"
# exheader: SCI codeset info (name at 0, flags at 0xD: bit0 = compressed)
assert not (exheader[0xD] & 1), "the code is marked compressed; set EnableCompress: false"
text_pages = struct.unpack_from("<I", exheader, 0x14)[0]
print("exheader name:", exheader[:8].rstrip(b"\0"), "text size", hex(text_pages), "ro", hex(struct.unpack_from("<I", exheader, 0x24)[0]),
      "data", hex(struct.unpack_from("<I", exheader, 0x34)[0]), "bss", hex(struct.unpack_from("<I", exheader, 0x3C)[0]))
open(out + "/exheader.bin", "wb").write(exheader)
open(out + "/code.bin", "wb").write(code)
print("exheader.bin", len(exheader), "bytes; code.bin", len(code), "bytes")
