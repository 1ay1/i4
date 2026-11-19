#pragma once
// i4/base/enum.hpp — an enum's range, derived rather than declared.
//
//   enum class Layout { splith, splitv, tabbed, stacking };
//   static_assert(enum_count<Layout>() == 4);
//
// The wire codec has to reject an enumerator that does not exist (a decoded
// Layout of 9 would walk straight past every switch), and the schema
// fingerprint has to change when an enumerator is added. Both need the
// enum's size, and a hand-maintained count is a count that goes stale.
//
// So it is read off the compiler: a function template's signature names its
// enum argument (`E = Layout::tabbed`) when the value is an enumerator, and
// spells a cast (`E = (Layout)4`) when it is not. Counting from 0 until the
// first cast is the number of enumerators. GCC and Clang print the same
// shape; tests/fail and the static_asserts below hold them to it.
//
// The rules this imposes, and checks: an enum that crosses the wire is
// scoped (so it has a fixed underlying type, and every value of that type is
// a valid constant to probe with) and dense from zero (no `= 7`), which is
// what every i4 enum is.

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

namespace i4 {

namespace detail::enums {

template <auto V>
consteval std::string_view signature() {
    return __PRETTY_FUNCTION__;
}

/// The enumerator is named, not cast: the text after "V = " does not start
/// with '('.
template <class E, std::size_t I>
consteval bool named() {
    constexpr auto s = signature<static_cast<E>(I)>();
    constexpr auto at = s.find("V = ");
    static_assert(at != std::string_view::npos, "i4: unrecognised __PRETTY_FUNCTION__ shape");
    return s[at + 4] != '(';
}

template <class E, std::size_t... I>
consteval std::size_t count(std::index_sequence<I...>) {
    std::size_t n = 0;
    bool open = true;
    ((open = open && named<E, I>(), n += open ? 1 : 0), ...);
    return n;
}

}  // namespace detail::enums

/// Probe this far. An enum on the wire with more enumerators than this is a
/// compile error (enum_count would come out equal to the bound).
inline constexpr std::size_t enum_probe_limit = 64;

template <class E>
concept wire_enum = std::is_scoped_enum_v<E>;

template <wire_enum E>
consteval std::size_t enum_count() {
    constexpr auto n = detail::enums::count<E>(std::make_index_sequence<enum_probe_limit>{});
    static_assert(n > 0, "i4: an enum on the wire must have an enumerator at 0");
    static_assert(n < enum_probe_limit, "i4: enum too large to probe");
    return n;
}

template <wire_enum E>
constexpr bool enum_valid(std::underlying_type_t<E> raw) noexcept {
    using U = std::underlying_type_t<E>;
    if constexpr (std::is_signed_v<U>)
        if (raw < 0) return false;
    return static_cast<std::size_t>(raw) < enum_count<E>();
}

namespace detail::enums {
enum class Probe : unsigned char { a, b, c };
enum class Wide : int { x, y };
static_assert(enum_count<Probe>() == 3);
static_assert(enum_count<Wide>() == 2);
static_assert(enum_valid<Wide>(1) && !enum_valid<Wide>(2) && !enum_valid<Wide>(-1));
}  // namespace detail::enums

}  // namespace i4
