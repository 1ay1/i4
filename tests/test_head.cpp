// tests/test_head.cpp — i4-head's decisions and its pixel conversion.
//
// The head program is pure, so it runs under jaal::headless: messages in,
// recorded effects out. No window, no GPU, no session.
#include <jaal/host/headless.hpp>

#include "check.hpp"
#include "head/head.hpp"
#include "head/pixels.hpp"
#include "i4/base/match.hpp"

using namespace i4;
using namespace i4::head;

namespace {

using H = jaal::headless<Head>;

std::vector<link::Up> said(H& h) {
    std::vector<link::Up> out;
    for (const auto& s : h.effects<do_say>()) out.push_back(s.message);
    h.record().clear();
    return out;
}

/// A head welcomed into a session with one output and two windows.
void welcome(H& h) {
    link::Server server;
    (void)server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}});
    (void)server.apply(wm::ev::WindowMapped{wm::WindowId{1}, {"a", "a", {}, false}});
    (void)server.apply(wm::ev::WindowMapped{wm::WindowId{2}, {"b", "b", {}, false}});
    h.send(Heard{*server.receive(link::Replica::hello()).to_sender, std::nullopt});
}

std::vector<std::byte> bytes(std::initializer_list<int> xs) {
    std::vector<std::byte> v;
    for (int x : xs) v.push_back(static_cast<std::byte>(x));
    return v;
}

}  // namespace

// -- pixels --------------------------------------------------------------------------

TEST(shm_pixels_are_swizzled_and_unpremultiplied) {
    // Two pixels, little-endian BGRA: opaque red, and half-transparent white
    // (premultiplied: 128,128,128,128).
    const auto mem = bytes({0, 0, 255, 255, 128, 128, 128, 128});
    auto img = convert(mem, Layout{2, 1, 0, 8, true});
    CHECK(img.has_value());
    if (!img) return;
    CHECK((std::vector<int>(img->rgba.begin(), img->rgba.end()) == std::vector<int>{255, 0, 0, 255, 255, 255, 255, 128}));
}

TEST(xrgb_ignores_the_padding_byte) {
    const auto mem = bytes({10, 20, 30, 0});
    auto img = convert(mem, Layout{1, 1, 0, 4, false});
    CHECK(img && (std::vector<int>(img->rgba.begin(), img->rgba.end()) == std::vector<int>{30, 20, 10, 255}));
}

TEST(offset_and_stride_select_the_frame) {
    // A pool of 16 bytes: 4 of padding, then a 1x2 frame with an 8-byte stride.
    const auto mem = bytes({9, 9, 9, 9, /*row0*/ 1, 2, 3, 255, 9, 9, 9, 9, /*row1*/ 4, 5, 6, 255});
    auto img = convert(mem, Layout{1, 2, 4, 8, true});
    CHECK(img && (std::vector<int>(img->rgba.begin(), img->rgba.end()) == std::vector<int>{3, 2, 1, 255, 6, 5, 4, 255}));
}

TEST(a_frame_larger_than_its_pool_is_refused) {
    const auto mem = bytes({0, 0, 0, 0});
    CHECK(!convert(mem, Layout{2, 1, 0, 8, true}).has_value());
    CHECK(!convert(mem, Layout{1, 1, 4, 4, true}).has_value());
    CHECK(!convert(mem, Layout{1, 1, 0, 2, true}).has_value());  // stride shorter than a row
}

// -- the program ---------------------------------------------------------------------

TEST(the_head_says_hello_first) {
    H h;
    const auto s = said(h);
    CHECK(s.size() == 1 && s.front() == link::Replica::hello());
    h.send(GpuReady{{link::up::Format{0x34325241, 0}}});  // the GPU came up
    const auto c = said(h);
    CHECK(c.size() == 1 && is<link::up::Caps>(c.front()));
}

TEST(on_welcome_it_reports_its_screen) {
    H h;
    (void)said(h);
    welcome(h);
    const auto s = said(h);
    CHECK_EQ(s.size(), 1u);
    const auto* r = s.empty() ? nullptr : std::get_if<link::up::Request>(&s.front());
    CHECK(r && is<wm::ev::OutputChanged>(r->event));
}

TEST(a_bound_key_is_predicted_and_sent_as_a_request) {
    H h;
    welcome(h);
    (void)said(h);
    CHECK_EQ(wm::shape(h.model().replica.shown()), std::string("H[a b*]"));
    h.send(mayag::KeyEvent{mayag::Key::h, mayag::Mods{.alt = true}});
    // Shown at once, before any server has answered.
    CHECK_EQ(wm::shape(h.model().replica.shown()), std::string("H[a* b]"));
    const auto s = said(h);
    CHECK(s.size() == 1 && is<link::up::Request>(s.front()));
}

TEST(an_unbound_key_goes_to_the_client_as_evdev) {
    H h;
    welcome(h);
    (void)said(h);
    h.send(mayag::KeyEvent{mayag::Key::a, mayag::Mods{.shift = true}});
    const auto s = said(h);
    CHECK_EQ(s.size(), 2u);  // press, release
    if (s.size() != 2) return;
    const auto* down = std::get_if<link::up::Key>(&s[0]);
    const auto* up = std::get_if<link::up::Key>(&s[1]);
    CHECK(down && down->code == 30 && down->pressed && down->mods.has(wm::Mod::shift));
    CHECK(up && up->code == 30 && !up->pressed);
    CHECK_EQ(wm::shape(h.model().replica.shown()), std::string("H[a b*]"));
}

TEST(a_frame_is_uploaded_and_acknowledged) {
    H h;
    welcome(h);
    (void)said(h);
    mayag::ImageData img;
    img.width = img.height = 1;
    img.rgba = {1, 2, 3, 255};
    h.send(Heard{link::down::Content{wm::WindowId{1}, 5, {1, 1}, link::down::ShmLayout{0, 4, true}, std::nullopt},
                 jaal::shared<mayag::ImageData>::make(std::move(img))});
    CHECK_EQ(h.effects<mayag::upload_image>().size(), 1u);
    const auto s = said(h);
    // Copied, so the client's memory is free at once; shown, so it may draw.
    CHECK((s.size() == 2 && s.front() == link::Up{link::up::Released{wm::WindowId{1}, 5}}));
    CHECK((s.size() == 2 && s.back() == link::Up{link::up::Shown{wm::WindowId{1}, 5}}));
    CHECK(h.model().textures.contains(wm::WindowId{1}));
}

TEST(the_pointer_is_reported_in_desktop_pixels) {
    H h;
    welcome(h);
    (void)said(h);
    h.send(mayag::MouseMove{{100.0f, 100.0f + bar_height}});
    const auto s = said(h);
    CHECK((s.size() == 1 && s.front() == link::Up{link::up::Motion{{100, 100}}}));
}

TEST(i3_exit_quits) {
    H h;
    welcome(h);
    h.send(mayag::KeyEvent{mayag::Key::e, mayag::Mods{.shift = true, .alt = true}});
    CHECK(h.kernel().quitting());
}
