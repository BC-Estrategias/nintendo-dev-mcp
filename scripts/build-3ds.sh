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

VERSION="$(sed -n 's/^#define NDP_AGENT_VERSION "\(.*\)"$/\1/p' agent/common/include/ndp/ndp_defs.h)"
[ -n "$VERSION" ] || { echo "could not read NDP_AGENT_VERSION" >&2; exit 1; }

make -C agent/3ds clean >/dev/null
make -C agent/3ds

# The version is part of the file name so different builds are never confused in Finder or on the SD.
mkdir -p dist
rm -f dist/nintendo-dev-agent*.3dsx dist/nintendo-dev-agent*.smdh
OUT="nintendo-dev-agent-v${VERSION}"
cp agent/3ds/nintendo-dev-agent.3dsx "dist/${OUT}.3dsx"
cp agent/3ds/nintendo-dev-agent.smdh "dist/${OUT}.smdh"
( cd dist && shasum -a 256 "${OUT}.3dsx" "${OUT}.smdh" | tee SHA256SUMS )
echo "built: dist/${OUT}.3dsx ($(wc -c < "dist/${OUT}.3dsx") bytes)"
