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
  src/state/world.c
)
target_include_directories(infeasible_core PUBLIC src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(infeasible_core PRIVATE -Wall -Wextra)
endif()

enable_testing()
foreach(t IN ITEMS test_dl test_world)
  add_executable(${t} tests/${t}.c)
  target_link_libraries(${t} PRIVATE infeasible_core)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

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
