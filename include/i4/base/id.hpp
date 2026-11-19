#pragma once
// i4/base/id.hpp — identities that cannot be confused with each other.
//
//   using WindowId = Id<struct WindowTag>;
//   using ConId    = Id<struct ConTag>;
//
//   void close(WindowId);
//   close(some_con_id);        // COMPILE ERROR: a ConId is not a WindowId
//
// An Id is a value: copyable, ordered, hashable, 4 bytes. It is ordered so
// it can key an ordered map, and i4 uses ordered maps everywhere state is
// iterated, because iteration order is part of what makes the window manager
// deterministic (the same events produce the same state on the server and on
// a head predicting it — see docs/ARCHITECTURE.md).
//
// There is no "null" Id. Absence is std::optional<Id>, so a function that can
// fail to find something says so in its type.

#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>

namespace i4 {

/// `Rep` is the representation: 32 bits for things that come and go, 64 for
/// positions in a log that only grows.
template <class Tag, std::unsigned_integral Rep = std::uint32_t>
struct Id {
    using tag = Tag;
    using rep = Rep;

    Rep value = 0;

    constexpr Id() = default;
    constexpr explicit Id(Rep v) noexcept : value(v) {}

    /// The one after this: ids are handed out in order.
    [[nodiscard]] constexpr Id next() const noexcept { return Id{static_cast<Rep>(value + 1)}; }

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

static_assert(sizeof(Id<struct A>) == 4);
static_assert(sizeof(Id<struct A, std::uint64_t>) == 8);
static_assert(std::is_trivially_copyable_v<Id<struct A>>);
static_assert(!std::is_convertible_v<Id<struct A>, Id<struct B>>);
static_assert(!std::is_convertible_v<std::uint32_t, Id<struct A>>);

}  // namespace i4

template <class Tag, class Rep>
struct std::hash<i4::Id<Tag, Rep>> {
    std::size_t operator()(i4::Id<Tag, Rep> id) const noexcept { return std::hash<Rep>{}(id.value); }
};
