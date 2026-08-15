# infeasible

A narrative game engine in C: the world is a logic database. Defeasible logic
for judgments, defeasible inertia for state transitions. Host code drives it
through a generated, vocabulary-checked C API. A hand-written Canvas2D web
renderer for presentation (WASM engine + JS host loop).

A narrative/dialogue layer is out of scope — see DESIGN.md §2.

**Read [DESIGN.md](DESIGN.md) first** — it explains the semantics, the
architecture, and the milestone plan. `examples/cellar.story` sketches the
surface language (parser lands in M1; the scaffold builds worlds via the C API).

## Build

```sh
# core + tests (no renderer, no display needed — this is the whole native build)
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Presentation is a JS + Canvas2D web client over a WASM build of the core (not
built here yet — see DESIGN.md §11–§12); there is no native renderer.

## Layout

```
src/core/   arena allocator, string interning
src/logic/  defeasible logic engine (solve + why-traces)
src/state/  fact store, step function (inertia, ramifications, conflicts)
tests/      golden semantic tests: Yale shooting, cellar, ramifications, conflicts
examples/   surface-language sketches (.story)
```

## Licence

The engine is **Apache-2.0** (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).
Apache rather than MIT for two reasons that matter to a project taking outside
contributions: an express patent grant with retaliation, and §5, under which a
contribution is offered under the licence unless it says otherwise — a CLA for
code, without the paperwork.

**Content is licensed separately**, because Apache-2.0 is a software licence:

- assets (graphics, audio) under **CC0 or CC BY 4.0** — never `BY-NC` (which
  forbids selling the result) or `BY-ND` (which forbids the remixing that is
  the point);
- fonts under the **SIL Open Font License**, or supplied by the backend, since
  §12 freezes text as metrics rather than glyphs.

Each contributed asset carries a manifest entry — author, licence, source —
which serves the CC BY attribution requirement and the credits from one record.
