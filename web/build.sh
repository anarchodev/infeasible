#!/bin/sh
# Build the infeasible core to a self-contained WASM ES module (DESIGN.md §12).
#
# Emscripten is a dev-time toolchain only — the shipped artifact is the .wasm
# (below the durability line, §12) plus its C source; the build step is not.
# SINGLE_FILE embeds the wasm as base64 in the .mjs so there is one vendored
# container with nothing fetched at runtime (§12 rot-rule #1).
#
# Usage:  web/build.sh        (from the repo root)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
# CommonJS module (MODULARIZE, no EXPORT_ES6): emscripten 3.1.6's ES6 output
# references __dirname, which is undefined under Node ESM. The CJS module loads
# cleanly in Node (via createRequire in the .mjs host) and, once packaged for
# the browser, the WEB path never touches __dirname. Revisit to a native ESM
# module with a newer emcc when the browser build lands (M2).
out="$root/web/infeasible.cjs"

emcc -O2 -I"$root/src" \
    "$root/src/core/arena.c" \
    "$root/src/core/intern.c" \
    "$root/src/logic/dl.c" \
    "$root/src/logic/dl_col.c" \
    "$root/src/state/world.c" \
    "$root/src/lang/lexer.c" \
    "$root/src/lang/story.c" \
    "$root/web/exports.c" \
    -sMODULARIZE=1 -sEXPORT_NAME=createInfeasible \
    -sSINGLE_FILE=1 -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString \
    -sEXPORTED_FUNCTIONS=_inf_compile,_inf_free,_inf_intern,_inf_name,_inf_query,_inf_get,_inf_set,_inf_step,_inf_step1,_inf_last_err,_inf_last_diag,_inf_why,_malloc,_free \
    -o "$out"

echo "built $out"
