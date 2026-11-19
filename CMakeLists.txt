cmake_minimum_required(VERSION 3.29)
project(i4 VERSION 0.1.0 LANGUAGES CXX)

# i4 — an i3-style Wayland compositor, split into a session server (i4d) and
# a presenter (i4-head). See docs/ARCHITECTURE.md.
#
# Layers, bottom to top (enforced by cmake/layering.cmake):
#
#   base     vocabulary: ids, typed coordinate spaces, match, contracts
#   wm       the i3 brain: tree, commands, arrangement. Pure, portable.
#   link     server <-> head protocol                       (milestone 3)
#   server   i4d: Wayland protocol + policy, on jaal        (milestone 2)
#   head     i4-head: output, input, rendering, on mayag    (milestone 2)
#   app      the binaries
#
# base, wm and link are PORTABLE: they build for every head platform, so they
# may not include an OS header. The lint checks that too.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Headers, not C++ modules: GCC, Clang, MSVC and Emscripten all have to
# build the portable layers, and module support still differs across them.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(I4_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(I4_WERROR      "Treat warnings as errors"  OFF)

# Warnings every i4 target compiles with. -Werror=switch is load-bearing: an
# enum that gains a case must break every switch that forgot it.
add_library(i4_warnings INTERFACE)
target_compile_options(i4_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Werror=switch -Werror=return-type
    $<$<BOOL:${I4_WERROR}>:-Werror>)

# -- base ---------------------------------------------------------------------
add_library(i4_base INTERFACE)
target_include_directories(i4_base INTERFACE ${PROJECT_SOURCE_DIR}/include)
add_library(i4::base ALIAS i4_base)

# -- wm -----------------------------------------------------------------------
add_library(i4_wm STATIC
    src/wm/tree.cpp
    src/wm/apply.cpp
    src/wm/arrange.cpp
    src/wm/parse.cpp
    src/wm/shape.cpp)
target_link_libraries(i4_wm PUBLIC i4::base PRIVATE i4_warnings)
add_library(i4::wm ALIAS i4_wm)

# -- server / head --------------------------------------------------------------
# Not yet. They will pull in third_party/mayag, which brings jaal (the
# runtime) with it as third_party/mayag/third_party/jaal:
#
#   add_subdirectory(third_party/mayag EXCLUDE_FROM_ALL)
#
# The wm layer must never depend on either: it is what the head runs to
# predict, and what tests run with nothing else present.

if(I4_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
