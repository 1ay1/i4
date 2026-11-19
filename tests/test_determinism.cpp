// tests/test_determinism.cpp — random sessions, every invariant, every step.
//
// Thousands of random events (windows coming and going, outputs plugged and
// unplugged, every command) from fixed seeds. After every single step the
// tree must pass check() and arrange() must produce sane geometry. At the
// end, replaying the same events into a fresh State must give an identical
// State: that is the property a head relies on to predict.
//
// std::mt19937's sequence is fixed by the standard, so a failing seed fails
// the same way on every machine. To debug one, print the log.
#include <cstdlib>
#include <random>

#include "check.hpp"
#include "i4/base/match.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/arrange.hpp"

using namespace i4;
using namespace i4::wm;

namespace {

std::string describe(const Event& e) {
    return match(
        e, [](const ev::OutputAdded& x) { return "output added " + x.info.name; },
        [](const ev::OutputChanged& x) { return "output changed " + x.info.name; },
        [](const ev::OutputRemoved& x) { return "output removed " + x.name; },
        [](const ev::WindowMapped& x) { return "mapped " + std::to_string(x.id.value) + (x.info.floating ? " floating" : ""); },
        [](const ev::WindowChanged& x) { return "changed " + std::to_string(x.id.value); },
        [](const ev::WindowUnmapped& x) { return "unmapped " + std::to_string(x.id.value); },
        [](const ev::WindowActivated& x) { return "activated " + std::to_string(x.id.value); },
        [](const ev::Run& x) { return "run (" + std::to_string(x.script.size()) + " commands)"; },
        [](const ev::Configure&) { return std::string("configure"); });
}

constexpr std::string_view commands[] = {
    "focus left", "focus right", "focus up", "focus down", "focus parent", "focus child", "focus mode_toggle",
    "focus output left", "focus output right", "move left", "move right", "move up", "move down",
    "split v", "split h", "split toggle", "layout tabbed", "layout stacking", "layout toggle split",
    "layout toggle all", "layout toggle", "floating toggle", "fullscreen toggle", "kill",
    "workspace 1", "workspace 2", "workspace 3", "workspace next", "workspace prev",
    "workspace next_on_output", "workspace back_and_forth", "workspace number 4", "workspace web",
    "move container to workspace 2", "move container to workspace number 3", "move container to workspace web",
    "move container to output right", "move container to output left",
    "move workspace to output right", "move workspace to output left",
    "resize grow width 10 px or 10 ppt", "resize shrink height 10 px or 10 ppt", "resize shrink width 50 px or 40 ppt",
    "exec foot",
};

struct Session {
    State state;
    std::vector<Event> log;
    std::vector<std::string> notes;  // parallel to log: what each event was
    std::mt19937 rng;
    std::uint32_t next_window = 1;
    bool ok = true;

    explicit Session(unsigned seed) : rng(seed) {}

    std::size_t pick(std::size_t n) { return static_cast<std::size_t>(rng() % n); }

    std::vector<WindowId> windows() const {
        std::vector<WindowId> out;
        for (const auto& [id, w] : state.windows) out.push_back(id);
        for (const auto& u : state.unplaced) out.push_back(u.id);
        return out;
    }

    void push(Event e, std::string note = {}) {
        notes.push_back(note.empty() ? describe(e) : std::move(note));
        log.push_back(e);
        const auto out = apply(state, e);
        if (auto c = check(state); !c) {
            i4::test::fail("step " + std::to_string(log.size()) + ": " + c.error() + "\n       shape: " + shape(state));
            for (std::size_t i = log.size() > 6 ? log.size() - 6 : 0; i < log.size(); ++i)
                std::printf("       event %zu: %s\n", i + 1, notes[i].c_str());
            ok = false;
            return;
        }
        sane(arrange(state));
        // The server's half of a kill: the window goes.
        for (const auto& fx : out.effects)
            if (const auto* close = std::get_if<fx::Close>(&fx)) push(ev::WindowUnmapped{close->window});
    }

    void sane(const Arrangement& a) {
        for (const auto& sc : a.screens)
            for (const auto& t : sc.tiles) {
                if (t.frame.size.width < 0 || t.frame.size.height < 0 || t.content.size.width < 0 ||
                    t.content.size.height < 0) {
                    i4::test::fail("negative size at step " + std::to_string(log.size()));
                    ok = false;
                }
                if (t.layer == Layer::tiling && t.frame.intersected(sc.rect) != t.frame && !t.frame.empty()) {
                    i4::test::fail("tiled window outside its output at step " + std::to_string(log.size()));
                    ok = false;
                }
            }
    }

    void step() {
        const auto roll = pick(100);
        const auto ids = windows();
        if (roll < 22) {
            const WindowId id{next_window++};
            const bool floating = pick(8) == 0;
            std::optional<Size<Desktop>> size;
            if (pick(3) == 0) size = Size<Desktop>{static_cast<std::int32_t>(100 + pick(1500)), static_cast<std::int32_t>(100 + pick(1000))};
            push(ev::WindowMapped{id, WindowInfo{"w" + std::to_string(id.value), "", size, floating}});
        } else if (roll < 34 && !ids.empty()) {
            push(ev::WindowUnmapped{ids[pick(ids.size())]});
        } else if (roll < 38 && !ids.empty()) {
            push(ev::WindowActivated{ids[pick(ids.size())]});
        } else if (roll < 41) {
            // Plug and unplug a second output, sometimes moved.
            bool has_b = false;
            for (const auto& [id, o] : state.outputs) has_b |= o.info.name == "B";
            if (has_b && pick(3) == 0)
                push(ev::OutputChanged{OutputInfo{"B", {0, 800, 1280, 720}}});
            else if (has_b)
                push(ev::OutputRemoved{"B"});
            else
                push(ev::OutputAdded{OutputInfo{"B", {1000, 0, 1920, 1080}}});
        } else if (roll == 41) {
            bool has_a = false;
            for (const auto& [id, o] : state.outputs) has_a |= o.info.name == "A";
            push(has_a ? Event{ev::OutputRemoved{"A"}} : Event{ev::OutputAdded{OutputInfo{"A", {0, 0, 1000, 800}}}});
        } else if (roll == 42) {
            Settings st;
            st.border = static_cast<Border>(pick(3));
            st.gaps_inner = static_cast<std::int32_t>(pick(3) * 8);
            st.gaps_outer = static_cast<std::int32_t>(pick(3) * 8);
            st.focus_wrapping = pick(2) == 0;
            push(ev::Configure{st});
        } else {
            const auto c = commands[pick(std::size(commands))];
            push(ev::Run{*parse(c)}, std::string(c));
        }
    }
};

}  // namespace

TEST(random_sessions_keep_every_invariant_and_replay_exactly) {
    // 12 seeds by default keeps the suite quick; I4_FUZZ_SEEDS=500 for a
    // deep run.
    const char* env = std::getenv("I4_FUZZ_SEEDS");
    const unsigned seeds = env ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 12;
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        Session s(seed);
        s.push(ev::OutputAdded{OutputInfo{"A", {0, 0, 1000, 800}}});
        for (int i = 0; i < 2000 && s.ok; ++i) s.step();
        if (!s.ok) {
            std::printf("  (seed %u)\n", seed);
            return;
        }

        State replay;
        for (const auto& e : s.log) apply(replay, e);
        if (!(replay == s.state)) {
            i4::test::fail("seed " + std::to_string(seed) + ": replay diverged");
            return;
        }
    }
}

TEST(every_command_parses) {
    for (auto c : commands) {
        if (!parse(c)) i4::test::fail("does not parse: " + std::string(c));
    }
}
