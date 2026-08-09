# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`infeasible` is a narrative game engine in C where **the world is a logic database**. One non-monotonic logic (propositional *defeasible logic*) serves as the single semantics for both *what is true* (stats, judgments) and *what happens next* (state transitions via defeasible inertia). Worlds are authored in a `.story` surface language that grounds to a vocabulary-checked C world; a Canvas2D web client (WASM engine + JS host) handles presentation.

**A narrative/dialogue layer is out of scope** (DESIGN.md §2). Games are built as host code against the `world_*` surface (the typed JS/C binding of §6.3). Do not add narrative concepts — knots, choices, diverts, a dialogue VM — to the design or code; a narrative front end, if ever built, is a client above the `world_*` surface, not part of the engine.

**Read `DESIGN.md` before non-trivial work** — it is the source of truth for semantics, invariants, and the milestone plan (§11). Current state: the logic engine, step function, and golden tests are in place; the **M1 `.story` compiler** (`src/lang/`: lexer → recursive-descent parse → build-time grounder) is live and growing slice by slice (typed vars over declared sorts, multi-valued and numeric fluents, providers, ramifications, set-quantified effect binders, burst cues). Every file in `examples/` compiles zero-diagnostic, pinned by `test_examples`; nine of them additionally have their *semantics* pinned (`test_prov`, `test_spatial`, `test_reaction`, `test_binder`, `test_taxprobe`, `test_probe5e`, `test_emit`). Add a new example to `test_examples`' list — the list is explicit so that a file outside the net is a deliberate omission rather than an oversight. Presentation is **M2**: a WASM build of the core (`scripts/build_wasm.sh`) driven from JS (`web/`) — a data-boundary spike today, Canvas2D rendering still to come.

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
src/lsp/     native .story language server (JSON-RPC/stdio)  (deps: core, lang)
src/wasm/    emcc-only JS↔C shim over world_* (not in CMake)
tests/       golden semantic tests + benchmarks (ctest)
```

`core`, `logic`, `state`, `lang`, and `lsp` link into one static lib `infeasible_core`; tests link against it. The `story-lsp` binary (`src/lsp/main.c`) links that lib — a shipped executable, not a test. `src/wasm/bindings.c` compiles **only** under `emcc` (see `scripts/build_wasm.sh`) — it is not part of the CMake library. There is no native renderer tier — the shipped presentation is a web client over a WASM build.

### The logic engine (`src/logic/dl.h`)

Standard Billington / Antoniou-Billington-Governatori-Maher defeasible logic: **ambiguity-blocking, team defeat**. Literals are `(atom, neg)`. Rules are strict (`->`), defeasible (`=>`), or defeaters (`~>`, block-only — never conclude), plus an acyclic superiority relation `>`. `dl_solve` computes four proof statuses per literal via a tri-valued fixpoint: `dl_definite` = ±Δ (strict), `dl_defeasible` = ±∂. Verdicts are `DL_PROVED` / `DL_REFUTED` / `DL_UNDECIDED`. `dl_why` prints the proof/defeat trace — this trace is the product's whole point (a `why?` debugger), so keep it working.

API shape: `dl_theory_new` → `dl_add_rule`/`dl_add_sup`/`dl_add_fact` → `dl_solve` → query + `dl_why`. **All atoms must be interned before `dl_solve`** — the result array is sized to the intern table at call time.

The why-trace format lives once in `dl_trace.c/h` as a vtable over an opaque ctx; the scalar (`dl_result`) and columnar (`dlcol`) backings each supply accessors, so `dl_why` and `dlcol_why` render **byte-for-byte identical** traces by construction (pinned by `test_col`). Change trace formatting in one place.

Evaluation order is the **SCC-ordered ("weak-topological") sweep** of §5.2 — walk the dependency condensation in topological order, iterate only inside a genuine component — *not* Maher's counter/worklist route (rationale + literature in DESIGN.md §5.2). It is the evaluator in `dl_col.c`, where the schedule compiles once with the schema (`compile_indices`) and every tick just walks it; `dl.c` keeps the plain sweep as its default and offers the ordered driver as `dl_solve_scc`, because that path recompiles per solve and cannot amortize a schedule. **Two condensations, different questions**: the cycle rule's (#109) is over the *support* graph and decides meaning; the schedule's is over the *dependency* graph — `body → head` **and** `body → head^1`, defeaters included, since deciding a literal reads its attackers' bodies — and decides only order. `dl_graph.c` holds the shared Tarjan so the two backings cannot drift. Still ahead for M3 linearity: the Antoniou transformation + stratifying compiler (the build-time half). The golden tests pin the semantics so each swap stays safe. Cycles follow the §5.2 cycle rule (#109, implemented in both engines): an unattacked support cycle is Datalog — least fixpoint, then unsupported members complete to REFUTED (`dl_why` renders the loop); a cycle any rule attacks stays honestly `DL_UNDECIDED` in the engine and is a located compile error in the `.story` layer ("defeat cannot reach through a cycle").

### State & the step function (`src/state/world.h`)

A `world` = base facts (the **only** mutable state, closed-world) + judgment rules + step rules.

- **Fluents** are ground boolean atoms; `world_declare_fluent` then `world_set`. Closed-world: each evaluation asserts `f` or `~f` for every declared fluent.
- **Judgment rules** (`world_add_rule`) derive conclusions from current state. These are queried via `world_query` / `world_why` and are **never stored back** as facts (invariant I1).
- **Step rules** (`world_add_step_rule`) are the *only* way facts change (I2). An `action` atom triggers the rule; `action == INTERN_NONE` makes it a **ramification** (fires in any step whose state matches — indirect effects like a dead guard dropping a torch). Body `step_cond`s may reference the *next* state via `primed=true`; effects are always about the next state.
- **Burst cues** (`world_declare_emit`/`world_emits`, `.story`: `emit spark(actor)`) are the step's transient OUTPUT channel (§12) — the twin of an action, which is its transient input. A cue is an effect head with no fact, no inertia and no commit: `world_step` reads the emissions off the same final solve the next state comes from, into a flat per-tick buffer the client renders and the next step clears. Write-only and positive-only (reading, negating or priming one is a located compile error), a set rather than a multiset, in declaration order (I4). A world with emissions steps N=1 — the lanes carry no emit columns, and the grounder builds no step lane family for one.
- `world_step(actions…)` evaluates a primed-atom theory with auto-generated inertia rules (`f => f'`, causal rules beat inertia) and commits the next state. The theory's structure is compiled once into a cached columnar schema (`src/logic/dl_col.h`, an N=1 family) and invalidated when rules/fluents are added; each step only rewrites fact columns and re-solves. Returns `-1` **without mutating** if a fluent's next value is contested/undecided (conflict = authoring error), writing the fluent name into `err`.

Beyond boolean fluents, `world.h` also carries the M1 richer state model — **numeric** fluents (`world_declare_num`/`world_set_num`, comparison guards via `world_add_guard`, `:=`/`+=`/`-=` effects over a tiny expression VM via `world_add_expr_guard`/`world_add_num_effect`, dynamic clamp bounds), **multi-valued** fluents, **providers** (`world_set_provider_fn` — host-answered *relations* like adjacency/LoS the engine can't derive; and `world_set_fn_provider_fn` — value-returning *functions* like grid movement `neighbor(cell, dir)`, consulted from the effect VM via `EXPR_CALL`), and **seeded rolls** (`world_set_seed`/`world_add_roll_site` — engine-side deterministic dice; I4). **Space is providers, not a primitive** (§5.6): positions are store-backed cell fluents, movement is a causal rule calling a function provider, and spatial relations (`near`, `los`) are boolean providers the host answers from its *own* index over `at(·)` — the grid geometry lives inside the provider, never in the engine ("hex vs. square is just the neighbor function inside the provider"). See `examples/patrol.story` + `test_spatial` for the worked pattern. For scale, `world` also holds **columnar "lane" families** (`world_add_lane_family`, `world_add_step_lane_family`): bit-parallel judgment and transition over many entities at once, the performance thesis behind hitting 60fps at BG3 scale. Both are routed: `world_query` answers from a judgment family when the queried atom is a lane cell, and `world_step` takes the step family when one covers the whole transition (plus the mixed lane/N=1 split path). `world_lanes_check` / `world_step_lanes_check` are the differential pins that keep each identical to N=1 — but `world_step_lanes_check` compares **boolean** next-state verdicts only: the numeric commit pipeline runs outside the verdict columns, so a wrong numeric delta needs a committed-state comparison too. `test_lanefront` runs both pins over the frontier's authoring shapes, and is the file to extend when a lane bail retires — a shape that flips to laned starts being differentially checked there automatically.

### The .story compiler (`src/lang/`)

`story_compile(src, srcname, syms, diags)` is the front half of the M1 pipeline (`story.h`): a hand-written **lexer** (`lexer.c` — maximal-munch, tracks line/col for diagnostics; keywords for the full §6 surface are reserved even where the parser doesn't yet handle them) feeds a recursive-descent **parser** into an arena AST, then a semantic + **build-time grounder** emits ground rules into the `world_*` API. Typed variables (`X : actor`) are grounded up front over declared finite sorts, so `holding(actor,item)` expands to ground atoms interned as `"holding(a,b)"` — a host querying the equivalent ground atom sees the same id. The grammar handled by the current slice is documented at the top of `story.h`; read it before touching the parser. Diagnostics collect into a caller sink with panic-mode recovery at declaration boundaries; a compile fails only on error-severity diagnostics (warnings like orphan/typo detection never fail it). Beyond the world and diagnostics, `story_compile_model` emits a **span model** (`src/lang/story_model.h`) — symbols + occurrences with source spans, harvested from the parser's already-spanned tables (best-effort even on a failed compile). It is a tier-neutral `lang` output and the single source of truth for span-based tooling (the LSP, hover, §6.3 interface artifact, §6.1 cones); consumers must read it rather than re-parsing.

### The language server (`src/lsp/`)

A native LSP for `.story` (DESIGN.md §6.1 item 7), JSON-RPC 2.0 over stdio. It links the compiler directly, so an editor sees the **same `story_diags`** an author gets at build time. `lsp_dispatch` is the pure core — feed it one message body, capture replies through a sink — so behaviour is tested without a process (`test_lsp`); `lsp_run` wires it to framed stdio, and `main.c` is just `stdin`/`stdout`. `json.c` is a small zero-dependency JSON parser (arena value tree) + a self-escaping `strbuf` writer — no external LSP/JSON deps, matching the repo's no-hidden-allocation ethos. Features: lifecycle (`initialize`/`shutdown`/`exit`), full-text sync (`didOpen`/`didChange`/`didClose`), push diagnostics, **navigation** (go-to-definition, find-references, documentSymbol), **hover** — a dependency/attacker cone summary (which rules conclude the atom vs. which attack it — a `~p` head — and how many bodies read it), and **callHierarchy** — the *navigable* cone (`outgoing` = what an atom is affected by, its rules' premises; `incoming` = what it feeds, the heads of rules reading it; polarity-agnostic — attackers live in hover).

**Everything reads the compiler's span model, never a re-parse.** The parser already tracks `line, col` on every AST node; `story_compile_model` harvests that into a `story_model` (see `src/lang/story_model.h`) — a tier-neutral **`lang` output**: `symbols` (declarations + kind + span + a compact `detail` signature like `provider(actor, actor)` or `fluent : int in 0..40` — the concept word the closed LSP SymbolKind enum can't carry), `occurrences` (atom, role ∈ decl/head/body/effect/arg, span, **`neg` polarity**, and the owning **`rule` index**), and `rules` (label + span). Head polarity + rule grouping are what the §6.1/§9 cone is built over: a HEAD occ with `neg=true` **attacks** its pred, one with `neg=false` **concludes** it. One source of truth, shared with the §6.3 interface artifact. **Do not re-lex/re-parse in the LSP** — a second grammar walker drifts.

A thin **VS Code client** lives in `editors/vscode/` (buildless CommonJS, matching the repo's no-build-step JS ethos — not in CMake): it only spawns `story-lsp` over stdio via `vscode-languageclient` and contributes the `.story` file association, comment/bracket config, and a TextMate grammar. Any language logic belongs in the server, so every editor benefits — the shim stays dumb. `npm install` + F5 to run (see its README). Positions are UTF-16 code units (the LSP default): the model/compiler produce byte columns, converted at the protocol boundary against the document text (`write_range` outgoing, `nav_target` incoming) — a no-op for ASCII, which the whole `.story` identifier grammar is. The span model is **cached per document**: one compile per edit in `refresh` (which produces both the published diagnostics and the cached model), reused by every navigation request until a didChange invalidates it — not a recompile per request. **Next:** effect-expr-tree traversal in the harvest (roll/fn-call guard reads), incremental document sync.

### Core (`src/core/`)

`intern` maps names ↔ dense `uint32` atom ids; id 0 = `INTERN_NONE` sentinel. `arena` is a bump allocator — no individual frees, release the whole arena. No hidden allocation in the solve loop.

## Invariants — do not break (DESIGN.md §5.4)

- **I1 No write-back**: derived conclusions are never stored as base facts (that recreates Osiris's stale-fact bug).
- **I2 Actions are the only mutation**: everything changes facts exclusively through `world_step`.
- **I4 Determinism**: no wall-clock, no unseeded randomness in the logic core. A save is base facts + action log; replay is exact.

## Tests are golden semantic tests

`tests/test_dl.c` and `tests/test_world.c` pin the *meaning* of the engine, not implementation details: Tweety/penguin (superiority), defeaters, unresolved conflict, strict-wins, Yale shooting (inertia survives `wait`), torch ramification, flip-flop conflict rejection. The rest of the suite (numeric, multivalued, providers, lanes, and the `.story` compiler — `test_parse`, `test_ground`, `test_scene`, `test_bands`, …) pins each M1 slice. They use a `CHECK(cond)`-returns-1 harness (no framework) and each file's `main` runs its cases in sequence, printing "all passed". When changing engine internals, **the semantic tests must keep passing unchanged** — that is how the M3 algorithm swap and the lane/N=1 equivalence stay honest.

Add a new test target by extending the **first** `foreach` list in `CMakeLists.txt`. Tests that compile a real `.story` from `examples/` must **also** be added to the *second* `foreach` — it hands them `STORY_DIR` (the examples path) as a compile definition. `bench_*` targets are benchmarks, not tests: they're built `-O2` regardless of build type (build Release for meaningful numbers) and are not registered with ctest.
