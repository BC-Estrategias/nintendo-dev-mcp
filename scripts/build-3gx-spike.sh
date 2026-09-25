#!/usr/bin/env bash
# Builds the throwaway 3GX network spike against the (untracked) third_party CTRPluginFramework template.
# Usage: NDEV_SPIKE_HOST=192,168,15,16 ./scripts/build-3gx-spike.sh
set -euo pipefail
cd "$(dirname "$0")/.."
: "${NDEV_SPIKE_HOST:?set NDEV_SPIKE_HOST to this computer IPv4 as four comma-separated bytes, e.g. 192,168,15,16}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
export PATH="$PWD/third_party/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
T=third_party/pk7/CTRPluginFramework-BlankTemplate
[ -d "$T" ] || { echo "missing $T: see docs/research/3gx-toolchain.md"; exit 1; }
command -v 3gxtool >/dev/null || { echo "missing third_party/bin/3gxtool"; exit 1; }
B=build-3gx-spike
rm -rf "$B"
mkdir "$B"
cp -R "$T/Includes" "$T/3gx.ld" "$T/Makefile" "$T/Sources" "$B/"
rm -f "$B/Sources/main.cpp"
cp spike/3gx-net/Sources/main.cpp "$B/Sources/"
cp spike/3gx-net/CTRPluginFramework.plgInfo "$B/"
sed -i.bak "s|-D__3DS__|-D__3DS__ -DNDEV_SPIKE_HOST=${NDEV_SPIKE_HOST}|" "$B/Makefile"
make -C "$B" CTRPFLIB="$PWD/third_party/libctrpf" 2>&1 | grep -E "error|Error|undefined|creating" || true
OUT="$(ls "$B"/*.3gx 2>/dev/null | head -1)"
[ -n "$OUT" ] || { echo "build failed"; exit 1; }
mkdir -p dist
cp "$OUT" dist/ndev-spike-v0.0.4.3gx
shasum -a 256 dist/ndev-spike-v0.0.4.3gx
echo "built: dist/ndev-spike-v0.0.4.3gx"
