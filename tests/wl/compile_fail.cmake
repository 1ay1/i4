# compile_fail.cmake — every case below MUST fail to compile.
#
# A library that sells compile-time safety has to prove the errors actually
# happen. Each fragment is a real misuse; if one of them ever compiles, a
# guarantee has silently rotted and this test fails.
#
# Bodies live in tests/fail/*.cpp rather than in CMake strings, because CMake's
# `;` escaping silently corrupts inlined C++ and turns this suite into a test
# that only proves the compiler rejects backslashes.

set(CASES
  # --- data: the Π/Σ algebra over arguments ---
  version_too_new
  wrong_enum_type
  wrong_object_type
  too_few_args
  too_many_args
  foreign_message
  bind_above_max
  upgrade_version
  wrong_handler_args
  foreign_handler
  illegal_null
  server_new_id_as_ref
  # --- resources: affine objects, linear descriptors ---
  raw_fd
  copy_fd
  destroy_lvalue
  copy_unique
  # --- time: protocol state ---
  attach_before_configure
  # --- refinement: values the XML does not constrain ---
  negative_size
  forged_serial
  # --- direction: roles are dual, not interchangeable ---
  client_sends_event)

# blocking_under_runtime (loom's) is gone: i4::wl has no jaal glue of its
# own, the server is driven by i4d's host and nothing below it waits.

set(failed 0)
set(passed "")

# A few cases are rejected by a WARNING promoted to an error (a deprecated
# blocking call under a runtime), not by a type error, so they need -Werror.
# They also include <jaal/jaal.hpp>, which requires C++26 — at C++23 the case
# would "fail" on jaal's own #error instead of on the misuse under test,
# which proves nothing.
#set(NEEDS_WERROR blocking_under_runtime)
#set(NEEDS_CXX26  blocking_under_runtime)

foreach(case ${CASES})
  set(src "${SRC_DIR}/wl/fail/${case}.cpp")
  if(NOT EXISTS "${src}")
    message(SEND_ERROR "missing case file: ${src}")
    set(failed 1)
    continue()
  endif()

  set(extra "")
  set(std -std=c++26)
  if(case IN_LIST NEEDS_WERROR)
    set(extra -Werror)
  endif()
  if(case IN_LIST NEEDS_CXX26)
    set(std -std=c++2c)
  endif()

  execute_process(
    COMMAND ${CXX} ${std} -fsyntax-only ${extra}
            -I${ROOT_INCLUDE} -I${BIN_DIR}/generated -I${JAAL_INCLUDE} "${src}"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE errout)

  # A stray-character or missing-header error would make any file "fail", which
  # would make this suite vacuously green. Demand a real semantic rejection.
  string(REGEX MATCH "stray|No such file|expected [^ ]+ before" bogus "${errout}")
  if(bogus)
    message(SEND_ERROR "  ${case}: rejected for the WRONG reason (${bogus})")
    set(failed 1)
  elseif(rc EQUAL 0)
    message(SEND_ERROR "  ${case}: COMPILED but should not have")
    set(failed 1)
  else()
    string(FIND "${errout}" "i4::wl:" has_ours)
    if(has_ours GREATER -1)
      message(STATUS "  ${case}: rejected, with an i4::wl diagnostic")
    else()
      message(STATUS "  ${case}: rejected")
    endif()
    list(APPEND passed ${case})
  endif()
endforeach()

if(failed)
  message(FATAL_ERROR "compile-fail suite: a misuse was accepted or misdiagnosed")
endif()

list(LENGTH passed n)
message(STATUS "compile-fail suite: ${n} misuses correctly rejected")
