#!/bin/sh
# Build the infeasible core to a self-contained WASM ES module (DESIGN.md §12).
#
# Emscripten is a dev-time toolchain only — the shipped artifact is the .wasm
# (below the durability line, §12) plus its C source; the build step is not.
# SINGLE_FILE embeds the wasm as base64 in the .mjs so there is one vendored
# container with nothing fetched at runtime (§12 rot-rule #1).
#
# Usage:  web/build.sh        (from anywhere)
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)

# Bootstrap the repo-local pinned emsdk if emcc is not already on PATH — the
# same toolchain scripts/build_wasm.sh uses, so the two builds cannot drift
# onto different compilers. Nothing system-wide is installed.
if ! command -v emcc >/dev/null 2>&1; then
    "$root/scripts/bootstrap_emsdk.sh"
    # shellcheck disable=SC1091
    . "$root/.emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
# CommonJS module (MODULARIZE, no EXPORT_ES6): emscripten 3.1.6's ES6 output
# references __dirname, which is undefined under Node ESM. The CJS module loads
# cleanly in Node (via createRequire in the .mjs host) and the browser's classic
# <script> path never touches __dirname. Revisit to a native ESM module with a
# newer emcc.
#
# The extension is `.js` and that is load-bearing, not cosmetic. The browser
# loads this as a CLASSIC script, and a static host that serves an unknown
# extension as application/octet-stream alongside `X-Content-Type-Options:
# nosniff` — GitHub Pages does exactly this — makes the browser REFUSE to
# execute it. `.js` is the one extension every host maps to a JavaScript MIME
# type. Node needs no help either way: with no package.json declaring
# `"type": "module"` above this directory, `.js` is CommonJS, which is what
# `createRequire` here expects.
out="$root/web/infeasible.js"

emcc -O2 -I"$root/src" \
    "$root/src/core/arena.c" \
    "$root/src/core/intern.c" \
    "$root/src/logic/dl.c" \
    "$root/src/logic/dl_col.c" \
    "$root/src/logic/dl_graph.c" \
    "$root/src/logic/dl_trace.c" \
    "$root/src/state/world.c" \
    "$root/src/state/factindex.c" \
    "$root/src/lang/lexer.c" \
    "$root/src/lang/story.c" \
    "$root/web/exports.c" \
    -sMODULARIZE=1 -sEXPORT_NAME=createInfeasible \
    -sSINGLE_FILE=1 -sALLOW_MEMORY_GROWTH=1 \
    -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString,HEAP32,HEAPU32 \
    -sEXPORTED_FUNCTIONS=_inf_compile,_inf_free,_inf_intern,_inf_name,_inf_query,_inf_get,_inf_set,_inf_get_num,_inf_set_num,_inf_interface,_inf_step,_inf_step1,_inf_emit_count,_inf_emits,_inf_bool_deltas,_inf_num_deltas,_inf_num_receipt,_inf_actions,_inf_action_status,_inf_action_blockers,_inf_subscribe,_inf_unsubscribe,_inf_sub_verdict,_inf_sub_edges,_inf_last_err,_inf_last_diag,_inf_why,_malloc,_free \
    -o "$out"

echo "built $out"
