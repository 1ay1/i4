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
#include <cstdint>
#include <functional>
#include <type_traits>

namespace i4 {

template <class Tag>
struct Id {
    using tag = Tag;

    std::uint32_t value = 0;

    constexpr Id() = default;
    constexpr explicit Id(std::uint32_t v) noexcept : value(v) {}

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

static_assert(sizeof(Id<struct A>) == 4);
static_assert(std::is_trivially_copyable_v<Id<struct A>>);
static_assert(!std::is_convertible_v<Id<struct A>, Id<struct B>>);
static_assert(!std::is_convertible_v<std::uint32_t, Id<struct A>>);

}  // namespace i4

template <class Tag>
struct std::hash<i4::Id<Tag>> {
    std::size_t operator()(i4::Id<Tag> id) const noexcept { return std::hash<std::uint32_t>{}(id.value); }
};
