#pragma once
// tests/desk.hpp — a window manager on a desk: one 1000x800 output, windows
// named by app_id, commands as i3 strings. Every step is followed by
// check(), so every test is also a test of every invariant.

#include <map>
#include <string>

#include "check.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/arrange.hpp"

namespace i4::test {

using namespace i4;
using namespace i4::wm;

struct Desk {
    State s;
    std::uint32_t next_window = 1;
    std::vector<Effect> effects;
    std::vector<Result> results;
    std::map<std::string, WindowId> ids;

    explicit Desk(bool with_output = true) {
        if (with_output) add_output("A", {0, 0, 1000, 800});
    }

    void after(Outcome o, std::source_location at) {
        effects.insert(effects.end(), o.effects.begin(), o.effects.end());
        results = std::move(o.results);
        if (auto ok = check(s); !ok) fail("invariant broken: " + ok.error() + "\n       shape: " + shape(), at);
    }

    void step(const Event& e, std::source_location at = std::source_location::current()) { after(apply(s, e), at); }

    /// Runs the commands; true when every one succeeded.
    bool cmd(std::string_view text, std::source_location at = std::source_location::current()) {
        after(run(s, text), at);
        for (const auto& r : results)
            if (!r) return false;
        return true;
    }

    void add_output(std::string name, Rect<Desktop> r, std::source_location at = std::source_location::current()) {
        step(ev::OutputAdded{OutputInfo{std::move(name), r}}, at);
    }

    WindowId open(const std::string& app, bool floating = false, std::optional<Size<Desktop>> size = {},
                  std::source_location at = std::source_location::current()) {
        const WindowId id{next_window++};
        ids[app] = id;
        step(ev::WindowMapped{id, WindowInfo{app, app + " title", size, floating}}, at);
        return id;
    }

    void close(const std::string& app, std::source_location at = std::source_location::current()) {
        step(ev::WindowUnmapped{ids.at(app)}, at);
    }

    void activate(const std::string& app, std::source_location at = std::source_location::current()) {
        step(ev::WindowActivated{ids.at(app)}, at);
    }

    [[nodiscard]] std::string shape() const { return wm::shape(s); }

    [[nodiscard]] std::string shape_of(const std::string& workspace) const {
        for (const auto& [id, w] : s.workspaces)
            if (w.name == workspace) return wm::shape(s, id);
        return "(no workspace " + workspace + ")";
    }

    [[nodiscard]] std::string workspace() const {
        if (!s.focus) return {};
        return s.workspaces.at(s.cons.at(*s.focus).workspace).name;
    }

    [[nodiscard]] std::string output() const {
        if (!s.focus) return {};
        return s.outputs.at(s.workspaces.at(s.cons.at(*s.focus).workspace).output).info.name;
    }

    /// The tile for a window, shown or hidden.
    [[nodiscard]] std::optional<Tile> tile(const std::string& app) const {
        const auto a = arrange(s);
        const auto id = ids.at(app);
        for (const auto& sc : a.screens)
            for (const auto& t : sc.tiles)
                if (t.window == id) return t;
        for (const auto& t : a.hidden)
            if (t.window == id) return t;
        return std::nullopt;
    }
};

}  // namespace i4::test
