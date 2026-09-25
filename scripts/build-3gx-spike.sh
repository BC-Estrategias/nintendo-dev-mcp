#!/usr/bin/env bash
# Builds the throwaway 3GX network spike against the (untracked) third_party CTRPluginFramework template.
# Usage: NDEV_SPIKE_HOST=192,168,15,16 ./scripts/build-3gx-spike.sh
set -euo pipefail
cd "$(dirname "$0")/.."
: "${NDEV_SPIKE_HOST:?set NDEV_SPIKE_HOST to this computer IPv4 as four comma-separated bytes, e.g. 192,168,15,16}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
export PATH="$PWD/third_party/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
T=third_party/CTRPluginFramework-BlankTemplate
[ -d "$T" ] || { echo "missing $T: git clone https://github.com/Nanquitas/CTRPluginFramework-BlankTemplate into third_party/"; exit 1; }
command -v 3gxtool >/dev/null || { echo "missing third_party/bin/3gxtool"; exit 1; }
B=build-3gx-spike
rm -rf "$B"
mkdir "$B"
cp -R "$T/Includes" "$T/Lib" "$T/3ds.ld" "$T/Makefile" "$T/Sources" "$B/"
rm -f "$B/Sources/main.cpp" "$B/Sources/zz_shim.c"
cp spike/3gx-net/Sources/main.cpp "$B/Sources/"
cp spike/3gx-net/CTRPluginFramework.plgInfo "$B/"
# the 2019 CTRPluginFramework library still references the old libsysbase table that current devkitARM dropped
printf '/* stub for the 2019 CTRPF library */\nstruct { void *slot[128]; } __syscalls;\n' > "$B/Sources/zz_shim.c"
sed -i.bak "s|-DARM11 -D_3DS|-DARM11 -D_3DS -DNDEV_SPIKE_HOST=${NDEV_SPIKE_HOST}|" "$B/Makefile"
make -C "$B" 2>&1 | grep -E "error|Error|undefined|creating" || true
OUT="$(ls "$B"/*.3gx 2>/dev/null | head -1)"
[ -n "$OUT" ] || { echo "build failed"; exit 1; }
mkdir -p dist
cp "$OUT" dist/ndev-spike-v0.0.1.3gx
shasum -a 256 dist/ndev-spike-v0.0.1.3gx
echo "built: dist/ndev-spike-v0.0.1.3gx"
