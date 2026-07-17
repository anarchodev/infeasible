# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`infeasible` is a narrative game engine in C where **the world is a logic database**. One non-monotonic logic (propositional *defeasible logic*) serves as the single semantics for both *what is true* (stats, judgments) and *what happens next* (state transitions via defeasible inertia). Games are built as host code against a generated, vocabulary-checked C API; raylib handles presentation.

**A narrative/dialogue layer is explicitly out of scope** (DESIGN.md §12.1). An Ink-style "weave" was designed and then removed to keep the project focused on the rules engine. Do not reintroduce weave concepts — knots, choices, diverts, a narrative VM — into the design or code; §12.1 records the seam it would reattach at if it ever returns.

**Read `DESIGN.md` before non-trivial work** — it is the source of truth for semantics, invariants, and the milestone plan. The current tree is **M0 (scaffold)**: core + logic engine + step function + golden tests + raylib shell. The `.story` language and its parser do **not exist yet** (M1). `examples/cellar.story` is a sketch of the *future* surface syntax, not something that compiles today — worlds are built via the C API.

## Build & test

```sh
# Core + tests only — no raylib, no display needed. Use this for logic/engine work.
cmake -B build -DINFEASIBLE_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# A single test target:
ctest --test-dir build -R test_dl --output-on-failure
./build/test_dl          # or run the binary directly; it prints "test_dl: all passed"

# Full build with the raylib demo (fetches raylib 5.5; needs X11/Wayland dev packages):
cmake -B build && cmake --build build && ./build/infeasible
```

Default build type is `Debug`; core compiles with `-Wall -Wextra`. Prefer `-DINFEASIBLE_BUILD_APP=OFF` when iterating — it skips the raylib fetch/compile entirely.

## Architecture

Three tiers, one semantics (see `DESIGN.md` §4). Strict dependency direction — **raylib touches nothing below `app/`**:

```
src/core/    arena allocator, string interning        (no deps)
src/logic/   defeasible engine: theory, solve, why     (deps: core)
src/state/   fact store, step function, inertia gen    (deps: core, logic)
src/app/     raylib shell + demo scene                 (only raylib user)
tests/       golden semantic tests (ctest)
```

Everything links into one static lib `infeasible_core`; tests and the app link against it.

### The logic engine (`src/logic/dl.h`)

Standard Billington / Antoniou-Billington-Governatori-Maher defeasible logic: **ambiguity-blocking, team defeat**. Literals are `(atom, neg)`. Rules are strict (`->`), defeasible (`=>`), or defeaters (`~>`, block-only — never conclude), plus an acyclic superiority relation `>`. `dl_solve` computes four proof statuses per literal via a tri-valued fixpoint: `dl_definite` = ±Δ (strict), `dl_defeasible` = ±∂. Verdicts are `DL_PROVED` / `DL_REFUTED` / `DL_UNDECIDED`. `dl_why` prints the proof/defeat trace — this trace is the product's whole point (a `why?` debugger), so keep it working.

API shape: `dl_theory_new` → `dl_add_rule`/`dl_add_sup`/`dl_add_fact` → `dl_solve` → query + `dl_why`. **All atoms must be interned before `dl_solve`** — the result array is sized to the intern table at call time.

Scaffold caveat: this is the correct-but-not-yet-linear implementation. Maher's linear algorithm and the transformation pipeline (M3) will land *behind the same API*; the golden tests pin the semantics so the swap stays safe. Cyclic rule graphs may leave literals `DL_UNDECIDED` (the future compiler rejects cycles).

### State & the step function (`src/state/world.h`)

A `world` = base facts (the **only** mutable state, closed-world) + judgment rules + step rules.

- **Fluents** are ground boolean atoms; `world_declare_fluent` then `world_set`. Closed-world: each evaluation asserts `f` or `~f` for every declared fluent.
- **Judgment rules** (`world_add_rule`) derive conclusions from current state. These are queried via `world_query` / `world_why` and are **never stored back** as facts (invariant I1).
- **Step rules** (`world_add_step_rule`) are the *only* way facts change (I2). An `action` atom triggers the rule; `action == INTERN_NONE` makes it a **ramification** (fires in any step whose state matches — indirect effects like a dead guard dropping a torch). Body `step_cond`s may reference the *next* state via `primed=true`; effects are always about the next state.
- `world_step(actions…)` builds a primed-atom theory with auto-generated inertia rules (`f => f'`, causal rules beat inertia), runs one defeasible pass, and commits the next state. Returns `-1` **without mutating** if a fluent's next value is contested/undecided (conflict = authoring error), writing the fluent name into `err`.

### Core (`src/core/`)

`intern` maps names ↔ dense `uint32` atom ids; id 0 = `INTERN_NONE` sentinel. `arena` is a bump allocator — no individual frees, release the whole arena. No hidden allocation in the solve loop.

## Invariants — do not break (DESIGN.md §5.4)

- **I1 No write-back**: derived conclusions are never stored as base facts (that recreates Osiris's stale-fact bug).
- **I2 Actions are the only mutation**: everything changes facts exclusively through `world_step`.
- **I4 Determinism**: no wall-clock, no unseeded randomness in the logic core. A save is base facts + action log; replay is exact.

## Tests are golden semantic tests

`tests/test_dl.c` and `tests/test_world.c` pin the *meaning* of the engine, not implementation details: Tweety/penguin (superiority), defeaters, unresolved conflict, strict-wins, Yale shooting (inertia survives `wait`), torch ramification, flip-flop conflict rejection. They use a `CHECK(cond)`-returns-1 harness (no framework) and each file's `main` runs its cases in sequence. When changing engine internals, **these must keep passing unchanged** — that is how the M3 algorithm swap stays honest. Add a new test target by extending the `foreach` list in `CMakeLists.txt`.
