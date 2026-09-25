#!/usr/bin/env bash
# Builds dist/nintendo-dev-agent-vX.Y.Z.cia (a CIA you install with FBI to get a Home Menu icon).
# Needs third_party/bin/makerom and third_party/bin/bannertool (see docs/research/cia-toolchain.md).
set -euo pipefail
cd "$(dirname "$0")/.."
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$PWD/third_party/bin:$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
command -v makerom >/dev/null || { echo "missing third_party/bin/makerom"; exit 1; }
command -v bannertool >/dev/null || { echo "missing third_party/bin/bannertool"; exit 1; }

VERSION="$(sed -n 's/^#define NDP_AGENT_VERSION "\(.*\)"$/\1/p' agent/common/include/ndp/ndp_defs.h)"
IFS=. read -r MAJOR MINOR MICRO <<<"$VERSION"
# The installed CIA version. This makerom build drops the "minor" bits of -ver, so the agent's minor number goes in the
# TMD "major" slot (0.6.3 -> 6.0.3): still strictly increasing, which is all FBI needs to update in place.

# the agent itself (also refreshes agent/3ds/nintendo-dev-agent.elf and .smdh)
./scripts/build-3ds.sh >/dev/null

W=build-cia
rm -rf "$W"
mkdir -p "$W" dist
python3 agent/3ds/cia/make_banner_assets.py "$W"
bannertool makebanner -i "$W/banner.png" -a "$W/banner.wav" -o "$W/banner.bnr" >/dev/null
OUT="nintendo-dev-agent-v${VERSION}.cia"
rm -f dist/nintendo-dev-agent*.cia
makerom -f cia -o "dist/$OUT" -elf agent/3ds/nintendo-dev-agent.elf -rsf agent/3ds/cia/nintendo-dev-agent.rsf \
  -icon agent/3ds/nintendo-dev-agent.smdh -banner "$W/banner.bnr" -ver "$(( (MINOR << 10) | MICRO ))" \
  -target t 2>&1 | grep -E "ERROR|error|Warning" || true
[ -s "dist/$OUT" ] || { echo "makerom failed"; exit 1; }
( cd dist && shasum -a 256 "$OUT" | tee -a SHA256SUMS )
echo "built: dist/$OUT ($(wc -c < "dist/$OUT") bytes), title id 000400000BD00100"
