#pragma once
// i4/base/flags.hpp — a set of enumerators, as a type.
//
//   enum class Mod : std::uint8_t { shift, ctrl, alt, super };
//   Flags<Mod> m = Mod::super | Mod::shift;
//   m.has(Mod::ctrl)                 // false
//   Flags<Layout> x = Mod::shift;    // COMPILE ERROR: wrong enum
//
// A raw bitmask (`uint8_t mods = 0x41`) says nothing about which bits mean
// what, and accepts any number. Flags<E> holds only E's enumerators: its
// constructor takes an E, its width comes from enum_count<E>, and the bits
// above the last enumerator are never set (so == is set equality).

#include <cstdint>
#include <initializer_list>
#include <type_traits>

#include "i4/base/enum.hpp"

namespace i4 {

template <wire_enum E>
class Flags {
    static_assert(enum_count<E>() <= 32, "Flags holds at most 32 enumerators");

public:
    using enum_type = E;

    constexpr Flags() = default;
    constexpr Flags(E e) noexcept : bits_(bit(e)) {}  // NOLINT: implicit on purpose
    constexpr Flags(std::initializer_list<E> es) noexcept {
        for (E e : es) bits_ |= bit(e);
    }

    [[nodiscard]] constexpr bool has(E e) const noexcept { return (bits_ & bit(e)) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr std::uint32_t raw() const noexcept { return bits_; }

    constexpr Flags& operator|=(Flags o) noexcept {
        bits_ |= o.bits_;
        return *this;
    }
    [[nodiscard]] friend constexpr Flags operator|(Flags a, Flags b) noexcept { return a |= b; }
    [[nodiscard]] friend constexpr Flags operator|(E a, Flags b) noexcept { return Flags(a) | b; }

    friend constexpr bool operator==(Flags, Flags) = default;
    friend constexpr auto operator<=>(Flags, Flags) = default;

    // The wire codec reads this struct's one member; a decoded value with
    // bits past the last enumerator is refused by valid().
    [[nodiscard]] constexpr bool valid() const noexcept { return (bits_ >> enum_count<E>()) == 0; }

    std::uint32_t bits_ = 0;  // public only so the structural codec can see it

private:
    static constexpr std::uint32_t bit(E e) noexcept { return std::uint32_t{1} << static_cast<unsigned>(e); }
};

template <wire_enum E>
[[nodiscard]] constexpr Flags<E> operator|(E a, E b) noexcept {
    return Flags<E>(a) | Flags<E>(b);
}

}  // namespace i4
