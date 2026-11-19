# cmake/layering.cmake — i4's layers, bottom to top, enforced.
#
#   base       vocabulary
#   wm         the i3 brain (pure)
#   link       server <-> head protocol (pure)
#   wl         Wayland wire + objects, both halves (from loom)
#   transport  moving link messages between processes (POSIX)
#   server     i4d                 } same rank: neither may include the other.
#   head       i4-head             } They meet only through link.
#   app        binaries
#
# A file in include/i4/<A>/ or src/<A>/ may include i4/<B>/ (or ../<B>/) only
# when B is STRICTLY lower than A. Equal ranks are siblings and may not see
# each other: that is what keeps the server from reaching into the head, which
# is the whole client/server split.
#
# Portable layers (base, wm, link) build on every head platform — Windows,
# macOS, Android, the web — so they may not include an OS header.
#
# Run with: cmake -DROOT=<i4 source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(rank_base      0)
set(rank_wm        1)
set(rank_link      2)
set(rank_wl        2)
set(rank_transport 3)
set(rank_server    4)
set(rank_head      4)
set(rank_app       5)

set(portable base wm link)
set(os_headers "unistd\\.h|sys/|linux/|fcntl\\.h|poll\\.h|xf86drm|gbm\\.h|EGL/|windows\\.h|wayland-")

set(hits "")
foreach(tree include/i4 src)
    file(GLOB dirs LIST_DIRECTORIES true ${ROOT}/${tree}/*)
    foreach(d IN LISTS dirs)
        if(NOT IS_DIRECTORY ${d})
            continue()
        endif()
        get_filename_component(layer ${d} NAME)
        if(NOT DEFINED rank_${layer})
            list(APPEND hits "${tree}/${layer}/: not a known layer (add it to cmake/layering.cmake)")
            continue()
        endif()
        file(GLOB_RECURSE files ${d}/*.hpp ${d}/*.cpp)
        foreach(f IN LISTS files)
            file(RELATIVE_PATH rel ${ROOT} ${f})
            file(STRINGS ${f} lines REGEX "^[ \t]*#[ \t]*include")
            foreach(l IN LISTS lines)
                string(STRIP "${l}" s)
                if(l MATCHES "(\\.\\./|i4/)([a-z_]+)/")
                    set(target ${CMAKE_MATCH_2})
                    if(DEFINED rank_${target} AND NOT target STREQUAL layer
                       AND NOT rank_${target} LESS rank_${layer})
                        list(APPEND hits "${rel}: ${s}   (${layer}=${rank_${layer}} may not include ${target}=${rank_${target}})")
                    endif()
                endif()
                if(layer IN_LIST portable AND l MATCHES "<(${os_headers})")
                    list(APPEND hits "${rel}: ${s}   (${layer} is portable: no OS headers)")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR "i4 layering violated:\n  ${msg}\n\n"
        "Move the shared piece DOWN a layer, or pass it in as a template "
        "parameter. server and head meet only through link.")
endif()
message(STATUS "i4 layering: ok")
