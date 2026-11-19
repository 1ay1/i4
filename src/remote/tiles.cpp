// src/remote/tiles.cpp — tile diff + zstd.
#include "tiles.hpp"

#include <algorithm>
#include <cstring>

#include <zstd.h>

namespace i4::remote {

namespace {

struct Grid {
    std::int32_t width, height;
    std::uint32_t tile;
    std::uint32_t cols, rows;
    Grid(std::int32_t w, std::int32_t h, std::uint32_t t)
        : width(w), height(h), tile(t), cols((static_cast<std::uint32_t>(w) + t - 1) / t),
          rows((static_cast<std::uint32_t>(h) + t - 1) / t) {}
    [[nodiscard]] std::uint32_t count() const { return cols * rows; }
    /// Tile `i` as x, y, w, h in pixels.
    struct Rect {
        std::int32_t x, y, w, h;
    };
    [[nodiscard]] Rect rect(std::uint32_t i) const {
        const auto tx = static_cast<std::int32_t>((i % cols) * tile), ty = static_cast<std::int32_t>((i / cols) * tile);
        return {tx, ty, std::min<std::int32_t>(static_cast<std::int32_t>(tile), width - tx),
                std::min<std::int32_t>(static_cast<std::int32_t>(tile), height - ty)};
    }
};

}  // namespace

std::optional<link::down::Encoded> encode(Canvas& shadow, const Source& s, std::uint32_t tile, int level) {
    if (s.width <= 0 || s.height <= 0 || s.offset < 0 || s.stride < s.width * 4) return std::nullopt;
    const auto end = static_cast<std::uint64_t>(s.offset) +
                     static_cast<std::uint64_t>(s.stride) * static_cast<std::uint64_t>(s.height - 1) +
                     static_cast<std::uint64_t>(s.width) * 4;
    if (end > s.memory.size()) return std::nullopt;

    link::down::Encoded e;
    e.tile = tile;
    e.alpha = s.alpha;
    e.keyframe = shadow.width != s.width || shadow.height != s.height;
    if (e.keyframe) {
        shadow.width = s.width;
        shadow.height = s.height;
        shadow.pixels.assign(static_cast<std::size_t>(s.width) * static_cast<std::size_t>(s.height) * 4, std::byte{});
    }
    const Grid g{s.width, s.height, tile};
    const auto pitch = static_cast<std::size_t>(s.width) * 4;
    std::vector<std::byte> raw;
    for (std::uint32_t i = 0; i < g.count(); ++i) {
        const auto r = g.rect(i);
        const auto row_bytes = static_cast<std::size_t>(r.w) * 4;
        const auto at_src = [&](std::int32_t y) {
            return s.memory.data() + s.offset + static_cast<std::size_t>(r.y + y) * static_cast<std::size_t>(s.stride) +
                   static_cast<std::size_t>(r.x) * 4;
        };
        const auto at_dst = [&](std::int32_t y) {
            return shadow.pixels.data() + static_cast<std::size_t>(r.y + y) * pitch + static_cast<std::size_t>(r.x) * 4;
        };
        bool changed = e.keyframe;
        for (std::int32_t y = 0; !changed && y < r.h; ++y) changed = std::memcmp(at_src(y), at_dst(y), row_bytes) != 0;
        if (!changed) continue;
        e.tiles.push_back(i);
        for (std::int32_t y = 0; y < r.h; ++y) {
            std::memcpy(at_dst(y), at_src(y), row_bytes);
            raw.insert(raw.end(), at_src(y), at_src(y) + row_bytes);
        }
    }
    e.raw_size = static_cast<std::uint32_t>(raw.size());
    if (!raw.empty()) {
        // One context per thread, reused: no allocation per frame.
        thread_local ZSTD_CCtx* cctx = ZSTD_createCCtx();
        e.data.resize(ZSTD_compressBound(raw.size()));
        const auto n = ZSTD_compressCCtx(cctx, e.data.data(), e.data.size(), raw.data(), raw.size(), level);
        if (ZSTD_isError(n)) return std::nullopt;
        e.data.resize(n);
    }
    return e;
}

bool apply(Canvas& c, std::int32_t width, std::int32_t height, const link::down::Encoded& e, bool rgba) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || !e.valid()) return false;
    const Grid g{width, height, e.tile};
    // Every listed tile exists, in rising order, and their sizes add up.
    std::uint64_t want = 0;
    for (std::size_t k = 0; k < e.tiles.size(); ++k) {
        if (e.tiles[k] >= g.count() || (k > 0 && e.tiles[k] <= e.tiles[k - 1])) return false;
        const auto r = g.rect(e.tiles[k]);
        want += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h) * 4;
    }
    if (want != e.raw_size) return false;
    if (e.keyframe && e.tiles.size() != g.count()) return false;
    if (!e.keyframe && (c.width != width || c.height != height)) return false;  // a delta needs its base

    std::vector<std::byte> raw(e.raw_size);
    if (e.raw_size != 0) {
        const auto n = ZSTD_decompress(raw.data(), raw.size(), e.data.data(), e.data.size());
        if (ZSTD_isError(n) || n != raw.size()) return false;
    }
    if (e.keyframe) {
        c.width = width;
        c.height = height;
        c.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    }
    const auto pitch = static_cast<std::size_t>(width) * 4;
    std::size_t at = 0;
    for (auto i : e.tiles) {
        const auto r = g.rect(i);
        const auto row_bytes = static_cast<std::size_t>(r.w) * 4;
        for (std::int32_t y = 0; y < r.h; ++y) {
            auto* dst = c.pixels.data() + static_cast<std::size_t>(r.y + y) * pitch + static_cast<std::size_t>(r.x) * 4;
            const auto* src = raw.data() + at;
            if (!rgba) {
                std::memcpy(dst, src, row_bytes);
            } else {
                // Wayland's little-endian ARGB (premultiplied) -> straight RGBA.
                auto* o = reinterpret_cast<std::uint8_t*>(dst);
                const auto* in = reinterpret_cast<const std::uint8_t*>(src);
                for (std::int32_t x = 0; x < r.w; ++x, o += 4, in += 4) {
                    const std::uint8_t a = e.alpha ? in[3] : 255;
                    if (a == 255 || a == 0) {
                        o[0] = in[2], o[1] = in[1], o[2] = in[0];
                    } else {
                        const auto un = [a](std::uint8_t v) {
                            return static_cast<std::uint8_t>(std::min(255u, (v * 255u + a / 2u) / a));
                        };
                        o[0] = un(in[2]), o[1] = un(in[1]), o[2] = un(in[0]);
                    }
                    o[3] = a;
                }
            }
            at += row_bytes;
        }
    }
    return true;
}

}  // namespace i4::remote
