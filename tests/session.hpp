#pragma once
// tests/session.hpp — random wm sessions, shared by the fuzz tests.
//
// A Session is a seeded generator of realistic event streams: windows
// coming and going, outputs plugged and unplugged, every command. Several
// tests consume the same stream for different properties (invariants,
// replay, the wire codec, prediction), so the generator lives here once.

#include <random>
#include <string>
#include <string_view>

#include "i4/base/match.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/arrange.hpp"

namespace i4::test {

inline constexpr std::string_view commands[] = {
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

inline std::string describe(const wm::Event& e) {
    using namespace wm;
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

/// Draws events that make sense against a State (it never unmaps a window
/// that does not exist). It reads the state but never changes it.
class Generator {
public:
    explicit Generator(unsigned seed) : rng_(seed) {}

    std::size_t pick(std::size_t n) { return static_cast<std::size_t>(rng_() % n); }

    /// The next event, and a note saying what it was (the command text).
    std::pair<wm::Event, std::string> next(const wm::State& s) {
        using namespace wm;
        const auto roll = pick(100);
        std::vector<WindowId> ids;
        for (const auto& [id, w] : s.windows) ids.push_back(id);
        for (const auto& u : s.unplaced) ids.push_back(u.id);

        auto has = [&](std::string_view name) {
            for (const auto& [id, o] : s.outputs)
                if (o.info.name == name) return true;
            return false;
        };

        if (roll < 22) {
            const WindowId id{next_window_++};
            const bool floating = pick(8) == 0;
            std::optional<Size<space::Desktop>> size;
            if (pick(3) == 0)
                size = Size<space::Desktop>{static_cast<std::int32_t>(100 + pick(1500)),
                                            static_cast<std::int32_t>(100 + pick(1000))};
            Event e = ev::WindowMapped{id, WindowInfo{"w" + std::to_string(id.value), "", size, floating}};
            return {e, describe(e)};
        }
        if (roll < 34 && !ids.empty()) {
            Event e = ev::WindowUnmapped{ids[pick(ids.size())]};
            return {e, describe(e)};
        }
        if (roll < 38 && !ids.empty()) {
            Event e = ev::WindowActivated{ids[pick(ids.size())]};
            return {e, describe(e)};
        }
        if (roll < 41) {
            Event e = has("B") ? (pick(3) == 0 ? Event{ev::OutputChanged{OutputInfo{"B", {0, 800, 1280, 720}}}}
                                               : Event{ev::OutputRemoved{"B"}})
                               : Event{ev::OutputAdded{OutputInfo{"B", {1000, 0, 1920, 1080}}}};
            return {e, describe(e)};
        }
        if (roll == 41) {
            Event e = has("A") ? Event{ev::OutputRemoved{"A"}} : Event{ev::OutputAdded{OutputInfo{"A", {0, 0, 1000, 800}}}};
            return {e, describe(e)};
        }
        if (roll == 42) {
            Settings st;
            st.border = static_cast<Border>(pick(3));
            st.gaps_inner = static_cast<std::int32_t>(pick(3) * 8);
            st.gaps_outer = static_cast<std::int32_t>(pick(3) * 8);
            st.focus_wrapping = pick(2) == 0;
            return {ev::Configure{st}, "configure"};
        }
        const auto c = commands[pick(std::size(commands))];
        return {ev::Run{*parse(c)}, std::string(c)};
    }

private:
    std::mt19937 rng_;
    std::uint32_t next_window_ = 1;
};

inline unsigned seeds_from_env(unsigned fallback) {
    const char* env = std::getenv("I4_FUZZ_SEEDS");
    return env ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : fallback;
}

}  // namespace i4::test
