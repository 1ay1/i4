#pragma once
// tests/check.hpp — the whole test framework.
//
// One test binary is one .cpp that includes this, which is why main() can
// live in a header. A TEST runs to completion; a failed CHECK records and
// carries on, so one run shows every broken expectation, not the first.

#include <cstdio>
#include <source_location>
#include <string>
#include <vector>

namespace i4::test {

struct Case {
    const char* name;
    void (*run)();
};

inline std::vector<Case>& cases() {
    static std::vector<Case> all;
    return all;
}

inline int failures = 0;

struct Register {
    Register(const char* name, void (*run)()) { cases().push_back({name, run}); }
};

inline void fail(const std::string& what, std::source_location at = std::source_location::current()) {
    std::printf("  FAIL %s:%u\n       %s\n", at.file_name(), static_cast<unsigned>(at.line()), what.c_str());
    ++failures;
}

template <class T>
std::string show(const T& v) {
    using std::to_string;
    if constexpr (requires { std::string(v); })
        return "\"" + std::string(v) + "\"";
    else if constexpr (requires { to_string(v); })
        return to_string(v);
    else
        return "(unprintable)";
}

}  // namespace i4::test

#define TEST(name)                                                 \
    static void name();                                            \
    static const ::i4::test::Register name##_registered{#name, &name}; \
    static void name()

#define CHECK(cond)                                  \
    do {                                             \
        if (!(cond)) ::i4::test::fail("CHECK(" #cond ")"); \
    } while (0)

// Copies, not references: `CHECK_EQ(f()->member, x)` would otherwise bind to
// a member of a temporary that is gone by the comparison.
#define CHECK_EQ(actual, expected)                                                                     \
    do {                                                                                               \
        const auto got_ = (actual);                                                                    \
        const auto want_ = (expected);                                                                 \
        if (!(got_ == want_))                                                                          \
            ::i4::test::fail(std::string(#actual " == " #expected "\n         got ") + ::i4::test::show(got_) + \
                             "\n    expected " + ::i4::test::show(want_));                            \
    } while (0)

int main() {
    int failed = 0;
    for (const auto& c : ::i4::test::cases()) {
        const int before = ::i4::test::failures;
        c.run();
        if (::i4::test::failures != before) {
            std::printf("FAIL %s\n", c.name);
            ++failed;
        }
    }
    std::printf("%zu cases, %d failed\n", ::i4::test::cases().size(), failed);
    return failed == 0 ? 0 : 1;
}
