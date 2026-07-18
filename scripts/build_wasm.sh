#!/usr/bin/env bash
# Build the core + wasm shim to an ES module for the Node/browser loop driver.
# Self-contained: bootstraps a repo-local pinned emsdk on first run, then uses
# it. No system emsdk needed. No raylib, no display.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/build-wasm"
mkdir -p "$out"

# ensure + activate the repo-local toolchain
"$here/scripts/bootstrap_emsdk.sh"
# shellcheck disable=SC1091
source "$here/.emsdk/emsdk_env.sh" 2>/dev/null

emcc -O2 -I"$here/src" \
  "$here/src/core/arena.c" \
  "$here/src/core/intern.c" \
  "$here/src/logic/dl.c" \
  "$here/src/state/world.c" \
  "$here/src/wasm/bindings.c" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sENVIRONMENT=node,web \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU32,HEAP32,UTF8ToString \
  -o "$out/infeasible.mjs"

echo "built $out/infeasible.mjs (+ .wasm)"
