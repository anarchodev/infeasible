cmake_minimum_required(VERSION 3.20)
project(infeasible C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(INFEASIBLE_BUILD_APP "Build the raylib demo app" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_library(infeasible_core STATIC
  src/core/arena.c
  src/core/intern.c
  src/logic/dl.c
  src/logic/dl_col.c
  src/state/world.c
)
target_include_directories(infeasible_core PUBLIC src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(infeasible_core PRIVATE -Wall -Wextra)
endif()

enable_testing()
foreach(t IN ITEMS test_dl test_world test_multival test_landmark test_spawn test_col)
  add_executable(${t} tests/${t}.c)
  target_link_libraries(${t} PRIVATE infeasible_core)
  add_test(NAME ${t} COMMAND ${t})
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

if(INFEASIBLE_BUILD_APP)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(raylib)

  add_executable(infeasible src/app/main.c)
  target_link_libraries(infeasible PRIVATE infeasible_core raylib)
endif()
