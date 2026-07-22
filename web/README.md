# web — WASM core + JS host

The infeasible logic core (C) compiled to WASM, driven from JavaScript. This is
the M2 boundary spike (DESIGN.md §4.2, §12): it proves a game loop can compile a
`.story`, query judgments, propose actions, step, and read `why?` traces
entirely from JS against `world_*` — **no codegen** yet. The typed JS binding
(§6.3) will sit on top of exactly this seam.

## Files

- `exports.c` — the hand-written JS↔WASM shim: a flat, primitive-only surface
  over `world_*` (atom ids and strings cross the boundary — no `dl_lit` structs
  or `FILE*`). Compile a source, intern names, query, get/set, step, `why?`.
- `host.mjs` — a plain-JS Node host driving `examples/cellar_ground.story`:
  interns ground-atom names by hand (as the C golden tests do), prints the
  two-actor grounding, dumps a `why?` trace, and steps `force_door(guard)`.
- `build.sh` — `emcc` → a single-file, self-contained module (the `.wasm` is
  embedded base64, so nothing is fetched at runtime — §12 rot-rule #1).

## Build & run

Requires Emscripten (`emcc`) and Node.

```sh
web/build.sh          # emits web/infeasible.cjs (~108 KB, git-ignored)
node web/host.mjs
```

Expected output ends with `boundary OK: .story in, judgments/why/step out — all
from JS.`

## Known rough edges (M2 to smooth)

- The module is **CommonJS**, loaded from the ESM host via `createRequire`:
  emscripten 3.1.6's `EXPORT_ES6` output references `__dirname`, undefined under
  Node ESM. A native ESM module (newer emcc, or a thin loader) lands with the
  browser build; the browser path never touches `__dirname`.
- No renderer — this is the data boundary only. Canvas2D presentation (§12) is
  separate M2 work over the same `world_subscribe` delta seam.
- `inf_step` marshals an action array through the WASM heap; `inf_step1` is the
  single-action convenience the cellar uses.
