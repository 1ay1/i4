#pragma once
// src/head/pixels.hpp — a client's frame, as an image mayag can draw.
//
// Wayland shm (argb8888 / xrgb8888) is 32-bit little-endian words: bytes
// B, G, R, A in memory, alpha PREMULTIPLIED. mayag's ImageData is straight
// (un-premultiplied) R, G, B, A bytes. So every pixel is swizzled, and for
// argb8888 un-premultiplied; for xrgb8888 the fourth byte is padding and
// alpha is 255.
//
// This is the one copy on the local path, and it exists only because mayag
// takes pixels as owned bytes today. When mayag can import foreign memory
// (third_party/mayag/docs/compositor-substrate.md) the head hands it the
// mapping instead, and this file goes.
//
// Pure (bytes in, bytes out), so tests/test_pixels.cpp checks it exactly.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <mayag/mayag.hpp>

namespace i4::head {

struct Layout {
    std::int32_t width, height, offset, stride;
    bool alpha;
};

/// The frame `layout` describes inside `memory`, or nullopt when it does not
/// fit (a client lying about its pool must not make the head read past it).
[[nodiscard]] std::optional<mayag::ImageData> convert(std::span<const std::byte> memory, Layout layout);

}  // namespace i4::head
