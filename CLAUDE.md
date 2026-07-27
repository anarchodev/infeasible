# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`infeasible` is a narrative game engine in C where **the world is a logic database**. One non-monotonic logic (propositional *defeasible logic*) serves as the single semantics for both *what is true* (stats, judgments) and *what happens next* (state transitions via defeasible inertia). Worlds are authored in a `.story` surface language that grounds to a vocabulary-checked C world; a Canvas2D web client (WASM engine + JS host) handles presentation.

**A narrative/dialogue layer is out of scope** (DESIGN.md §2). Games are built as host code against the `world_*` surface (the typed JS/C binding of §6.3). Do not add narrative concepts — knots, choices, diverts, a dialogue VM — to the design or code; a narrative front end, if ever built, is a client above the `world_*` surface, not part of the engine.

**Read `DESIGN.md` before non-trivial work** — it is the source of truth for semantics, invariants, and the milestone plan (§11). Current state: the logic engine, step function, and golden tests are in place; the **M1 `.story` compiler** (`src/lang/`: lexer → recursive-descent parse → build-time grounder) is live and growing slice by slice (typed vars over declared sorts, multi-valued and numeric fluents, providers, ramifications, set-quantified effect binders). The `examples/*.story` files compile and are exercised by tests. Presentation is **M2**: a WASM build of the core (`scripts/build_wasm.sh`) driven from JS (`web/`) — a data-boundary spike today, Canvas2D rendering still to come.

## Build & test

```sh
# Core + tests — no display needed. This is the whole native build (no renderer).
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure

# A single test target:
ctest --test-dir build -R test_dl --output-on-failure
./build/test_dl          # or run the binary directly; it prints "test_dl: all passed"
```

Default build type is `Debug`; core compiles with `-Wall -Wextra` under **C17**. There is **no native renderer**. The WASM/JS boundary is a separate build, not part of the CMake project:

```sh
scripts/build_wasm.sh   # bootstraps a repo-local pinned emsdk, emits build-wasm/ (git-ignored)
web/build.sh            # emcc → web/infeasible.cjs (self-contained, base64-embedded .wasm)
node web/host.mjs       # drives cellar_ground.story end-to-end from JS; prints "boundary OK: …"
```

## Architecture

One semantics, layered tiers (see `DESIGN.md` §4). Strict dependency direction — the presentation client (JS + Canvas2D over WASM) touches nothing below the frozen presentation interface (§12):

```
src/core/    arena allocator, string interning         (no deps)
src/logic/   defeasible engine: theory, solve, why      (deps: core)
src/state/   fact store, step function, inertia gen      (deps: core, logic)
src/lang/    .story compiler: lexer → parse → grounder    (deps: core, state)
src/wasm/    emcc-only JS↔C shim over world_* (not in CMake)
tests/       golden semantic tests + benchmarks (ctest)
```

`core`, `logic`, `state`, and `lang` link into one static lib `infeasible_core`; tests link against it. `src/wasm/bindings.c` compiles **only** under `emcc` (see `scripts/build_wasm.sh`) — it is not part of the CMake library. There is no native renderer tier — the shipped presentation is a web client over a WASM build.

### The logic engine (`src/logic/dl.h`)

Standard Billington / Antoniou-Billington-Governatori-Maher defeasible logic: **ambiguity-blocking, team defeat**. Literals are `(atom, neg)`. Rules are strict (`->`), defeasible (`=>`), or defeaters (`~>`, block-only — never conclude), plus an acyclic superiority relation `>`. `dl_solve` computes four proof statuses per literal via a tri-valued fixpoint: `dl_definite` = ±Δ (strict), `dl_defeasible` = ±∂. Verdicts are `DL_PROVED` / `DL_REFUTED` / `DL_UNDECIDED`. `dl_why` prints the proof/defeat trace — this trace is the product's whole point (a `why?` debugger), so keep it working.

API shape: `dl_theory_new` → `dl_add_rule`/`dl_add_sup`/`dl_add_fact` → `dl_solve` → query + `dl_why`. **All atoms must be interned before `dl_solve`** — the result array is sized to the intern table at call time.

The why-trace format lives once in `dl_trace.c/h` as a vtable over an opaque ctx; the scalar (`dl_result`) and columnar (`dlcol`) backings each supply accessors, so `dl_why` and `dlcol_why` render **byte-for-byte identical** traces by construction (pinned by `test_col`). Change trace formatting in one place.

Scaffold caveat: this is the correct-but-not-yet-linear implementation. The committed M3 linear path (behind the same API) is the Antoniou transformation + a stratifying/SCC-condensing compiler, then an **SCC-ordered ("weak-topological") sweep** as the evaluator — *not* Maher's counter/worklist route (rationale + literature in DESIGN.md §5.2). The golden tests pin the semantics so the swap stays safe. Cyclic rule graphs may leave literals `DL_UNDECIDED` (the future compiler rejects/stratifies cycles).

### State & the step function (`src/state/world.h`)

A `world` = base facts (the **only** mutable state, closed-world) + judgment rules + step rules.

- **Fluents** are ground boolean atoms; `world_declare_fluent` then `world_set`. Closed-world: each evaluation asserts `f` or `~f` for every declared fluent.
- **Judgment rules** (`world_add_rule`) derive conclusions from current state. These are queried via `world_query` / `world_why` and are **never stored back** as facts (invariant I1).
- **Step rules** (`world_add_step_rule`) are the *only* way facts change (I2). An `action` atom triggers the rule; `action == INTERN_NONE` makes it a **ramification** (fires in any step whose state matches — indirect effects like a dead guard dropping a torch). Body `step_cond`s may reference the *next* state via `primed=true`; effects are always about the next state.
- `world_step(actions…)` evaluates a primed-atom theory with auto-generated inertia rules (`f => f'`, causal rules beat inertia) and commits the next state. The theory's structure is compiled once into a cached columnar schema (`src/logic/dl_col.h`, an N=1 family) and invalidated when rules/fluents are added; each step only rewrites fact columns and re-solves. Returns `-1` **without mutating** if a fluent's next value is contested/undecided (conflict = authoring error), writing the fluent name into `err`.

Beyond boolean fluents, `world.h` also carries the M1 richer state model — **numeric** fluents (`world_declare_num`/`world_set_num`, comparison guards via `world_add_guard`, `:=`/`+=`/`-=` effects over a tiny expression VM via `world_add_expr_guard`/`world_add_num_effect`, dynamic clamp bounds), **multi-valued** fluents, **providers** (`world_set_provider_fn` — host-answered atoms like geometry/adjacency the engine can't derive), and **seeded rolls** (`world_set_seed`/`world_add_roll_site` — engine-side deterministic dice; I4). For scale, `world` also holds **columnar "lane" families** (`world_add_lane_family`, `world_add_step_lane_family`): bit-parallel judgment and transition over many entities at once, the performance thesis behind hitting 60fps at BG3 scale. Lanes are prototype-before-adopt — built and validated against the N=1 path before being routed through `world_step`.

### The .story compiler (`src/lang/`)

`story_compile(src, srcname, syms, diags)` is the front half of the M1 pipeline (`story.h`): a hand-written **lexer** (`lexer.c` — maximal-munch, tracks line/col for diagnostics; keywords for the full §6 surface are reserved even where the parser doesn't yet handle them) feeds a recursive-descent **parser** into an arena AST, then a semantic + **build-time grounder** emits ground rules into the `world_*` API. Typed variables (`X : actor`) are grounded up front over declared finite sorts, so `holding(actor,item)` expands to ground atoms interned as `"holding(a,b)"` — a host querying the equivalent ground atom sees the same id. The grammar handled by the current slice is documented at the top of `story.h`; read it before touching the parser. Diagnostics collect into a caller sink with panic-mode recovery at declaration boundaries; a compile fails only on error-severity diagnostics (warnings like orphan/typo detection never fail it).

### Core (`src/core/`)

`intern` maps names ↔ dense `uint32` atom ids; id 0 = `INTERN_NONE` sentinel. `arena` is a bump allocator — no individual frees, release the whole arena. No hidden allocation in the solve loop.

## Invariants — do not break (DESIGN.md §5.4)

- **I1 No write-back**: derived conclusions are never stored as base facts (that recreates Osiris's stale-fact bug).
- **I2 Actions are the only mutation**: everything changes facts exclusively through `world_step`.
- **I4 Determinism**: no wall-clock, no unseeded randomness in the logic core. A save is base facts + action log; replay is exact.

## Tests are golden semantic tests

`tests/test_dl.c` and `tests/test_world.c` pin the *meaning* of the engine, not implementation details: Tweety/penguin (superiority), defeaters, unresolved conflict, strict-wins, Yale shooting (inertia survives `wait`), torch ramification, flip-flop conflict rejection. The rest of the suite (numeric, multivalued, providers, lanes, and the `.story` compiler — `test_parse`, `test_ground`, `test_scene`, `test_bands`, …) pins each M1 slice. They use a `CHECK(cond)`-returns-1 harness (no framework) and each file's `main` runs its cases in sequence, printing "all passed". When changing engine internals, **the semantic tests must keep passing unchanged** — that is how the M3 algorithm swap and the lane/N=1 equivalence stay honest.

Add a new test target by extending the **first** `foreach` list in `CMakeLists.txt`. Tests that compile a real `.story` from `examples/` must **also** be added to the *second* `foreach` — it hands them `STORY_DIR` (the examples path) as a compile definition. `bench_*` targets are benchmarks, not tests: they're built `-O2` regardless of build type (build Release for meaningful numbers) and are not registered with ctest.
