#pragma once
// i4/base/expect.hpp — preconditions that are bugs, not errors.
//
// Two kinds of failure, kept apart:
//
//   * Something the outside world can cause (a bad command, a client asking
//     for a window that is gone): that is a value, std::expected, and the
//     caller decides.
//   * Something only a bug in i4 can cause (a container whose parent does
//     not list it): that is `expect`, and it stops the program loudly with
//     the place it happened. Continuing past a broken invariant would only
//     move the crash somewhere less informative.

#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace i4 {

[[noreturn]] inline void broken(const char* what, std::source_location at) noexcept {
    std::fprintf(stderr, "i4: invariant broken: %s\n  at %s:%u (%s)\n", what, at.file_name(),
                 static_cast<unsigned>(at.line()), at.function_name());
    std::abort();
}

constexpr void expect(bool ok, const char* what,
                      std::source_location at = std::source_location::current()) noexcept {
    if (!ok) [[unlikely]]
        broken(what, at);
}

}  // namespace i4
