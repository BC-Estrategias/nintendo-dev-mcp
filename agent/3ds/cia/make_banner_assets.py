#!/usr/bin/env python3
"""Writes the CIA's banner image (256x128 PNG) and a short silent WAV, with no third-party libraries."""
import struct, sys, wave, zlib

def png(path, w, h, pixel):
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

# 5x7 pixel font for the few letters we need
FONT = {
    "N": ["10001", "11001", "10101", "10101", "10011", "10001", "10001"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
}

def banner(x, y):
    # dark blue gradient with the letters NDEV in white and a thin cyan line
    base = (12 + y // 6, 22 + y // 5, 60 + y // 3)
    text, scale, ox, oy = "NDEV", 9, 34, 30
    cx = (x - ox) // (6 * scale)
    if 0 <= cx < len(text) and (x - ox) % (6 * scale) < 5 * scale:
        gx, gy = ((x - ox) % (6 * scale)) // scale, (y - oy) // scale
        if 0 <= gy < 7 and FONT[text[cx]][gy][gx] == "1":
            return (245, 245, 250)
    if 104 <= y <= 106:
        return (60, 200, 230)
    return base

if __name__ == "__main__":
    out = sys.argv[1]
    png(out + "/banner.png", 256, 128, banner)
    with wave.open(out + "/banner.wav", "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
        w.writeframes(b"\x00\x00" * (22050 // 2))
