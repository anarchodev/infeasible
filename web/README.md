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
node web/gen_binding.mjs examples/cellar_ground.story   # the typed binding
node web/host.mjs
node web/binding_check.mjs   # after generating from examples/reaction5e.story
```

Expected output ends with `boundary OK: .story in, judgments/why/step out — all
from JS.`

## The typed binding (§6.3)

`gen_binding.mjs` compiles a `.story`, reads the **interface artifact** the
compiler emits — the declared vocabulary plus, crucially, how a ground atom is
spelled — and writes a plain ES module of typed helpers over the WASM exports.

That closes the host-side version of the failure the orphan pass exists to
kill: `query(intern("can_force_dor(guard)"))` interns a fresh, always-false
atom and answers refuted forever. Through the binding the same mistake is a
missing property (`w.q.can_force_dor` is `undefined`), a bad entity is a
`TypeError` where it is written, and an unknown action has no constructor at
all. The generated module carries a hash of the source it was generated from
and refuses a story it does not match, so a stale binding is loud rather than
silently always-false.

Types are JSDoc — `tsc --checkJs` and any editor consume them with **no build
step**, and the module that type-checks is exactly the module that ships (§12).
The generated files are committed: they are source-shaped, not build output.

Action sets are assembled through a builder rather than passed as arrays, which
is where §6.3 puts the host-protocol checks: `exclusive` groups (#159) are
enforced at `add`, naming the order already in the set, so a host collecting
orders from remote clients rejects one order instead of losing the tick.
`binding_check.mjs` pins that behaviour against `examples/reaction5e.story`.

## Known rough edges (M2 to smooth)

- The module is **CommonJS**, loaded from the ESM host via `createRequire`:
  emscripten 3.1.6's `EXPORT_ES6` output references `__dirname`, undefined under
  Node ESM. A native ESM module (newer emcc, or a thin loader) lands with the
  browser build; the browser path never touches `__dirname`.
- No renderer — this is the data boundary only. Canvas2D presentation (§12) is
  separate M2 work over the same `world_subscribe` delta seam.
- `inf_step` marshals an action array through the WASM heap; `inf_step1` is the
  single-action convenience the cellar uses.
- The step log (#88) marshals into a caller-provided `long` buffer and returns
  the size the FULL answer needs, so a short buffer is a grow-and-retry rather
  than a silent truncation: `inf_bool_deltas` / `inf_num_deltas` (the
  changeset) and `inf_num_receipt` (a header, then one variable-length row per
  contribution). The `wf_*` shim carries the same three.
- Burst cues (#11, §12) cross the boundary the way the delta will:
  `inf_emit_count` + `inf_emits` hand back the step's emission buffer as a
  pointer into WASM memory, read as a zero-copy
  `new Uint32Array(M.HEAPU32.buffer, ptr, n)` view and resolved to names with
  `inf_name` — one crossing per step, never one per event. `web/loop_driver.mjs`
  drives the same channel over the `wf_*` shim (`wf_declare_emit`,
  `wf_emit_count`, `wf_emits`).
