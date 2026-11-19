// tests/test_tiles.cpp — frames across a network: exact, incremental, safe.
#include "check.hpp"

#include <cstring>
#include <random>

#include "i4/link/wire.hpp"
#include "remote/tiles.hpp"

using namespace i4;

namespace {

/// A client's shm pool: `w` x `h` BGRA at `offset`, rows `stride` apart.
struct Pool {
    std::int32_t w, h, offset, stride;
    std::vector<std::byte> mem;
    Pool(std::int32_t w_, std::int32_t h_, std::int32_t pad = 12, std::int32_t offset_ = 32)
        : w(w_), h(h_), offset(offset_), stride(w_ * 4 + pad),
          mem(static_cast<std::size_t>(offset_ + stride * h_), std::byte{0x5a}) {}
    std::byte* px(std::int32_t x, std::int32_t y) { return mem.data() + offset + y * stride + x * 4; }
    void fill(std::uint32_t seed) {
        std::mt19937 r{seed};
        for (std::int32_t y = 0; y < h; ++y)
            for (std::int32_t x = 0; x < w * 4; ++x) px(0, y)[x] = static_cast<std::byte>(r());
    }
    [[nodiscard]] remote::Source src() const { return {mem, w, h, offset, stride, true}; }
};

/// The pool's pixels, tight: what a decoder must end up with.
std::vector<std::byte> tight(Pool& p) {
    std::vector<std::byte> out;
    for (std::int32_t y = 0; y < p.h; ++y) out.insert(out.end(), p.px(0, y), p.px(0, y) + p.w * 4);
    return out;
}

/// Through the wire codec too: what a head actually decodes.
link::down::Encoded wire(const link::down::Encoded& e) {
    const auto bytes = link::encode(link::Down{link::down::Content{wm::WindowId{1}, 1, {1, 1}, e}});
    auto d = link::decode<link::Down>(bytes);
    return std::get<link::down::Encoded>(std::get<link::down::Content>(*d).layout);
}

}  // namespace

TEST(a_first_frame_is_a_keyframe_and_decodes_exactly) {
    Pool p{150, 70};  // not a multiple of the tile size: edge tiles are partial
    p.fill(1);
    remote::Canvas shadow, canvas;
    const auto e = remote::encode(shadow, p.src(), 64);
    CHECK(e.has_value());
    CHECK(e && e->keyframe && e->tiles.size() == 3u * 2u);
    CHECK(e && remote::apply(canvas, p.w, p.h, wire(*e)));
    CHECK(canvas.pixels == tight(p));
}

TEST(only_changed_tiles_travel) {
    Pool p{256, 128};
    p.fill(2);
    remote::Canvas shadow, canvas;
    CHECK(remote::apply(canvas, p.w, p.h, *remote::encode(shadow, p.src())));
    // Nothing changed: nothing to send.
    auto e = remote::encode(shadow, p.src());
    CHECK(e && !e->keyframe && e->tiles.empty() && e->data.empty());
    // One pixel in tile (2, 1): that tile alone.
    p.px(130, 70)[0] = std::byte{0};
    p.px(130, 70)[1] = std::byte{1};
    e = remote::encode(shadow, p.src());
    CHECK(e && e->tiles == std::vector<std::uint32_t>{1u * 4u + 2u});
    CHECK(e && remote::apply(canvas, p.w, p.h, wire(*e)));
    CHECK(canvas.pixels == tight(p));
}

TEST(a_resize_is_a_keyframe) {
    Pool a{100, 100}, b{120, 90};
    a.fill(3);
    b.fill(4);
    remote::Canvas shadow, canvas;
    CHECK(remote::apply(canvas, a.w, a.h, *remote::encode(shadow, a.src())));
    const auto e = remote::encode(shadow, b.src());
    CHECK(e && e->keyframe);
    CHECK(e && remote::apply(canvas, b.w, b.h, *e));
    CHECK(canvas.pixels == tight(b));
}

TEST(a_frame_that_does_not_fit_its_pool_is_refused) {
    Pool p{64, 64};
    remote::Canvas shadow;
    auto s = p.src();
    s.height = 65;
    CHECK(!remote::encode(shadow, s).has_value());
}

TEST(malformed_updates_leave_the_canvas_alone) {
    Pool p{128, 64};
    p.fill(5);
    remote::Canvas shadow, canvas;
    const auto key = *remote::encode(shadow, p.src());
    CHECK(remote::apply(canvas, p.w, p.h, key));
    const auto before = canvas.pixels;
    auto bad = key;
    bad.tiles.back() = 99;  // past the grid
    CHECK(!remote::apply(canvas, p.w, p.h, bad));
    bad = key;
    bad.raw_size -= 4;  // sizes that do not add up
    CHECK(!remote::apply(canvas, p.w, p.h, bad));
    bad = key;
    bad.data[bad.data.size() / 2] = static_cast<char>(bad.data[bad.data.size() / 2] ^ 0x55);  // corrupt stream
    (void)remote::apply(canvas, p.w, p.h, bad);  // may or may not decode; must not crash
    bad = key;
    bad.keyframe = false;
    bad.tiles = {0};
    bad.raw_size = 64 * 64 * 4;
    CHECK(!remote::apply(canvas, p.w + 1, p.h, bad));  // a delta against the wrong size
    CHECK(canvas.pixels == before || canvas.pixels.size() == before.size());
}

TEST(a_scroll_moves_rows_then_paints_the_revealed_tiles) {
    // A terminal printing: everything moves up 20 rows, a new line appears at
    // the bottom. The head holds frame A; it gets a move + the bottom tiles.
    Pool a{200, 150, 0, 0}, b{200, 150, 0, 0};
    a.fill(21);
    for (std::int32_t y = 0; y < 130; ++y) std::memcpy(b.px(0, y), a.px(0, y + 20), 200 * 4);
    std::mt19937 r{22};
    for (std::int32_t y = 130; y < 150; ++y)
        for (std::int32_t x = 0; x < 200 * 4; ++x) b.px(0, y)[x] = static_cast<std::byte>(r());
    remote::Canvas shadow, canvas;
    const auto key = *remote::encode(shadow, a.src());
    CHECK(remote::apply(canvas, a.w, a.h, key));
    std::vector<std::uint8_t> rgba;
    std::vector<std::array<int, 4>> written;
    CHECK(remote::apply_rgba(rgba, a.w, a.h, wire(key), written));
    // The update: move rows 0..130 from +20, then the tiles of rows 128..150.
    const auto runs = remote::runs(b.w, b.h, {{0, 128, 200, 22}});
    std::vector<std::byte> px;
    for (const auto& run : runs.rects)
        for (std::int32_t y = run[1]; y < run[1] + run[3]; ++y)
            px.insert(px.end(), b.px(run[0], y), b.px(run[0], y) + run[2] * 4);
    auto e = *remote::pack(runs, px, true);
    e.scroll = link::down::Move{0, 0, 200, 130, 20};
    CHECK(remote::apply(canvas, b.w, b.h, wire(e)));
    CHECK(canvas.pixels == tight(b));
    // The straight-RGBA path ends up with frame B too.
    written.clear();
    CHECK(remote::apply_rgba(rgba, b.w, b.h, wire(e), written));
    remote::Canvas s3, want;
    CHECK(remote::apply(want, b.w, b.h, *remote::encode(s3, b.src()), true));
    CHECK(want.pixels.size() == rgba.size() && std::memcmp(rgba.data(), want.pixels.data(), rgba.size()) == 0);
    CHECK(!written.empty() && written.front() == (std::array<int, 4>{0, 0, 200, 130}));
}

TEST(the_encoder_finds_a_scroll_and_sends_only_what_it_reveals) {
    // A terminal: 300 rows of distinct lines, then everything moves up 20.
    Pool a{256, 300, 0, 0}, b{256, 300, 0, 0};
    a.fill(31);
    for (std::int32_t y = 0; y < 280; ++y) std::memcpy(b.px(0, y), a.px(0, y + 20), 256 * 4);
    std::mt19937 r{32};
    for (std::int32_t y = 280; y < 300; ++y)
        for (std::int32_t x = 0; x < 256 * 4; ++x) b.px(0, y)[x] = static_cast<std::byte>(r());
    remote::Canvas shadow, canvas;
    CHECK(remote::apply(canvas, a.w, a.h, wire(*remote::encode(shadow, a.src()))));
    const auto e = *remote::encode(shadow, b.src());
    CHECK(e.scroll.has_value() && e.scroll->dy == 20);
    CHECK(e.tiles.size() <= 8u);  // the bottom row of tiles only (4 across, 64 px band)
    CHECK(remote::apply(canvas, b.w, b.h, wire(e)));
    CHECK(canvas.pixels == tight(b));
    CHECK(shadow.pixels == tight(b));
}

TEST(a_move_that_does_not_fit_is_refused) {
    Pool p{64, 64, 0, 0};
    p.fill(23);
    remote::Canvas shadow, canvas;
    CHECK(remote::apply(canvas, p.w, p.h, *remote::encode(shadow, p.src())));
    auto e = *remote::encode(shadow, p.src());
    e.scroll = link::down::Move{0, 10, 64, 50, 10};  // reads rows 20..70: past the bottom
    CHECK(!remote::apply(canvas, p.w, p.h, e));
}

TEST(straight_rgba_for_a_renderer) {
    Pool p{2, 1, 0, 0};
    // B, G, R, A: opaque red; half-transparent premultiplied white.
    const std::uint8_t px[] = {0, 0, 255, 255, 128, 128, 128, 128};
    std::memcpy(p.px(0, 0), px, sizeof(px));
    remote::Canvas shadow, canvas;
    CHECK(remote::apply(canvas, 2, 1, *remote::encode(shadow, p.src()), true));
    const auto* o = reinterpret_cast<const std::uint8_t*>(canvas.pixels.data());
    CHECK(o[0] == 255 && o[1] == 0 && o[2] == 0 && o[3] == 255);
    CHECK(o[4] == 255 && o[5] == 255 && o[6] == 255 && o[7] == 128);
}

TEST(damaged_tiles_read_as_runs_pack_and_apply_exactly) {
    Pool p{200, 130, 0, 0};
    p.fill(11);
    // Damage touching tiles (1,0) and (2,0) (one run) and (0,2).
    const std::vector<std::array<std::int32_t, 4>> damage = {{70, 10, 70, 5}, {3, 129, 1, 1}};
    const auto r = remote::runs(p.w, p.h, damage);
    CHECK((r.tiles == std::vector<std::uint32_t>{1, 2, 8}));
    CHECK(r.rects.size() == 2u);
    // The runs' pixels, as a GPU readback would hand them over.
    std::vector<std::byte> px;
    for (const auto& run : r.rects)
        for (int y = run[1]; y < run[1] + run[3]; ++y) px.insert(px.end(), p.px(run[0], y), p.px(run[0], y) + run[2] * 4);
    const auto e = remote::pack(r, px, true);
    CHECK(e.has_value());
    // Applied onto a picture, it matches the source wherever it wrote.
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(p.w) * p.h * 4, 0);
    std::vector<std::array<int, 4>> written;
    CHECK(e && remote::apply_rgba(rgba, p.w, p.h, wire(*e), written));
    CHECK(written.size() == 3u);
    bool same = true;
    for (const auto& w : written)
        for (int y = w[1]; y < w[1] + w[3]; ++y)
            for (int x = w[0]; x < w[0] + w[2]; ++x) {
                const auto* s = reinterpret_cast<const std::uint8_t*>(p.px(x, y));
                const auto* o = rgba.data() + (static_cast<std::size_t>(y) * p.w + x) * 4;
                if (s[3] == 255 && (o[0] != s[2] || o[1] != s[1] || o[2] != s[0])) same = false;
            }
    CHECK(same);
}

TEST(a_terminal_keystroke_is_small) {
    // A mostly flat 1100x640 terminal; one glyph appears.
    Pool p{1100, 640, 0, 0};
    for (std::int32_t y = 0; y < p.h; ++y)
        for (std::int32_t x = 0; x < p.w; ++x) {
            auto* q = p.px(x, y);
            q[0] = std::byte{0x2e}, q[1] = std::byte{0x1e}, q[2] = std::byte{0x1e}, q[3] = std::byte{0xff};
        }
    remote::Canvas shadow;
    const auto key = *remote::encode(shadow, p.src());
    for (std::int32_t y = 300; y < 316; ++y)
        for (std::int32_t x = 500; x < 509; ++x) p.px(x, y)[1] = std::byte{0xd0};
    const auto e = *remote::encode(shadow, p.src());
    std::printf("  keyframe %zu bytes, keystroke %zu bytes\n", key.data.size(), e.data.size());
    CHECK(e.tiles.size() <= 2u);
    CHECK(e.data.size() < 2000u);
}
