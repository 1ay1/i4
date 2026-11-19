// tests/test_launcher.cpp — $mod+d: .desktop parsing, ranking, the flow.
#include <jaal/host/headless.hpp>

#include "check.hpp"
#include "head/head.hpp"
#include "head/launcher.hpp"
#include "i4/base/match.hpp"
#include "server/apps.hpp"

using namespace i4;
using namespace i4::head;

namespace {

link::App app(std::string name, std::string comment = {}, bool terminal = false) {
    return {name, name, comment, name == "htop" ? "htop" : "run-" + name, terminal};
}

const std::vector<link::App> apps = {
    app("Alacritty", "A fast terminal"), app("Files", "Browse files"), app("Firefox", "Browse the web"),
    app("htop", "Process viewer", true), app("Profile Manager"), app("Settings"),
};

std::vector<std::string> names(std::string_view q) {
    std::vector<std::string> out;
    for (auto i : rank(apps, q)) out.push_back(apps[i].name);
    return out;
}

}  // namespace

// -- .desktop files ------------------------------------------------------------------

TEST(a_desktop_entry_parses) {
    const auto a = server::parse_desktop("org.mozilla.firefox", R"(
# a comment
[Desktop Entry]
Type=Application
Name=Firefox
Name[de]=Feuerfuchs
Comment=Browse the World Wide Web
Exec=/usr/lib/firefox/firefox %u
Terminal=false

[Desktop Action new-window]
Name=New Window
Exec=firefox --new-window %u
)");
    CHECK(a.has_value());
    CHECK(a && a->name == "Firefox");  // not the localised one
    CHECK(a && a->exec == "/usr/lib/firefox/firefox");  // %u gone, and not the action's Exec
    CHECK(a && a->comment == "Browse the World Wide Web");
    CHECK(a && !a->terminal);
}

TEST(entries_that_are_not_for_launching_are_skipped) {
    CHECK(!server::parse_desktop("x", "[Desktop Entry]\nType=Link\nName=x\nExec=x\n"));
    CHECK(!server::parse_desktop("x", "[Desktop Entry]\nType=Application\nName=x\nExec=x\nNoDisplay=true\n"));
    CHECK(!server::parse_desktop("x", "[Desktop Entry]\nType=Application\nName=x\nExec=x\nHidden=true\n"));
    CHECK(!server::parse_desktop("x", "[Desktop Entry]\nType=Application\nName=x\nExec=x\nOnlyShowIn=GNOME;\n"));
    CHECK(!server::parse_desktop("x", "[Desktop Entry]\nType=Application\nName=x\n"));
}

TEST(field_codes_are_removed) {
    CHECK_EQ(server::strip_field_codes("foo %F --bar %u"), std::string("foo --bar"));
    CHECK_EQ(server::strip_field_codes("printf 100%%"), std::string("printf 100%"));
}

// -- ranking -----------------------------------------------------------------------------

TEST(an_empty_query_lists_everything_in_order) {
    CHECK_EQ(rank(apps, "").size(), apps.size());
    CHECK(names("").front() == "Alacritty");
}

TEST(prefix_beats_word_start_beats_scattered) {
    const auto r = names("fi");
    CHECK(r.size() >= 3);
    CHECK(r.at(0) == "Files");    // prefix, shorter
    CHECK(r.at(1) == "Firefox");  // prefix
    CHECK(r.at(2) == "Profile Manager");
}

TEST(letters_need_not_be_adjacent_but_must_be_in_order) {
    CHECK(names("ffx") == std::vector<std::string>{"Firefox"});
    CHECK(names("xff").empty());
}

TEST(the_comment_is_searched_too) {
    CHECK(names("web") == std::vector<std::string>{"Firefox"});
}

TEST(terminal_apps_run_in_the_terminal) {
    CHECK_EQ(command(apps[3], "kitty"), std::string("kitty -e htop"));
    CHECK_EQ(command(apps[2], "kitty"), std::string("run-Firefox"));
}

// -- the flow, in the head program ---------------------------------------------------------

namespace {

using H = jaal::headless<Head>;

void attached(H& h) {
    link::Server server;
    (void)server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}});
    h.send(Heard{*server.receive(link::Replica::hello()).to_sender, std::nullopt});
    h.send(Heard{link::down::Apps{apps}, std::nullopt});
    h.record().clear();
}

}  // namespace

TEST(mod_d_opens_typing_filters_enter_launches) {
    H h;
    attached(h);
    const mayag::Mods alt{.alt = true};
    h.send(mayag::KeyEvent{mayag::Key::d, alt});
    h.send(mayag::TextEvent{"d"});  // mayag's twin of the chord: not typing
    CHECK(h.model().launcher.open);
    CHECK(h.model().launcher.query.empty());

    h.send(mayag::TextEvent{"fire"});
    CHECK_EQ(h.model().launcher.shown.size(), 1u);
    // While open, keys are the launcher's: nothing reaches the session.
    CHECK(h.effects<do_say>().empty());

    h.send(mayag::KeyEvent{mayag::Key::enter, {}});
    CHECK(!h.model().launcher.open);
    const auto said = h.effects<do_say>();
    CHECK_EQ(said.size(), 1u);
    const auto* r = said.empty() ? nullptr : std::get_if<link::up::Request>(&said.front().message);
    const auto* run = r ? std::get_if<wm::ev::Run>(&r->event) : nullptr;
    CHECK(run && run->script == wm::Script{wm::cmd::Exec{"run-Firefox"}});
}

TEST(escape_closes_without_launching) {
    H h;
    attached(h);
    h.send(mayag::KeyEvent{mayag::Key::d, mayag::Mods{.alt = true}});
    h.send(mayag::KeyEvent{mayag::Key::down, {}});
    CHECK_EQ(h.model().launcher.selected, 1u);
    h.send(mayag::KeyEvent{mayag::Key::escape, {}});
    CHECK(!h.model().launcher.open);
    CHECK(h.effects<do_say>().empty());
}

TEST(backspace_widens_the_search_again) {
    H h;
    attached(h);
    h.send(mayag::KeyEvent{mayag::Key::d, mayag::Mods{.alt = true}});
    h.send(mayag::TextEvent{"ffx"});
    CHECK_EQ(h.model().launcher.shown.size(), 1u);
    h.send(mayag::KeyEvent{mayag::Key::backspace, {}});
    h.send(mayag::KeyEvent{mayag::Key::backspace, {}});
    CHECK(h.model().launcher.shown.size() > 1);
}

TEST(when_closed_text_and_keys_go_where_they_did) {
    H h;
    attached(h);
    h.send(mayag::TextEvent{"x"});  // nothing: the client gets the KeyEvent
    CHECK(h.effects<do_say>().empty());
    h.send(mayag::KeyEvent{mayag::Key::x, {}});
    CHECK_EQ(h.effects<do_say>().size(), 2u);  // press + release to the client
}
