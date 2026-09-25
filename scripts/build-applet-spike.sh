#!/usr/bin/env bash
# Builds exheader.bin + code.bin for the Game Notes replacement spike (see docs/research/applet-feasibility.md).
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$PWD/third_party/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
B=build-applet
rm -rf "$B"
mkdir -p "$B" dist
arm-none-eabi-gcc -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -O2 -Wall -Wextra -mword-relocations \
  -ffunction-sections -std=gnu99 -DARM11 -D__3DS__ -I"$DEVKITPRO/libctru/include" -specs=3dsx.specs -g \
  spike/applet/main.c -o "$B/applet.elf" -L"$DEVKITPRO/libctru/lib" -lctru -lm
makerom -f cxi -o "$B/applet.cxi" -elf "$B/applet.elf" -rsf spike/applet/applet.rsf -target t 2>&1 | grep -E "ERROR|error" || true
[ -s "$B/applet.cxi" ] || { echo "makerom failed"; exit 1; }
python3 spike/applet/extract_cxi.py "$B/applet.cxi" "$B"
mkdir -p dist/applet-spike
cp "$B/exheader.bin" "$B/code.bin" dist/applet-spike/
( cd dist/applet-spike && shasum -a 256 exheader.bin code.bin )
