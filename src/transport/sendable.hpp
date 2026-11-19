#pragma once
// src/transport/sendable.hpp — i4's value types, as jaal sees them.
//
// Every program in i4 (i4d's Session, i4-watch, the heads) is a jaal
// program, and their messages are made of i4's vocabulary. jaal proves a
// message Sendable by walking its fields; Id, Point, Size, Rect and Flags
// have constructors (so a raw integer cannot become an id by accident),
// which hides their fields from that walk. Each one is trivially copyable
// and owns nothing, which is exactly what Sendable means, so they opt in.
//
// Here, in the first layer that knows jaal, not in base/: the portable
// layers (base, wm, link) do not know jaal exists.

#include <type_traits>

#include <jaal/jaal.hpp>

#include "i4/base/flags.hpp"
#include "i4/base/geom.hpp"
#include "i4/base/id.hpp"

template <class Tag, class Rep>
inline constexpr bool jaal::sendable_opt_in<i4::Id<Tag, Rep>> = true;
template <class S>
inline constexpr bool jaal::sendable_opt_in<i4::Point<S>> = true;
template <class S>
inline constexpr bool jaal::sendable_opt_in<i4::Size<S>> = true;
template <class S>
inline constexpr bool jaal::sendable_opt_in<i4::Rect<S>> = true;
template <class E>
inline constexpr bool jaal::sendable_opt_in<i4::Flags<E>> = true;

static_assert(std::is_trivially_copyable_v<i4::Id<struct I4SendableProbe>> &&
              std::is_trivially_copyable_v<i4::Rect<i4::space::Desktop>>,
              "an opted-in type stopped being a plain value");
