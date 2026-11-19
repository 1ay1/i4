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
    src/wm/bindings.cpp
    src/wm/parse.cpp
    src/wm/shape.cpp)
target_link_libraries(i4_wm PUBLIC i4::base PRIVATE i4_warnings)
add_library(i4::wm ALIAS i4_wm)

# -- link ---------------------------------------------------------------------
# The codec is header-only (it is all templates); the replica is not.
add_library(i4_link STATIC src/link/predict.cpp)
target_link_libraries(i4_link PUBLIC i4::wm PRIVATE i4_warnings)
add_library(i4::link ALIAS i4_link)

# -- third party ------------------------------------------------------------------
# mayag brings jaal (the runtime). SYSTEM: i4's warning flags are for i4's
# code, not for a dependency's headers.
set(MAYAG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MAYAG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mayag EXCLUDE_FROM_ALL SYSTEM)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)

# -- wl: Wayland, both halves, no libwayland (from loom) -------------------------
include(cmake/wl.cmake)

# -- transport ------------------------------------------------------------------
# Moving link messages between processes: a SOCK_SEQPACKET channel with fds
# riding alongside. POSIX; a head on another OS gets its own.
add_library(i4_transport STATIC src/transport/channel.cpp)
target_link_libraries(i4_transport PUBLIC i4::link jaal::jaal PRIVATE i4_warnings)

# -- server: i4d ------------------------------------------------------------------
# session.cpp is the program (pure, tested headless); host*.cpp the shell.
add_library(i4_session STATIC src/server/session.cpp)
target_link_libraries(i4_session PUBLIC i4::link jaal::jaal PRIVATE i4_warnings)

add_library(i4_server STATIC src/server/host.cpp src/server/host_protocol.cpp src/server/keyboard.cpp)
target_link_libraries(i4_server PUBLIC i4_session i4_transport i4::wl_server PkgConfig::XKB PRIVATE i4_warnings)

add_executable(i4d src/app/i4d.cpp)
target_link_libraries(i4d PRIVATE i4_server i4_warnings)

add_executable(i4-watch src/app/watch.cpp)
target_link_libraries(i4-watch PRIVATE i4_transport i4_warnings)

# -- head + preview ---------------------------------------------------------------
option(I4_WITH_HEAD "Build the mayag head and i4-preview" ON)
if(I4_WITH_HEAD)
    add_library(i4_head STATIC src/head/scene.cpp)
    target_link_libraries(i4_head PUBLIC i4::link mayag::mayag PRIVATE i4_warnings)

    add_executable(i4-preview src/app/preview.cpp)
    target_link_libraries(i4-preview PRIVATE i4_head i4_warnings)
endif()

if(I4_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
