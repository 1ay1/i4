#pragma once
// i4/base/geom.hpp — coordinate spaces as types.
//
// A compositor juggles several coordinate systems at once, and they are all
// pairs of integers, so C++ will happily pass one where another belongs.
// That is behind a large share of compositor bugs: a window drawn at the
// wrong offset, damage tracked in the wrong space, hit-testing that works on
// one monitor and not the second. So the space is part of the type:
//
//   Point<space::Surface> p{10, 20};
//   Point<space::Desktop> q = p;     // COMPILE ERROR: different spaces
//
// Spaces in i4 (namespace i4::space, so the tags never collide with the
// things they measure — wm::Output is an output, space::Output its pixels):
//
//   Desktop   logical pixels, all outputs arranged in one plane. This is the
//             space the window manager thinks in, and the space the
//             server<->head protocol speaks.
//   Output    logical pixels, origin at one output's top-left.
//   Surface   logical pixels, origin at a client surface's top-left.
//   Buffer    the client's buffer pixels (scaled and/or rotated).
//   Device    an output's physical pixels — what KMS scans out.
//
// Logical vs physical is the other classic bug (fractional scaling), which is
// why Device and Buffer are separate spaces rather than "Output times scale".
//
// Lifted from weft (1ay1/weft, space.hpp): Point, Size and Rect. weft's
// Transform is deliberately NOT lifted: it is integer-scale only, and every
// modern output needs fractional scale. The head layer will define its own.

#include <algorithm>
#include <compare>
#include <cstdint>
#include <string>
#include <type_traits>

namespace i4 {

namespace space {
struct Desktop {};
struct Output {};
struct Surface {};
struct Buffer {};
struct Device {};
}  // namespace space

template <class Space>
struct Point {
    using space = Space;

    std::int32_t x = 0;
    std::int32_t y = 0;

    constexpr Point() = default;
    constexpr Point(std::int32_t x_, std::int32_t y_) noexcept : x(x_), y(y_) {}

    friend constexpr bool operator==(Point, Point) = default;
    friend constexpr auto operator<=>(Point, Point) = default;

    constexpr Point operator+(Point o) const noexcept { return {x + o.x, y + o.y}; }
    constexpr Point operator-(Point o) const noexcept { return {x - o.x, y - o.y}; }
    constexpr Point operator-() const noexcept { return {-x, -y}; }
};

/// Deliberately not a Point: adding two sizes means something, adding two
/// positions does not.
template <class Space>
struct Size {
    using space = Space;

    std::int32_t width = 0;
    std::int32_t height = 0;

    constexpr Size() = default;
    constexpr Size(std::int32_t w, std::int32_t h) noexcept : width(w), height(h) {}

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }
    [[nodiscard]] constexpr std::int64_t area() const noexcept {
        return empty() ? 0 : static_cast<std::int64_t>(width) * height;
    }

    friend constexpr bool operator==(Size, Size) = default;
};

/// Origin + size, the way every protocol and kernel API says it; converting
/// to and from corners is where off-by-ones breed.
template <class Space>
struct Rect {
    using space = Space;

    Point<Space> origin;
    Size<Space> size;

    constexpr Rect() = default;
    constexpr Rect(Point<Space> o, Size<Space> s) noexcept : origin(o), size(s) {}
    constexpr Rect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) noexcept
        : origin(x, y), size(w, h) {}

    [[nodiscard]] constexpr std::int32_t left() const noexcept { return origin.x; }
    [[nodiscard]] constexpr std::int32_t top() const noexcept { return origin.y; }
    [[nodiscard]] constexpr std::int32_t right() const noexcept { return origin.x + size.width; }
    [[nodiscard]] constexpr std::int32_t bottom() const noexcept { return origin.y + size.height; }
    [[nodiscard]] constexpr Point<Space> center() const noexcept {
        return {origin.x + size.width / 2, origin.y + size.height / 2};
    }

    [[nodiscard]] constexpr bool empty() const noexcept { return size.empty(); }

    [[nodiscard]] constexpr bool contains(Point<Space> p) const noexcept {
        return p.x >= left() && p.x < right() && p.y >= top() && p.y < bottom();
    }

    [[nodiscard]] constexpr bool intersects(Rect o) const noexcept {
        return !empty() && !o.empty() && left() < o.right() && o.left() < right() &&
               top() < o.bottom() && o.top() < bottom();
    }

    [[nodiscard]] constexpr Rect intersected(Rect o) const noexcept {
        const auto l = std::max(left(), o.left());
        const auto t = std::max(top(), o.top());
        const auto r = std::min(right(), o.right());
        const auto b = std::min(bottom(), o.bottom());
        if (r <= l || b <= t) return {};
        return {Point<Space>{l, t}, Size<Space>{r - l, b - t}};
    }

    [[nodiscard]] constexpr Rect translated(Point<Space> d) const noexcept { return {origin + d, size}; }

    /// Shrink on every side (grow with a negative value). Never yields a
    /// negative size: an over-inset rect is empty at its center.
    [[nodiscard]] constexpr Rect inset(std::int32_t by) const noexcept {
        const auto w = std::max(0, size.width - 2 * by);
        const auto h = std::max(0, size.height - 2 * by);
        return {Point<Space>{origin.x + (size.width - w) / 2, origin.y + (size.height - h) / 2},
                Size<Space>{w, h}};
    }

    friend constexpr bool operator==(Rect, Rect) = default;
};

/// For logs and test failures: "{x,y wxh}".
template <class Space>
[[nodiscard]] std::string to_string(Rect<Space> r) {
    return "{" + std::to_string(r.origin.x) + "," + std::to_string(r.origin.y) + " " +
           std::to_string(r.size.width) + "x" + std::to_string(r.size.height) + "}";
}

static_assert(sizeof(Point<space::Desktop>) == 8);
static_assert(sizeof(Rect<space::Desktop>) == 16);
static_assert(std::is_trivially_copyable_v<Rect<space::Desktop>>);
static_assert(!std::is_convertible_v<Point<space::Surface>, Point<space::Desktop>>);
static_assert(!std::is_convertible_v<Rect<space::Buffer>, Rect<space::Device>>);

}  // namespace i4
