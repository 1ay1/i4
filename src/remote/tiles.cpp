// src/remote/tiles.cpp — tile diff + zstd.
#include "tiles.hpp"

#include <algorithm>
#include <cstring>
#include <map>

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

namespace {

std::uint64_t hash_row(const std::byte* p, std::size_t n) {
    // FNV-1a over 8-byte words (a row is whole pixels: n is a multiple of 4).
    std::uint64_t h = 1469598103934665603ull;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; ++i) h = (h ^ std::to_integer<std::uint8_t>(p[i])) * 1099511628211ull;
    return h;
}

}  // namespace

std::optional<link::down::Move> find_move(const std::vector<std::uint64_t>& now, const std::vector<std::uint64_t>& was,
                                          std::int32_t width) {
    const auto h = static_cast<std::int32_t>(now.size());
    if (h < 16 || was.size() != now.size()) return std::nullopt;
    int changed = 0;
    for (std::int32_t y = 0; y < h; ++y) changed += now[static_cast<std::size_t>(y)] != was[static_cast<std::size_t>(y)];
    if (changed < 64 && changed * 4 < h) return std::nullopt;  // not enough moved to be a scroll
    std::map<std::uint64_t, std::int32_t> where;  // a row of the last frame, if unique
    for (std::int32_t y = 0; y < h; ++y) {
        auto [it, fresh] = where.try_emplace(was[static_cast<std::size_t>(y)], y);
        if (!fresh) it->second = -1;
    }
    std::map<std::int32_t, int> votes;
    for (std::int32_t y = 0; y < h; ++y) {
        const auto r = now[static_cast<std::size_t>(y)];
        if (r == was[static_cast<std::size_t>(y)]) continue;
        if (auto it = where.find(r); it != where.end() && it->second >= 0 && it->second != y) ++votes[it->second - y];
    }
    std::int32_t dy = 0;
    int best = 0;
    for (const auto& [d, n] : votes)
        if (n > best) best = n, dy = d;
    if (best < 8 || best * 3 < changed) return std::nullopt;
    std::int32_t run0 = -1, best0 = 0, best1 = 0, last = -1;
    for (std::int32_t y = 0; y < h; ++y) {
        const auto from = y + dy;
        if (from < 0 || from >= h || now[static_cast<std::size_t>(y)] != was[static_cast<std::size_t>(from)]) continue;
        if (run0 < 0 || y - last > 4) run0 = y;
        last = y;
        if (y + 1 - run0 > best1 - best0) best0 = run0, best1 = y + 1;
    }
    if (best1 - best0 < 8) return std::nullopt;
    return link::down::Move{0, best0, width, best1 - best0, dy};
}

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
    // A scroll (a terminal printing, a page moving): rows of the shadow that
    // reappear shifted. The head moves its copy the same way; the diff is
    // then against the moved shadow, so only revealed rows are new.
    if (!e.keyframe) {
        std::vector<std::uint64_t> now(static_cast<std::size_t>(s.height)), was(static_cast<std::size_t>(s.height));
        for (std::int32_t y = 0; y < s.height; ++y) {
            now[static_cast<std::size_t>(y)] =
                hash_row(s.memory.data() + s.offset + static_cast<std::size_t>(y) * static_cast<std::size_t>(s.stride), pitch);
            was[static_cast<std::size_t>(y)] = hash_row(shadow.pixels.data() + static_cast<std::size_t>(y) * pitch, pitch);
        }
        if (auto m = find_move(now, was, s.width)) {
            e.scroll = m;
            auto row = [&](std::int32_t y) { return shadow.pixels.data() + static_cast<std::size_t>(y) * pitch; };
            if (m->dy > 0)
                for (std::int32_t y = m->y; y < m->y + m->h; ++y) std::memmove(row(y), row(y + m->dy), pitch);
            else
                for (std::int32_t y = m->y + m->h - 1; y >= m->y; --y) std::memmove(row(y), row(y + m->dy), pitch);
        }
    }
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

namespace {

/// Checks an update against a width x height grid and decompresses it.
std::optional<std::vector<std::byte>> unpack(std::int32_t width, std::int32_t height, const link::down::Encoded& e) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 || !e.valid()) return std::nullopt;
    const Grid g{width, height, e.tile};
    // Every listed tile exists, in rising order, and their sizes add up.
    std::uint64_t want = 0;
    for (std::size_t k = 0; k < e.tiles.size(); ++k) {
        if (e.tiles[k] >= g.count() || (k > 0 && e.tiles[k] <= e.tiles[k - 1])) return std::nullopt;
        const auto r = g.rect(e.tiles[k]);
        want += static_cast<std::uint64_t>(r.w) * static_cast<std::uint64_t>(r.h) * 4;
    }
    if (want != e.raw_size) return std::nullopt;
    if (e.keyframe && e.tiles.size() != g.count()) return std::nullopt;
    std::vector<std::byte> raw(e.raw_size);
    if (e.raw_size != 0) {
        const auto n = ZSTD_decompress(raw.data(), raw.size(), e.data.data(), e.data.size());
        if (ZSTD_isError(n) || n != raw.size()) return std::nullopt;
    }
    return raw;
}

/// Little-endian premultiplied ARGB words -> straight RGBA bytes.
void to_rgba(std::uint8_t* o, const std::uint8_t* in, std::int32_t n, bool alpha) {
    for (std::int32_t x = 0; x < n; ++x, o += 4, in += 4) {
        const std::uint8_t a = alpha ? in[3] : 255;
        if (a == 255 || a == 0) {
            o[0] = in[2], o[1] = in[1], o[2] = in[0];
        } else {
            const auto un = [a](std::uint8_t v) { return static_cast<std::uint8_t>(std::min(255u, (v * 255u + a / 2u) / a)); };
            o[0] = un(in[2]), o[1] = un(in[1]), o[2] = un(in[0]);
        }
        o[3] = a;
    }
}

/// Rows of `px` (width x height, 4 bytes a pixel) moved as `m` says, in
/// place: overlap-safe (rows walk away from where they read). false: the
/// move does not fit the picture.
bool move_rows(std::uint8_t* px, std::int32_t width, std::int32_t height, const link::down::Move& m) {
    if (m.w <= 0 || m.h <= 0 || m.x < 0 || m.y < 0 || m.x + m.w > width || m.y + m.h > height) return false;
    if (m.y + m.dy < 0 || m.y + m.h + m.dy > height) return false;
    const auto pitch = static_cast<std::size_t>(width) * 4;
    const auto bytes = static_cast<std::size_t>(m.w) * 4;
    auto row = [&](std::int32_t y) { return px + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(m.x) * 4; };
    if (m.dy > 0)  // content moved up: read below, so walk down
        for (std::int32_t y = m.y; y < m.y + m.h; ++y) std::memmove(row(y), row(y + m.dy), bytes);
    else if (m.dy < 0)
        for (std::int32_t y = m.y + m.h - 1; y >= m.y; --y) std::memmove(row(y), row(y + m.dy), bytes);
    return true;
}

}  // namespace

bool apply(Canvas& c, std::int32_t width, std::int32_t height, const link::down::Encoded& e, bool rgba) {
    if (!e.keyframe && (c.width != width || c.height != height)) return false;  // a delta needs its base
    const auto raw = unpack(width, height, e);
    if (!raw) return false;
    const Grid g{width, height, e.tile};
    if (e.keyframe) {
        c.width = width;
        c.height = height;
        c.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    }
    if (e.scroll && !move_rows(reinterpret_cast<std::uint8_t*>(c.pixels.data()), width, height, *e.scroll)) return false;
    const auto pitch = static_cast<std::size_t>(width) * 4;
    std::size_t at = 0;
    for (auto i : e.tiles) {
        const auto r = g.rect(i);
        const auto row_bytes = static_cast<std::size_t>(r.w) * 4;
        for (std::int32_t y = 0; y < r.h; ++y) {
            auto* dst = c.pixels.data() + static_cast<std::size_t>(r.y + y) * pitch + static_cast<std::size_t>(r.x) * 4;
            const auto* src = raw->data() + at;
            if (!rgba)
                std::memcpy(dst, src, row_bytes);
            else
                to_rgba(reinterpret_cast<std::uint8_t*>(dst), reinterpret_cast<const std::uint8_t*>(src), r.w, e.alpha);
            at += row_bytes;
        }
    }
    return true;
}

bool apply_rgba(std::vector<std::uint8_t>& rgba, std::int32_t width, std::int32_t height, const link::down::Encoded& e,
                std::vector<std::array<int, 4>>& written) {
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    if (!e.keyframe && rgba.size() != size) return false;
    const auto raw = unpack(width, height, e);
    if (!raw) return false;
    rgba.resize(size);
    if (e.scroll) {
        if (!move_rows(rgba.data(), width, height, *e.scroll)) return false;
        written.push_back({e.scroll->x, e.scroll->y, e.scroll->w, e.scroll->h});
    }
    const Grid g{width, height, e.tile};
    const auto pitch = static_cast<std::size_t>(width) * 4;
    std::size_t at = 0;
    for (auto i : e.tiles) {
        const auto r = g.rect(i);
        for (std::int32_t y = 0; y < r.h; ++y) {
            to_rgba(rgba.data() + static_cast<std::size_t>(r.y + y) * pitch + static_cast<std::size_t>(r.x) * 4,
                    reinterpret_cast<const std::uint8_t*>(raw->data() + at), r.w, e.alpha);
            at += static_cast<std::size_t>(r.w) * 4;
        }
        written.push_back({r.x, r.y, r.w, r.h});
    }
    return true;
}

Runs runs(std::int32_t width, std::int32_t height, const std::vector<std::array<std::int32_t, 4>>& damage,
          std::uint32_t tile) {
    Runs out;
    if (width <= 0 || height <= 0) return out;
    const Grid g{width, height, tile};
    std::vector<bool> hit(g.count(), false);
    const auto t = static_cast<std::int64_t>(tile);
    for (const auto& d : damage) {
        // Clamp (clients send INT32_MAX for "everything"), then mark tiles.
        const std::int64_t x0 = std::max<std::int64_t>(0, d[0]), y0 = std::max<std::int64_t>(0, d[1]);
        const std::int64_t x1 = std::min<std::int64_t>(width, static_cast<std::int64_t>(d[0]) + d[2]);
        const std::int64_t y1 = std::min<std::int64_t>(height, static_cast<std::int64_t>(d[1]) + d[3]);
        if (x1 <= x0 || y1 <= y0) continue;
        for (auto ty = y0 / t; ty <= (y1 - 1) / t; ++ty)
            for (auto tx = x0 / t; tx <= (x1 - 1) / t; ++tx) hit[static_cast<std::size_t>(ty) * g.cols + tx] = true;
    }
    for (std::uint32_t row = 0; row < g.rows; ++row)
        for (std::uint32_t col = 0; col < g.cols;) {
            if (!hit[row * g.cols + col]) {
                ++col;
                continue;
            }
            const auto first = g.rect(row * g.cols + col);
            std::int32_t w = 0;
            for (; col < g.cols && hit[row * g.cols + col]; ++col) {
                out.tiles.push_back(row * g.cols + col);
                w += g.rect(row * g.cols + col).w;
            }
            out.rects.push_back({first.x, first.y, w, first.h});
            out.area += static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(first.h);
        }
    return out;
}

std::optional<link::down::Encoded> pack(const Runs& r, std::span<const std::byte> pixels, bool alpha, std::uint32_t tile,
                                        int level) {
    link::down::Encoded e;
    e.tile = tile;
    e.alpha = alpha;
    e.keyframe = false;
    e.tiles = r.tiles;
    // Each run's rows are its tiles side by side: regroup tile by tile.
    std::vector<std::byte> raw;
    raw.reserve(pixels.size());
    std::size_t at = 0;
    for (const auto& run : r.rects) {
        const auto pitch = static_cast<std::size_t>(run[2]) * 4;
        if (at + pitch * static_cast<std::size_t>(run[3]) > pixels.size()) return std::nullopt;
        for (int x = 0; x < run[2]; x += static_cast<int>(tile)) {
            const auto tw = static_cast<std::size_t>(std::min<int>(static_cast<int>(tile), run[2] - x)) * 4;
            for (int y = 0; y < run[3]; ++y) {
                const auto* row = pixels.data() + at + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4;
                raw.insert(raw.end(), row, row + tw);
            }
        }
        at += pitch * static_cast<std::size_t>(run[3]);
    }
    e.raw_size = static_cast<std::uint32_t>(raw.size());
    if (!raw.empty()) {
        thread_local ZSTD_CCtx* cctx = ZSTD_createCCtx();
        e.data.resize(ZSTD_compressBound(raw.size()));
        const auto n = ZSTD_compressCCtx(cctx, e.data.data(), e.data.size(), raw.data(), raw.size(), level);
        if (ZSTD_isError(n)) return std::nullopt;
        e.data.resize(n);
    }
    return e;
}

}  // namespace i4::remote
