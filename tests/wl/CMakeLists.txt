# tests/wl/ — the Wayland layer's own suite, as it came from loom.
#
# Algebra laws, resources, protocol completeness, a forked client/server
# round trip, server conformance, a decoder fuzzer, and the compile-fail
# cases that prove misuse (a request newer than the bound version, a
# server sending a request, copying an fd, ...) does not build.

foreach(t test_algebra test_resources test_complete)
    add_executable(wl_${t} ${t}.cpp)
    target_link_libraries(wl_${t} PRIVATE i4::wl_client)
    add_test(NAME wl_${t} COMMAND wl_${t})
endforeach()

foreach(t test_roundtrip test_server)
    add_executable(wl_${t} ${t}.cpp)
    target_link_libraries(wl_${t} PRIVATE i4::wl_client i4::wl_server)
    add_test(NAME wl_${t} COMMAND wl_${t})
    # Each test makes its own wayland socket; give it a private runtime dir.
    set_tests_properties(wl_${t} PROPERTIES ENVIRONMENT "XDG_RUNTIME_DIR=${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

add_executable(wl_fuzz_decoder fuzz_decoder.cpp)
target_link_libraries(wl_fuzz_decoder PRIVATE i4::wl_client)
add_test(NAME wl_fuzz_decoder COMMAND wl_fuzz_decoder 200000)

add_test(NAME wl_compile_fail
         COMMAND ${CMAKE_COMMAND}
                 -DCXX=${CMAKE_CXX_COMPILER}
                 -DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
                 -DBIN_DIR=${PROJECT_BINARY_DIR}
                 -DJAAL_INCLUDE=${PROJECT_SOURCE_DIR}/third_party/mayag/third_party/jaal/include
                 -DROOT_INCLUDE=${PROJECT_SOURCE_DIR}/include
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail.cmake)
