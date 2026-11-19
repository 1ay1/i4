// tests/test_session.cpp — i4d's decisions, under jaal::headless.
//
// The Session is a jaal program with no fd in it, so every rule it keeps is
// checked here by sending it events and reading the effects it returns:
// what gets configured, who gets keyboard focus, which head hears what,
// when a client's frame callbacks fire. No socket, no client, no clock.
#include <jaal/host/headless.hpp>

#include "check.hpp"
#include "i4/base/match.hpp"
#include "server/session.hpp"

using namespace i4;
using namespace i4::server;

namespace {

using H = jaal::headless<Session>;

wm::OutputInfo output() { return {"A", {0, 0, 1000, 800}}; }
wm::WindowInfo app(std::string name) { return {name, name + " title", {}, false}; }

template <class D>
auto all(H& h) {
    auto v = h.template effects<D>();
    h.record().clear();
    return v;
}

/// A head that has said hello and been welcomed.
HeadId attach(H& h, std::uint32_t n = 1) {
    const HeadId id{n};
    h.send(ev::HeadJoined{id});
    h.send(ev::HeadSaid{id, link::up::Hello{link::protocol}});
    return id;
}

}  // namespace

TEST(a_new_window_is_configured_to_its_tile_and_focused) {
    H h;
    h.send(ev::OutputReady{output()});
    h.record().clear();
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    const auto conf = h.effects<do_configure>();
    CHECK_EQ(conf.size(), 1u);
    CHECK(conf.at(0).window == wm::WindowId{1});
    CHECK_EQ(conf.at(0).size, (Size<space::Desktop>{996, 796}));  // the frame minus its 2px border
    CHECK(conf.at(0).activated);
    const auto focus = h.effects<do_keyboard_focus>();
    CHECK(focus.size() == 1 && focus.at(0).window == wm::WindowId{1});
}

TEST(only_what_changed_is_configured) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    h.record().clear();
    h.send(ev::Mapped{wm::WindowId{2}, app("b")});
    // a shrinks and loses activation, b appears: two configures, no more.
    const auto conf = all<do_configure>(h);
    CHECK_EQ(conf.size(), 2u);
    // A title change moves nothing.
    h.send(ev::Retitled{wm::WindowId{2}, app("b2")});
    CHECK(h.effects<do_configure>().empty());
}

TEST(with_no_head_frames_are_released_at_once) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    h.record().clear();
    h.send(ev::Committed{wm::WindowId{1}, 1});
    const auto done = h.effects<do_frames_done>();
    CHECK(done.size() == 1 && done.at(0).generation == 1);
    CHECK(h.effects<do_share>().empty());
}

TEST(with_a_head_frames_wait_until_it_has_shown_them) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    const auto head = attach(h);
    h.record().clear();

    h.send(ev::Committed{wm::WindowId{1}, 7});
    CHECK(h.effects<do_frames_done>().empty());
    const auto share = all<do_share>(h);
    CHECK(share.size() == 1 && share.at(0).to == std::vector<HeadId>{head});

    h.send(ev::HeadSaid{head, link::up::Shown{wm::WindowId{1}, 7}});
    const auto done = h.effects<do_frames_done>();
    CHECK(done.size() == 1 && done.at(0).generation == 7);
}

TEST(a_head_is_welcomed_with_the_state_and_hears_every_event) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    h.record().clear();
    const auto head = attach(h);
    auto sent = all<do_send>(h);
    CHECK_EQ(sent.size(), 2u);  // Welcome, then what it can launch
    const auto* welcome = std::get_if<link::down::Welcome>(&sent.at(0).message);
    CHECK(welcome != nullptr);
    CHECK(welcome && welcome->state == h.model().session.state());
    CHECK(sent.size() == 2 && is<link::down::Apps>(sent.at(1).message));

    h.send(ev::Mapped{wm::WindowId{2}, app("b")});
    sent = all<do_send>(h);
    CHECK(sent.size() == 1 && sent.at(0).to == std::vector<HeadId>{head});
    CHECK(sent.size() == 1 && is<link::down::Applied>(sent.at(0).message));
}

TEST(a_head_from_another_build_is_refused_and_dropped) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::HeadJoined{HeadId{1}});
    h.record().clear();
    h.send(ev::HeadSaid{HeadId{1}, link::up::Hello{link::protocol ^ 1}});
    const auto sent = h.effects<do_send>();
    CHECK(sent.size() == 1 && is<link::down::Refused>(sent.at(0).message));
    CHECK_EQ(h.effects<do_drop>().size(), 1u);
    CHECK(h.model().heads.empty());
}

TEST(found_apps_go_to_every_welcomed_head) {
    H h;
    h.send(ev::OutputReady{output()});
    const auto head = attach(h);
    h.record().clear();
    h.send(ev::AppsFound{{link::App{"foot", "Foot", "Terminal", "foot", false}}});
    const auto sent = h.effects<do_send>();
    CHECK(sent.size() == 1 && sent.at(0).to == std::vector<HeadId>{head});
    const auto* apps = sent.empty() ? nullptr : std::get_if<link::down::Apps>(&sent.at(0).message);
    CHECK(apps && apps->apps.size() == 1 && apps->apps.front().name == "Foot");
}

TEST(a_head_request_is_applied_broadcast_and_obeyed) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    h.send(ev::Mapped{wm::WindowId{2}, app("b")});
    const auto head = attach(h);
    h.record().clear();

    h.send(ev::HeadSaid{head, link::up::Request{link::Nonce{1}, wm::ev::Run{*wm::parse("focus left")}}});
    const auto sent = h.effects<do_send>();
    CHECK_EQ(sent.size(), 1u);
    const auto* applied = sent.empty() ? nullptr : std::get_if<link::down::Applied>(&sent.at(0).message);
    CHECK(applied && applied->by == link::Nonce{1});
    const auto focus = h.effects<do_keyboard_focus>();
    CHECK(focus.size() == 1 && focus.at(0).window == wm::WindowId{1});
}

TEST(kill_asks_the_client_and_exec_spawns) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    const auto head = attach(h);
    h.record().clear();
    h.send(ev::HeadSaid{head, link::up::Request{link::Nonce{1}, wm::ev::Run{*wm::parse("kill; exec foot")}}});
    const auto close = h.effects<do_close>();
    CHECK(close.size() == 1 && close.at(0).window == wm::WindowId{1});
    const auto spawn = h.effects<do_spawn>();
    CHECK(spawn.size() == 1 && spawn.at(0).command == "foot");
}

TEST(clicking_a_window_focuses_it_and_input_goes_there) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    h.send(ev::Mapped{wm::WindowId{2}, app("b")});
    const auto head = attach(h);
    h.record().clear();

    h.send(ev::HeadSaid{head, link::up::Motion{{100, 100}}});  // over a, the left half
    const auto at = all<do_pointer_at>(h);
    CHECK(at.size() == 1 && at.at(0).window == wm::WindowId{1});
    CHECK(at.size() == 1 && at.at(0).at == (Point<space::Surface>{98, 98}));

    h.send(ev::HeadSaid{head, link::up::Button{0x110, true}});
    const auto focus = h.effects<do_keyboard_focus>();
    CHECK(focus.size() == 1 && focus.at(0).window == wm::WindowId{1});
    CHECK_EQ(h.effects<do_button>().size(), 1u);

    h.send(ev::HeadSaid{head, link::up::Key{30, true, wm::Mod::shift}});
    const auto keys = h.effects<do_key>();
    CHECK(keys.size() == 1 && keys.at(0).code == 30 && keys.at(0).mods.has(wm::Mod::shift));
}

TEST(an_app_that_opens_nothing_here_is_reported) {
    // Chrome already running on another display: the launched process
    // hands its window over and exits cleanly, having mapped nothing.
    H h;
    h.send(ev::OutputReady{output()});
    const auto head = attach(h);
    h.send(ev::Launched{LaunchId{1}, "/usr/bin/google-chrome-stable --x", true});
    h.record().clear();
    h.send(ev::Exited{LaunchId{1}, false, 0});
    const auto sent = h.effects<do_send>();
    CHECK(sent.size() == 1 && sent.at(0).to == std::vector<HeadId>{head});
    const auto* n = sent.empty() ? nullptr : std::get_if<link::down::Notice>(&sent.at(0).message);
    CHECK(n && n->text.starts_with("google-chrome-stable opened nothing here"));
}

TEST(an_app_that_opened_a_window_exits_quietly) {
    H h;
    h.send(ev::OutputReady{output()});
    attach(h);
    h.send(ev::Launched{LaunchId{1}, "foot", true});
    h.send(ev::Mapped{wm::WindowId{1}, app("foot")});
    h.record().clear();
    h.send(ev::Exited{LaunchId{1}, false, 0});
    CHECK(h.effects<do_send>().empty());
}

TEST(a_failed_launch_says_so) {
    H h;
    h.send(ev::OutputReady{output()});
    attach(h);
    h.send(ev::Launched{LaunchId{1}, "nosuchapp", true});
    h.record().clear();
    h.send(ev::Exited{LaunchId{1}, false, 127});
    const auto sent = h.effects<do_send>();
    const auto* n = sent.empty() ? nullptr : std::get_if<link::down::Notice>(&sent.at(0).message);
    CHECK(n && n->text == "nosuchapp failed (exit 127)");
}

TEST(when_the_last_head_leaves_no_client_waits_forever) {
    H h;
    h.send(ev::OutputReady{output()});
    h.send(ev::Mapped{wm::WindowId{1}, app("a")});
    const auto head = attach(h);
    h.send(ev::Committed{wm::WindowId{1}, 3});
    h.record().clear();
    h.send(ev::HeadLeft{head});
    const auto done = h.effects<do_frames_done>();
    CHECK(done.size() == 1 && done.at(0).generation == 3);
}
