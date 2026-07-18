#!/usr/bin/env bash
# Ensure a repo-local, version-pinned Emscripten SDK in .emsdk/ (gitignored).
#
# Idempotent: clones + installs only when missing or when the pin changed, so
# the first WASM build is slow (~320MB) and every build after is instant. The
# version is PINNED, not `latest`: compile output must be reproducible across
# machines (DESIGN §12 — two peers grounding the same source get identical
# WASM), which a floating toolchain breaks. Bump EMSDK_VERSION to upgrade.
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-6.0.3}"
here="$(cd "$(dirname "$0")/.." && pwd)"
emsdk_dir="$here/.emsdk"
stamp="$emsdk_dir/.pinned-version"

if [ -x "$emsdk_dir/upstream/emscripten/emcc" ] && [ "$(cat "$stamp" 2>/dev/null || true)" = "$EMSDK_VERSION" ]; then
  echo "emsdk $EMSDK_VERSION ready at $emsdk_dir"
  exit 0
fi

if [ ! -d "$emsdk_dir/.git" ]; then
  echo "cloning emsdk -> $emsdk_dir"
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$emsdk_dir"
fi

echo "installing emscripten $EMSDK_VERSION (first run only; ~320MB download)"
"$emsdk_dir/emsdk" install "$EMSDK_VERSION"
"$emsdk_dir/emsdk" activate "$EMSDK_VERSION"
echo "$EMSDK_VERSION" > "$stamp"
echo "emsdk $EMSDK_VERSION installed at $emsdk_dir"
