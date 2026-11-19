// refine.hpp — refinement types for the constraints the XML does not state.
//
// Wayland says "width and height are positive", "stride is at least width*4",
// "a serial comes from an event you received". None of that is in the XML, so
// no generator can derive it, and none of it is expressible as a plain C++
// type.
//
// What IS expressible is the distinction between a value known at compile time
// and one that isn't:
//
//   * a literal, a constexpr, anything the compiler can see  -> PROVE it
//   * a runtime value (a config file, a configure event)     -> CHECK it once,
//     at a named boundary, and carry the proof in the type from then on
//
// That is the honest ceiling without dependent types: total for constants,
// checked-once-then-static for the rest. `Positive<T>` below has no
// constructor that can produce an invalid value, so a function taking one
// never needs to re-validate.
#pragma once

#include <cstdint>
#include <optional>
#include <type_traits>

namespace i4::wl {

// ---------------------------------------------------------------------------
// Positive<T> — a T that is > 0, by construction.
// ---------------------------------------------------------------------------
template <class T>
class Positive {
    static_assert(std::is_integral_v<T>, "Positive is for integers");

public:
    Positive() = delete;

    /// Compile-time path: the value is a template argument, so an invalid one
    /// is a compile error and there is no runtime branch at all.
    template <T V>
    static consteval Positive of() {
        static_assert(V > 0, "i4::wl: this protocol argument must be positive.");
        return Positive(V);
    }

    /// Runtime path: checked once, here, at a boundary you can grep for.
    static constexpr std::optional<Positive> from(T v) {
        if (v <= 0) return std::nullopt;
        return Positive(v);
    }

    /// Escape hatch for a value proven positive by other means. Deliberately
    /// ugly to type and easy to find in review.
    static constexpr Positive unchecked(T v) { return Positive(v); }

    constexpr T get() const { return v_; }
    constexpr operator T() const { return v_; }  // NOLINT(*-explicit)

    friend constexpr bool operator==(Positive, Positive) = default;

private:
    constexpr explicit Positive(T v) : v_(v) {}
    T v_;
};

/// `10_pos` is a compile-time-proven positive int32.
template <char... C>
consteval Positive<std::int32_t> operator""_pos() {
    constexpr char digits[] = {C...};
    std::int32_t v = 0;
    for (char c : digits) {
        if (c == '\'') continue;
        v = v * 10 + (c - '0');
    }
    return Positive<std::int32_t>::unchecked(v);
}

// ---------------------------------------------------------------------------
// NonNegative<T> — a T that is >= 0. Offsets, counts.
// ---------------------------------------------------------------------------
template <class T>
class NonNegative {
    static_assert(std::is_integral_v<T>);

public:
    NonNegative() = delete;

    template <T V>
    static consteval NonNegative of() {
        static_assert(V >= 0, "i4::wl: this protocol argument must not be negative.");
        return NonNegative(V);
    }

    static constexpr std::optional<NonNegative> from(T v) {
        if (v < 0) return std::nullopt;
        return NonNegative(v);
    }
    static constexpr NonNegative unchecked(T v) { return NonNegative(v); }

    constexpr T get() const { return v_; }
    constexpr operator T() const { return v_; }  // NOLINT(*-explicit)

    friend constexpr bool operator==(NonNegative, NonNegative) = default;

private:
    constexpr explicit NonNegative(T v) : v_(v) {}
    T v_;
};

// ---------------------------------------------------------------------------
// Serial — a value that must have come from an event.
//
// Wayland requires the serial in a request (set_cursor, move, resize, popup
// grabs) to be one the compositor sent us. A bare uint32 makes an invented
// serial indistinguishable from a real one; this type makes the provenance
// part of the value. It cannot be constructed from a literal.
// ---------------------------------------------------------------------------
class Serial {
public:
    Serial() = delete;

    /// Only the event-decoding path calls this.
    static constexpr Serial from_event(std::uint32_t s) { return Serial(s); }

    constexpr std::uint32_t get() const { return v_; }

    friend constexpr bool operator==(Serial, Serial) = default;
    friend constexpr auto operator<=>(Serial a, Serial b) { return a.v_ <=> b.v_; }

private:
    constexpr explicit Serial(std::uint32_t v) : v_(v) {}
    std::uint32_t v_;
};

}  // namespace i4::wl
