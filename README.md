# infeasible

A narrative game engine in C: the world is a logic database. Defeasible logic
for judgments, defeasible inertia for state transitions. Host code drives it
through a generated, vocabulary-checked C API. raylib for presentation.

A narrative/dialogue layer is out of scope — see DESIGN.md §2.

**Read [DESIGN.md](DESIGN.md) first** — it explains the semantics, the
architecture, and the milestone plan. `examples/cellar.story` sketches the
surface language (parser lands in M1; the scaffold builds worlds via the C API).

## Build

```sh
# core + tests only (no raylib, no display needed)
cmake -B build -DINFEASIBLE_BUILD_APP=OFF
cmake --build build
ctest --test-dir build --output-on-failure

# with the raylib demo app (fetches raylib 5.5; needs X11/Wayland dev packages on Linux)
cmake -B build
cmake --build build
./build/infeasible
```

## Layout

```
src/core/   arena allocator, string interning
src/logic/  defeasible logic engine (solve + why-traces)
src/state/  fact store, step function (inertia, ramifications, conflicts)
src/app/    raylib demo (interactive cellar scene)
tests/      golden semantic tests: Yale shooting, cellar, ramifications, conflicts
examples/   surface-language sketches (.story)
```
