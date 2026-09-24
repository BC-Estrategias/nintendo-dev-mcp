#!/usr/bin/env bash
# Builds the 3DS agent (.3dsx + .smdh) with devkitPro and prints its checksum.
# Needs devkitARM + libctru + 3dstools (pacman group 3ds-dev).
set -euo pipefail
cd "$(dirname "$0")/.."

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
if [ ! -d "$DEVKITARM" ]; then
  echo "devkitARM not found at $DEVKITARM — install devkitPro (https://devkitpro.org/wiki/Getting_Started)" >&2
  exit 1
fi

make -C agent/3ds clean >/dev/null
make -C agent/3ds

mkdir -p dist
cp agent/3ds/nintendo-dev-agent.3dsx agent/3ds/nintendo-dev-agent.smdh dist/
( cd dist && shasum -a 256 nintendo-dev-agent.3dsx nintendo-dev-agent.smdh | tee SHA256SUMS )
echo "built: dist/nintendo-dev-agent.3dsx ($(wc -c < dist/nintendo-dev-agent.3dsx) bytes)"
