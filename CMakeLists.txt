cmake_minimum_required(VERSION 3.20)
project(infeasible C)

set(CMAKE_C_STANDARD 11)
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
  src/lang/lexer.c
  src/lang/story.c
)
target_include_directories(infeasible_core PUBLIC src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(infeasible_core PRIVATE -Wall -Wextra)
endif()

enable_testing()
foreach(t IN ITEMS test_dl test_world test_multival test_landmark test_spawn test_col test_parse test_ground test_mv test_numeric test_numpipe test_numeff test_ramif test_prov test_lanes test_binder test_numlane)
  add_executable(${t} tests/${t}.c)
  target_link_libraries(${t} PRIVATE infeasible_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# These compile real .story files from the source tree; hand them the path.
foreach(t IN ITEMS test_parse test_ground test_mv test_numeric test_numeff test_ramif test_binder)
  target_compile_definitions(${t} PRIVATE
    STORY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")
endforeach()

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
