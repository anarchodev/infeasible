# web — the browser client

The infeasible logic core (C) compiled to WASM, a `.story` world, a generated
typed binding, the frozen presentation interface, and a cart: the whole M2
client (DESIGN.md §4.2, §11 M2, §12). A game here is `.story` source plus host
JS against `world_*` — no engine code, and no build step to run or remix.

## Files

- `exports.c` — the hand-written JS↔WASM shim: a flat, primitive-only surface
  over `world_*` (atom ids and strings cross the boundary — no `dl_lit` structs
  or `FILE*`). Compile a source, intern names, query, get/set, step, `why?`.
- `gen_binding.mjs` → `*.binding.mjs` — the §6.3 typed host binding, generated
  from the compiler's own interface artifact (below).
- `platform/` — the frozen presentation interface (§12): `spec.mjs` is the
  freeze itself, `platform.mjs` assembles the four cart-facing surfaces,
  `canvas2d.mjs` and `headless.mjs` are the two backends, `runtime.mjs` is the
  tick/frame loop, `font.mjs` the built-in glyphs.
- `carts/cellar.mjs` + `examples/cellar_play.story` — the playable cellar.
- `index.html` — the page that puts all of it on a canvas.
- `host.mjs`, `loop_driver.mjs` — Node drivers over the data boundary alone.
- `build.sh` — `emcc` → a single-file, self-contained module (the `.wasm` is
  embedded base64, so nothing is fetched at runtime — §12 rot-rule #1).

## Build & run

Requires Emscripten (`emcc`) and Node.

```sh
web/build.sh                                  # emits web/infeasible.cjs
node web/gen_binding.mjs examples/cellar_play.story    # the typed binding
node web/platform_check.mjs                   # the interface + the cart, headless
node web/host.mjs                             # the data boundary alone
node web/binding_check.mjs                    # generated from reaction5e.story
```

To play it, serve the **repo root** (the page fetches the `.story` source —
source is always shipped, §12) and open `/web/`:

```sh
python3 -m http.server 8000
xdg-open http://localhost:8000/web/
```

## The presentation interface (§12)

`platform/spec.mjs` is the freeze, as data: twelve draw ops, four audio ops,
five input ops, two persistence ops, the blessed 1080-divisor resolution set,
the sixteen-entry palette, the frozen key set, the 4×6 text cell, and the
letterbox arithmetic (with its inverse, which belongs below the line — every
cart computing it independently is every cart getting the edges wrong).

`platform_check.mjs` asserts the correspondence in both directions: the
cart-facing surfaces expose exactly those names, and each backend implements
exactly those names. That check is what makes "frozen" a property of the code.
It then plays the real cart on the real world through the polled input surface,
from the locked door to the antidote, asserting on the recorded ops — the
headless backend makes a frame a list of ops, so "what did the cart draw?" has
a data answer.

The cart is worth reading for how little it decides. It does not know when the
door may be forced (`q.can_force_door(who)`); it does not compute what a click
changed (the step's changeset); it does not explain a greyed-out button (it
prints `why?`). There is no second copy of the rules to drift from.

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

The binding also carries the reactive channel (§11 M2): `w.subscribe(term)`
registers interest in a literal — base fact or judgment, same call — `w.verdict(h)`
is the level, and `w.edges()` is what flipped in the last step, each entry
carrying `rose`/`fell` alongside the from/to verdicts. The literal those three
take is spelled by `w.lit.*` — generated from the artifact's own ground-atom
grammar, including the `fluent=value` form of a multi-valued fact — so
`subscribe`, `why` and `receipt` stay inside the typed surface instead of being
the one place a host still types an atom by hand.

## Known rough edges (M2 to smooth)

- The module is **CommonJS**, loaded from the ESM host via `createRequire`:
  emscripten 3.1.6's `EXPORT_ES6` output references `__dirname`, undefined under
  Node ESM. A native ESM module (newer emcc, or a thin loader) lands with the
  browser build; the browser path never touches `__dirname`.
- Audio ships no assets: `sound(id)` looks `id` up in the backend's registry
  and is a no-op until the page registers one. The op set is frozen; the asset
  format is still open (§13).
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
