#!/usr/bin/env bash
# Full local verification: vectors are up to date, C builds (-Werror, ASan/UBSan) and passes,
# TypeScript typechecks and passes (including integration against the C host agent).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "==> vectors up to date?"
python3 docs/protocol/test-vectors/generate.py
if [ -d .git ] && ! git diff --quiet -- docs/protocol/test-vectors agent/common/tests/vectors_gen.h; then
  echo "vectors changed after regeneration — commit the regenerated files" >&2
  git --no-pager diff --stat -- docs/protocol/test-vectors agent/common/tests/vectors_gen.h >&2
  exit 1
fi

echo "==> C core + host agent"
cmake -S agent -B build/agent -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build/agent
./build/agent/common/test_ndp

echo "==> TypeScript bridge"
(cd bridge && npm install --no-audit --no-fund >/dev/null && npx tsc -p tsconfig.json --noEmit && npm test)

echo "==> all checks passed"
