# cmake/wl.cmake — i4's Wayland layer (copied from loom, as i4::wl).
#
# Protocol XML compiles into headers of TYPES (tools/wlgen.py): interfaces,
# versions and message signatures become C++ types, so a request newer than
# the bound version, a wrong object type, or a server sending a request is a
# compile error. Every protocol the system has, plus the wlroots, KDE and
# Hyprland ones vendored in protocols/ (the system's copy wins on overlap).
#
#   i4::wl_core    the algebra and the wire format, header-only
#   i4::wl_client  being a client (tests, and a nested head)
#   i4::wl_server  being a compositor (i4d)
#
# Neither side runs a loop: the server says which fds to watch and is told
# which are ready. jaal does the waiting (src/server/host.cpp).

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_get_variable(WAYLAND_XML_DIR wayland-scanner pkgdatadir)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT WAYLAND_XML_DIR)
    set(WAYLAND_XML_DIR /usr/share/wayland)
endif()
if(NOT WAYLAND_PROTOCOLS_DIR)
    set(WAYLAND_PROTOCOLS_DIR /usr/share/wayland-protocols)
endif()

set(I4_WLGEN ${PROJECT_SOURCE_DIR}/tools/wlgen.py)
set(I4_WL_GEN_DIR ${PROJECT_BINARY_DIR}/generated/i4/wl/proto)
file(MAKE_DIRECTORY ${I4_WL_GEN_DIR})

set(I4_WL_GENERATED "")
function(i4_wl_protocol name)
    cmake_parse_arguments(P "" "" "XML;DEPENDS" ${ARGN})
    set(out ${I4_WL_GEN_DIR}/${name}.hpp)
    set(inc "")
    foreach(dep ${P_DEPENDS})
        list(APPEND inc --include i4/wl/proto/${dep}.hpp)
    endforeach()
    add_custom_command(OUTPUT ${out}
        COMMAND ${Python3_EXECUTABLE} ${I4_WLGEN} ${P_XML} -o ${out} ${inc}
        DEPENDS ${I4_WLGEN} ${P_XML}
        COMMENT "wlgen ${name}.hpp" VERBATIM)
    set(I4_WL_GENERATED ${I4_WL_GENERATED} ${out} PARENT_SCOPE)
endfunction()

i4_wl_protocol(wayland XML ${WAYLAND_XML_DIR}/wayland.xml)
i4_wl_protocol(xdg_shell XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml DEPENDS wayland)

execute_process(
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/select_protocols.py
            ${WAYLAND_PROTOCOLS_DIR} ${PROJECT_SOURCE_DIR}/protocols
    OUTPUT_VARIABLE I4_WL_XML OUTPUT_STRIP_TRAILING_WHITESPACE)
foreach(xml ${I4_WL_XML})
    get_filename_component(stem ${xml} NAME_WE)
    string(REPLACE "-" "_" stem ${stem})
    if(NOT stem STREQUAL "xdg_shell")
        i4_wl_protocol(${stem} XML ${xml} DEPENDS wayland)
    endif()
endforeach()
list(LENGTH I4_WL_XML n)
message(STATUS "i4::wl: ${n} protocols")

# Every protocol in one header, so "it all composes" is compiled, not claimed.
set(all "// GENERATED: every protocol.\n#pragma once\n")
foreach(h ${I4_WL_GENERATED})
    get_filename_component(stem ${h} NAME)
    string(APPEND all "#include <i4/wl/proto/${stem}>\n")
endforeach()
file(GENERATE OUTPUT ${I4_WL_GEN_DIR}/all.hpp CONTENT "${all}")

# Every global interface at its newest version: the conformance checklist.
add_custom_command(OUTPUT ${I4_WL_GEN_DIR}/globals.hpp
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/wlglobals.py ${WAYLAND_XML_DIR}/wayland.xml
            ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml ${I4_WL_XML} -o ${I4_WL_GEN_DIR}/globals.hpp
    DEPENDS ${PROJECT_SOURCE_DIR}/tools/wlglobals.py ${I4_WL_XML}
    COMMENT "wlglobals globals.hpp" VERBATIM)
list(APPEND I4_WL_GENERATED ${I4_WL_GEN_DIR}/globals.hpp)

add_custom_target(i4_wl_protocols DEPENDS ${I4_WL_GENERATED})

add_library(i4_wl_core INTERFACE)
add_dependencies(i4_wl_core i4_wl_protocols)
target_include_directories(i4_wl_core INTERFACE ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(i4_wl_core INTERFACE jaal::jaal)
add_library(i4::wl_core ALIAS i4_wl_core)

add_library(i4_wl_client STATIC src/wl/connection.cpp)
target_link_libraries(i4_wl_client PUBLIC i4_wl_core Threads::Threads)
add_library(i4::wl_client ALIAS i4_wl_client)

add_library(i4_wl_server STATIC src/wl/server/display.cpp)
target_link_libraries(i4_wl_server PUBLIC i4_wl_core Threads::Threads)
add_library(i4::wl_server ALIAS i4_wl_server)
