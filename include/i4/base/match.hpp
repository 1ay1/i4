#pragma once
// i4/base/match.hpp — exhaustive pattern matching on sum types.
//
//   match(event,
//       [](const WindowMapped& e)   { ... },
//       [](const WindowUnmapped& e) { ... });
//
// Exhaustive by construction: if the variant gains an alternative and no
// lambda accepts it, std::visit fails to compile. Never add a catch-all
// `[](const auto&)` to silence that — the error is the point.

#include <utility>
#include <variant>

namespace i4 {

template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

template <class Variant, class... Fs>
constexpr decltype(auto) match(Variant&& v, Fs&&... fs) {
    return std::visit(overloaded{std::forward<Fs>(fs)...}, std::forward<Variant>(v));
}

/// `is<T>(v)`: the variant currently holds a T.
template <class T, class... Ts>
[[nodiscard]] constexpr bool is(const std::variant<Ts...>& v) noexcept {
    return std::holds_alternative<T>(v);
}

}  // namespace i4
