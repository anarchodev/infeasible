cmake_minimum_required(VERSION 3.20)
project(infeasible C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# No native renderer: presentation is a JS + Canvas2D web client over a WASM
# build of the core (DESIGN.md §12). This project builds the engine, tests, and
# benchmarks only. raylib was dropped 2026-07-21 when Canvas2D became the single
# renderer.

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(infeasible_core STATIC
  src/core/arena.c
  src/core/intern.c
  src/logic/dl.c
  src/logic/dl_col.c
  src/logic/dl_trace.c
  src/state/world.c
  src/state/factindex.c
  src/lang/lexer.c
  src/lang/story.c
  src/lsp/json.c
  src/lsp/lsp.c
)
target_include_directories(infeasible_core PUBLIC src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(infeasible_core PRIVATE -Wall -Wextra)
endif()

enable_testing()
foreach(t IN ITEMS test_dl test_world test_multival test_landmark test_spawn test_col test_parse test_scene test_bands test_clamp test_setparam test_domain test_intargs test_functional test_store test_fnprovider test_ground test_mv test_numeric test_numpipe test_numeff test_ramif test_prov test_lanes test_binder test_numlane test_binderlane test_judgstep test_provider test_safety test_cardinality test_spatial test_factindex test_matcher test_ticktime test_selectivity test_dlcol_incr test_locstable test_matchview test_sparsejfam test_sparseuniverse test_lazyq test_inits test_model test_lsp test_div test_value test_enumarg test_dtype test_merge test_exprtest test_strata test_layers test_modeled test_kinds test_reaction test_split test_taxonomy)
  add_executable(${t} tests/${t}.c)
  target_link_libraries(${t} PRIVATE infeasible_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# These compile real .story files from the source tree; hand them the path.
foreach(t IN ITEMS test_parse test_ground test_mv test_numeric test_numeff test_ramif test_binder test_provider test_spatial test_reaction)
  target_compile_definitions(${t} PRIVATE
    STORY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
endforeach()

# The native .story language server (DESIGN.md §6.1 item 7): JSON-RPC over
# stdio, pushing compiler diagnostics to an editor. Not a test — a shipped
# binary; the server logic is covered by test_lsp.
add_executable(story-lsp src/lsp/main.c)
target_link_libraries(story-lsp PRIVATE infeasible_core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(story-lsp PRIVATE -Wall -Wextra)
endif()

# Solver micro-benchmark (not a test). Built -O2 regardless of build type so
# the numbers reflect release codegen: `./build/bench_dl [natoms] [fanin] [iters]`.
add_executable(bench_dl tests/bench_dl.c)
target_link_libraries(bench_dl PRIVATE infeasible_core)
target_compile_options(bench_dl PRIVATE -O2)

# Columnar-vs-scalar family benchmark (DESIGN.md 5.8): build Release for
# meaningful numbers — the core library follows CMAKE_BUILD_TYPE.
add_executable(bench_col tests/bench_col.c)
target_link_libraries(bench_col PRIVATE infeasible_core)
target_compile_options(bench_col PRIVATE -O2)

# Vertical-slice RTS tick loop: provider + columnar judge + columnar step,
# deterministic (replay-hash checked). Build Release for meaningful numbers.
add_executable(bench_slice tests/bench_slice.c)
target_link_libraries(bench_slice PRIVATE infeasible_core)
target_compile_options(bench_slice PRIVATE -O2)

# Transition-layer benchmark: world_step routed on lanes vs the N=1 path.
add_executable(bench_step tests/bench_step.c)
target_link_libraries(bench_step PRIVATE infeasible_core)
target_compile_options(bench_step PRIVATE -O2)

# Realistic scenario: 5e mass-combat conditions, lane vs N=1, time AND space.
add_executable(bench_5e tests/bench_5e.c)
target_link_libraries(bench_5e PRIVATE infeasible_core)
target_compile_options(bench_5e PRIVATE -O2)

# Grounding-cost benchmark (§5.2/§8, EPIC #26): eager grounder over a synthetic
# nᵏ cross product — compile time, ground atoms, peak memory, MAX_INSTANCES cliff.
add_executable(bench_ground tests/bench_ground.c)
target_link_libraries(bench_ground PRIVATE infeasible_core)
target_compile_options(bench_ground PRIVATE -O2)

# Matcher-vs-eager benchmark (EPIC #43 / §8.1): time + space across density/scale.
add_executable(bench_matcher tests/bench_matcher.c)
target_link_libraries(bench_matcher PRIVATE infeasible_core)
target_compile_options(bench_matcher PRIVATE -O2)

# #84 adoption gate (DESIGN §5.8): configured per-type stage vs the modeled
# .story pipeline. Build Release for meaningful numbers; not a ctest target.
add_executable(bench_dtype tests/bench_dtype.c)
target_link_libraries(bench_dtype PRIVATE infeasible_core)
target_compile_options(bench_dtype PRIVATE -O2)

# #121 adoption gate (EPIC #117): full phase-guarded schema vs hand-narrowed
# per-phase worlds — `split` is built only if narrowing wins >= 1.3x at 100k.
add_executable(bench_phase tests/bench_phase.c)
target_link_libraries(bench_phase PRIVATE infeasible_core)
target_compile_options(bench_phase PRIVATE -O2)
