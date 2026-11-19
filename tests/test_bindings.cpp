// tests/test_bindings.cpp — chords and the default table.
#include "check.hpp"
#include "i4/link/wire.hpp"
#include "i4/wm/bindings.hpp"

using namespace i4;
using namespace i4::wm;

static_assert(link::Wire<Bindings>, "bindings travel to heads, so heads can predict keys");
static_assert((Mod::super | Mod::shift).has(Mod::shift));
static_assert(!(Mod::super | Mod::shift).has(Mod::ctrl));

TEST(chords) {
    CHECK(parse_chord("$mod+Shift+H", Mod::super) == (Chord{Mod::super | Mod::shift, "h"}));
    CHECK(parse_chord("Mod1+return", Mod::super) == (Chord{Mod::alt, "Return"}));
    CHECK(parse_chord("Ctrl+Alt+Delete", Mod::super) == (Chord{Mod::ctrl | Mod::alt, "Delete"}));
    CHECK(parse_chord("F12", Mod::super) == (Chord{{}, "F12"}));
    CHECK(!parse_chord("Hyper+x", Mod::super));
    CHECK(!parse_chord("$mod++x", Mod::super));
}

TEST(defaults_cover_i3) {
    const auto b = default_bindings(Mod::super, "foot");
    auto does = [&](std::string_view chord) -> std::optional<Script> {
        const auto* s = lookup(b, *parse_chord(chord, Mod::super));
        return s ? std::optional<Script>{*s} : std::nullopt;
    };
    CHECK(does("$mod+Return") == (Script{cmd::Exec{"foot"}}));
    CHECK(does("$mod+Shift+q") == (Script{cmd::Kill{}}));
    CHECK(does("$mod+h") == (Script{cmd::FocusDirection{Direction::left}}));
    CHECK(does("$mod+Shift+Right") == (Script{cmd::Move{Direction::right}}));
    CHECK(does("$mod+0") == (Script{cmd::SwitchWorkspace{ws::Number{10}}}));
    CHECK(does("$mod+Shift+3") == (Script{cmd::MoveToWorkspace{ws::Number{3}}}));
    CHECK(!does("$mod+Ctrl+h").has_value());  // exact match: extra modifiers miss
}

TEST(later_bindings_override) {
    auto b = default_bindings(Mod::alt, "foot");
    b.push_back(*parse_binding("$mod+Return", "exec kitty", Mod::alt));
    CHECK(*lookup(b, Chord{Mod::alt, "Return"}) == (Script{cmd::Exec{"kitty"}}));
}
