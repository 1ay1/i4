#pragma once
// src/remote/tiles.hpp — frames for a head across a network.
//
// Lossless and incremental: a frame is cut into tiles, and only the tiles
// that differ from what this head already has travel, zstd-compressed (see
// link::down::Encoded). Text stays exactly as sharp as on the machine that
// drew it; a keystroke in a terminal costs a few hundred bytes; an idle
// window costs nothing.
//
// Pure byte-in/byte-out (no fds, no platform headers), so it builds for a
// head on any OS and tests/test_tiles.cpp checks it exactly.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "i4/link/protocol.hpp"

namespace i4::remote {

/// What the far side has for one window: its pixels, tight 32-bit BGRA.
/// The encoder keeps one per (head, window); the decoder is the canvas.
struct Canvas {
    std::int32_t width = 0, height = 0;
    std::vector<std::byte> pixels;
};

/// Where a frame is in a client's shared memory.
struct Source {
    std::span<const std::byte> memory;
    std::int32_t width, height, offset, stride;
    bool alpha;
};

/// The tiles of `src` that differ from `shadow`, encoded, with `shadow`
/// updated to match. A size change (or an empty shadow) is a keyframe.
/// nullopt: `src` does not fit its memory (a client lying about its pool).
[[nodiscard]] std::optional<link::down::Encoded> encode(Canvas& shadow, const Source& src, std::uint32_t tile = 64,
                                                        int level = 1);

/// Apply an update to `canvas` (sized width x height; a keyframe may resize
/// it). false: malformed (a tile out of range, sizes that do not add up),
/// and the canvas is left as it was. With `rgba`, the canvas holds straight
/// (un-premultiplied) R,G,B,A bytes, converted only for the tiles that
/// changed: what a renderer that takes RGBA uploads as it is.
[[nodiscard]] bool apply(Canvas& canvas, std::int32_t width, std::int32_t height, const link::down::Encoded& e,
                         bool rgba = false);

}  // namespace i4::remote
