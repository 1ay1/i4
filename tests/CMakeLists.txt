# tests/ — behaviour, randomized invariants, and what must NOT compile.

function(i4_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE i4::link i4_warnings)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

i4_test(test_parse)
i4_test(test_wm)
i4_test(test_arrange)
i4_test(test_determinism)
i4_test(test_wire)
i4_test(test_replica)
i4_test(test_bindings)

add_executable(test_tiles test_tiles.cpp)
target_link_libraries(test_tiles PRIVATE i4_remote i4_warnings)
add_test(NAME test_tiles COMMAND test_tiles)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE i4_transport i4_warnings)
target_include_directories(test_transport PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_transport COMMAND test_transport)
set_tests_properties(test_transport PROPERTIES TIMEOUT 30)

add_executable(test_session test_session.cpp)
target_link_libraries(test_session PRIVATE i4_session i4_warnings)
target_include_directories(test_session PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME test_session COMMAND test_session)

# Compile-fail: each file in fail/ must compile as written and must NOT
# compile with -DI4_FAIL. The positive control stops a typo or a missing
# include from passing as "it failed to compile, as intended".
file(GLOB fail_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fail/*.cpp)
foreach(src IN LISTS fail_sources)
    get_filename_component(name ${src} NAME_WE)
    set(compile ${CMAKE_CXX_COMPILER} -std=c++26 -fsyntax-only -I${PROJECT_SOURCE_DIR}/include ${src})
    add_test(NAME compiles_${name} COMMAND ${compile})
    add_test(NAME refuses_${name} COMMAND ${compile} -DI4_FAIL)
    set_tests_properties(refuses_${name} PROPERTIES WILL_FAIL TRUE)
endforeach()

add_test(NAME layering
         COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/layering.cmake)

if(TARGET i4-preview)
    add_test(NAME preview_headless COMMAND i4-preview --headless)

    add_executable(test_head test_head.cpp ${PROJECT_SOURCE_DIR}/src/head/head.cpp)
    target_link_libraries(test_head PRIVATE i4_head i4_transport i4_warnings)
    target_include_directories(test_head PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME test_head COMMAND test_head)

    add_executable(test_launcher test_launcher.cpp ${PROJECT_SOURCE_DIR}/src/head/head.cpp)
    target_link_libraries(test_launcher PRIVATE i4_head i4_session i4_transport i4_warnings)
    target_include_directories(test_launcher PRIVATE ${PROJECT_SOURCE_DIR}/src)
    add_test(NAME test_launcher COMMAND test_launcher)
endif()

add_subdirectory(wl)
