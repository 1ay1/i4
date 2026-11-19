// src/wm/apply.cpp — the window manager.
//
// Behaviour follows i3 (src/tree.c, src/move.c, src/con.c), restated over
// i4's tree. Where i4 departs from i3 it says so at the place it does.
#include "i4/wm/apply.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "i4/base/expect.hpp"
#include "i4/base/match.hpp"
#include "tree.hpp"

namespace i4::wm {

namespace {

using namespace tree;

std::unexpected<std::string> fail(std::string why) { return std::unexpected(std::move(why)); }

// -- outputs ------------------------------------------------------------------

std::optional<OutputId> output_named(const State& s, std::string_view name) {
    for (const auto& [id, o] : s.outputs)
        if (o.info.name == name) return id;
    return std::nullopt;
}

/// The nearest output in a direction that overlaps this one across it.
std::optional<OutputId> output_toward(const State& s, OutputId from, Direction d) {
    const auto a = output(s, from).info.rect;
    std::optional<OutputId> best;
    std::int64_t best_gap = 0, best_off = 0;
    for (const auto& [id, o] : s.outputs) {
        if (id == from) continue;
        const auto b = o.info.rect;
        const bool rows = b.top() < a.bottom() && a.top() < b.bottom();
        const bool cols = b.left() < a.right() && a.left() < b.right();
        std::int64_t gap = -1;
        switch (d) {
            case Direction::right: gap = rows ? b.left() - a.right() : -1; break;
            case Direction::left: gap = rows ? a.left() - b.right() : -1; break;
            case Direction::down: gap = cols ? b.top() - a.bottom() : -1; break;
            case Direction::up: gap = cols ? a.top() - b.bottom() : -1; break;
        }
        if (gap < 0) continue;
        const std::int64_t off = axis_of(d) == Axis::horizontal ? std::abs(b.center().y - a.center().y)
                                                                : std::abs(b.center().x - a.center().x);
        if (!best || gap < best_gap || (gap == best_gap && off < best_off)) {
            best = id;
            best_gap = gap;
            best_off = off;
        }
    }
    return best;
}

std::optional<OutputId> resolve(const State& s, const OutputRef& ref) {
    const auto here = *focused_output(s);
    return match(
        ref, [&](Direction d) { return output_toward(s, here, d); },
        [&](const std::string& name) { return output_named(s, name); });
}

void translate_floating(State& s, WorkspaceId ws, Point<Desktop> by) {
    for (ConId f : workspace(s, ws).floating) {
        auto& r = std::get<Floating>(con(s, f).place).rect;
        r = r.translated(by);
    }
}

// -- workspaces ---------------------------------------------------------------

std::optional<WorkspaceId> workspace_named(const State& s, std::string_view name) {
    for (const auto& [id, w] : s.workspaces)
        if (w.name == name) return id;
    return std::nullopt;
}

/// All workspaces (or one output's) in i3's order.
std::vector<WorkspaceId> ordered(const State& s, std::optional<OutputId> on) {
    std::vector<WorkspaceId> out;
    for (const auto& [id, w] : s.workspaces)
        if (!on || w.output == *on) out.push_back(id);
    std::ranges::sort(out, [&](WorkspaceId a, WorkspaceId b) {
        const auto& x = workspace(s, a);
        const auto& y = workspace(s, b);
        return std::pair(number_of(x.name).value_or(INT_MAX), x.name) <
               std::pair(number_of(y.name).value_or(INT_MAX), y.name);
    });
    return out;
}

/// Find the workspace a reference means, creating it (hidden, on the
/// focused output) when a name or number does not exist yet. Callers that
/// may not end up using it gc() it.
std::expected<WorkspaceId, std::string> resolve(State& s, const WorkspaceRef& ref) {
    const auto here = *focused_workspace(s);
    const auto out = workspace(s, here).output;
    auto named = [&](const std::string& name) {
        if (auto w = workspace_named(s, name)) return *w;
        return create_workspace(s, out, name);
    };
    auto step = [&](std::optional<OutputId> on, int by) {
        const auto list = ordered(s, on);
        const auto n = static_cast<std::ptrdiff_t>(list.size());
        const auto i = std::ranges::find(list, here) - list.begin();
        return list[static_cast<std::size_t>((i + by + n) % n)];
    };
    using R = std::expected<WorkspaceId, std::string>;
    return match(
        ref,
        [&](const ws::Named& n) -> R {
            if (n.name.empty()) return fail("a workspace needs a name");
            return named(n.name);
        },
        [&](const ws::Number& n) -> R {
            for (auto id : ordered(s, std::nullopt))
                if (number_of(workspace(s, id).name) == n.number) return id;
            return named(std::to_string(n.number));
        },
        [&](ws::Next) -> R { return step(std::nullopt, +1); },
        [&](ws::Prev) -> R { return step(std::nullopt, -1); },
        [&](ws::NextOnOutput) -> R { return step(out, +1); },
        [&](ws::PrevOnOutput) -> R { return step(out, -1); },
        [&](ws::BackAndForth) -> R {
            if (!s.previous_workspace) return fail("no previous workspace");
            return named(*s.previous_workspace);
        });
}

// -- placing windows ----------------------------------------------------------

Rect<Desktop> floating_rect(const State& s, WorkspaceId ws, const WindowInfo& info) {
    const auto area = output(s, workspace(s, ws).output).info.rect;
    const auto content = info.size.value_or(Size<Desktop>{area.size.width / 2, area.size.height / 2});
    auto frame = decorated(s.settings, content);
    frame.width = std::min(frame.width, area.size.width);
    frame.height = std::min(frame.height, area.size.height);
    return {Point<Desktop>{area.left() + (area.size.width - frame.width) / 2,
                           area.top() + (area.size.height - frame.height) / 2},
            frame};
}

/// Where a new tiling con goes on `ws`: next to the focused tiling con if
/// focus is in this workspace's tiling layer, else next to the one that
/// was focused last there.
ConId tiling_anchor(const State& s, WorkspaceId ws) {
    if (s.focus && con(s, *s.focus).workspace == ws && !is_floating(s, top(s, *s.focus))) return *s.focus;
    return tiling_focus(s, ws);
}

/// After a leaf anchor, or as the last child of a split one (the root of an
/// empty workspace, or a container after `focus parent`).
void place_tiled(State& s, ConId c, ConId anchor) {
    if (is_split(s, anchor)) {
        insert(s, anchor, split(s, anchor).children.size(), c);
        return;
    }
    const auto p = *parent(s, anchor);
    insert(s, p, index_in(split(s, p), anchor) + 1, c);
}

void map_window(State& s, WindowId id, WindowInfo info) {
    const auto ws = *focused_workspace(s);
    const auto anchor = tiling_anchor(s, ws);
    const bool floating = info.floating;
    const auto rect = floating_rect(s, ws, info);
    const auto c = create_con(s, ws, Child{}, Leaf{id});
    s.windows.emplace(id, Window{std::move(info), c});
    if (floating)
        float_in(s, ws, c, rect);
    else
        place_tiled(s, c, anchor);
    // A new window does not take focus from a fullscreen one (i3 agrees).
    if (!workspace(s, ws).fullscreen) focus(s, c);
}

void unmap_window(State& s, WindowId id) {
    const auto c = s.windows.at(id).con;
    const auto ws = con(s, c).workspace;
    const auto old = unlink(s, c);
    erase(s, c);
    if (old) normalise(s, *old);
    refocus_if_lost(s, ws);
    gc(s, ws);
}

/// Move `c` to another workspace, landing where a new window would. It
/// becomes that workspace's focus (so switching there finds it), but
/// global focus is the caller's business.
void transfer(State& s, ConId c, WorkspaceId to) {
    const auto from = con(s, c).workspace;
    if (from == to) return;
    const bool floating = is_floating(s, c);
    std::optional<Rect<Desktop>> rect;
    if (floating) {
        const auto by = output(s, workspace(s, to).output).info.rect.origin -
                        output(s, workspace(s, from).output).info.rect.origin;
        rect = std::get<Floating>(con(s, c).place).rect.translated(by);
    }
    const auto anchor = tiling_anchor(s, to);
    const auto old = unlink(s, c);
    if (floating)
        float_in(s, to, c, *rect);
    else
        place_tiled(s, c, anchor);
    if (old) normalise(s, *old);
    if (!workspace(s, to).fullscreen) raise(s, c);
}

// -- directions ---------------------------------------------------------------

/// Into a container from one side: the child nearest that side when the
/// container runs along the direction, the most recent one otherwise.
ConId descend_toward(const State& s, ConId c, Direction d) {
    for (;;) {
        const auto* sp = std::get_if<Split>(&con(s, c).body);
        if (sp == nullptr || sp->children.empty()) return c;
        if (!shows_one_child(sp->layout) && axis_of(sp->layout) == axis_of(d))
            c = backward(d) ? sp->children.back() : sp->children.front();
        else
            c = sp->mru.front();
    }
}

/// Nearest floating window whose center lies that way.
std::optional<ConId> floating_toward(const State& s, ConId from, Direction d) {
    const auto& w = workspace(s, con(s, from).workspace);
    const auto a = std::get<Floating>(con(s, from).place).rect.center();
    std::optional<ConId> best;
    std::int64_t best_score = 0;
    for (ConId f : w.floating) {
        if (f == from) continue;
        const auto b = std::get<Floating>(con(s, f).place).rect.center();
        const std::int64_t dx = b.x - a.x, dy = b.y - a.y;
        std::int64_t ahead = 0, off = 0;
        switch (d) {
            case Direction::right: ahead = dx, off = dy; break;
            case Direction::left: ahead = -dx, off = dy; break;
            case Direction::down: ahead = dy, off = dx; break;
            case Direction::up: ahead = -dy, off = dx; break;
        }
        if (ahead <= 0) continue;
        const auto score = ahead + std::abs(off);
        if (!best || score < best_score) best = f, best_score = score;
    }
    return best;
}

Result focus_direction(State& s, Direction d) {
    const auto f = *s.focus;
    const auto wsid = con(s, f).workspace;
    const auto& w = workspace(s, wsid);
    const std::optional<ConId> limit = w.fullscreen;

    if (!limit && is_floating(s, f)) {
        if (auto g = floating_toward(s, f, d)) {
            focus(s, *g);
            return {};
        }
    } else if (!is_floating(s, f)) {
        // Up the tree to the first container running this way that has a
        // neighbour on that side.
        for (ConId cur = f; !is_root(s, cur) && cur != limit;) {
            const auto p = *parent(s, cur);
            const auto& sp = split(s, p);
            if (axis_of(sp.layout) == axis_of(d)) {
                const auto i = index_in(sp, cur);
                if (backward(d) ? i > 0 : i + 1 < sp.children.size()) {
                    focus(s, descend_toward(s, sp.children[backward(d) ? i - 1 : i + 1], d));
                    return {};
                }
            }
            cur = p;
        }
    }

    // The edge of the workspace: the next output that way.
    if (auto o = output_toward(s, w.output, d)) {
        focus(s, workspace_focus(s, output(s, *o).shown));
        return {};
    }

    // Nothing beyond: wrap within the outermost container running this way.
    if (!s.settings.focus_wrapping || is_floating(s, f)) return {};
    std::vector<ConId> path;
    for (ConId cur = f; !is_root(s, cur) && cur != limit;) {
        cur = *parent(s, cur);
        path.push_back(cur);
    }
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        const auto& sp = split(s, *it);
        if (axis_of(sp.layout) == axis_of(d) && sp.children.size() > 1) {
            focus(s, descend_toward(s, backward(d) ? sp.children.back() : sp.children.front(), d));
            return {};
        }
    }
    return {};
}

/// The workspace's children into one container with its old layout, so the
/// root can turn: what i3 calls ws_force_orientation.
void wrap_root(State& s, ConId root) {
    const auto r = split(s, root);  // a copy
    Split inner = r;
    const auto w = create_con(s, con(s, root).workspace, Child{root}, std::move(inner));
    for (ConId ch : r.children) con(s, ch).place = Child{w};
    auto& rs = split(s, root);
    rs.children = {w};
    rs.weights = {whole};
    rs.mru = {w};
}

/// Move the focused con to a workspace, focus staying behind (below).
Result move_to(State& s, WorkspaceId to);

Result move_direction(State& s, Direction d, std::int32_t pixels) {
    const auto f = *s.focus;
    if (is_root(s, f)) return fail("nothing to move: focus is on the workspace");
    const auto& w = workspace(s, con(s, f).workspace);
    if (w.fullscreen == f) return fail("cannot move a fullscreen container");

    if (is_floating(s, f)) {
        auto& r = std::get<Floating>(con(s, f).place).rect;
        switch (d) {
            case Direction::left: r = r.translated({-pixels, 0}); break;
            case Direction::right: r = r.translated({pixels, 0}); break;
            case Direction::up: r = r.translated({0, -pixels}); break;
            case Direction::down: r = r.translated({0, pixels}); break;
        }
        return {};
    }

    const std::optional<ConId> limit = w.fullscreen;
    const auto out = w.output;
    const auto p = *parent(s, f);

    // 1. A neighbour on that side: swap with a window, enter a container.
    if (const auto& sp = split(s, p); axis_of(sp.layout) == axis_of(d)) {
        const auto i = index_in(sp, f);
        if (backward(d) ? i > 0 : i + 1 < sp.children.size()) {
            const auto j = backward(d) ? i - 1 : i + 1;
            const auto n = sp.children[j];
            if (!is_split(s, n)) {
                auto& m = split(s, p);
                std::swap(m.children[i], m.children[j]);
                std::swap(m.weights[i], m.weights[j]);
                return {};
            }
            // Next to the con we would focus going that way; on the near
            // side of it when its container runs this way.
            const auto target = descend_toward(s, n, d);
            const auto tp = *parent(s, target);
            const bool after = axis_of(split(s, tp).layout) != axis_of(d) || backward(d);
            const auto old = unlink(s, f);
            insert(s, tp, index_in(split(s, tp), target) + (after ? 1 : 0), f);
            normalise(s, *old);
            focus(s, f);
            return {};
        }
    }

    // 2. At the edge of its container: out, beside the first ancestor whose
    //    parent runs this way.
    ConId cur = p;
    while (!is_root(s, cur) && cur != limit) {
        const auto g = *parent(s, cur);
        if (axis_of(split(s, g).layout) == axis_of(d)) {
            const auto old = unlink(s, f);
            insert(s, g, index_in(split(s, g), cur) + (backward(d) ? 0 : 1), f);
            normalise(s, *old);
            focus(s, f);
            return {};
        }
        cur = g;
    }
    if (cur == limit) return {};

    // 3. At the workspace root, running the other way: turn the root.
    if (axis_of(split(s, cur).layout) != axis_of(d)) {
        if (split(s, cur).children.size() > 1) wrap_root(s, cur);
        auto& rs = split(s, cur);
        rs.layout = rs.last_split = split_along(axis_of(d));
        const auto old = unlink(s, f);
        insert(s, cur, backward(d) ? 0 : split(s, cur).children.size(), f);
        normalise(s, *old);
        focus(s, f);
        return {};
    }

    // 4. At the edge of the workspace: onto the next output, and focus
    //    follows — unless a fullscreen window covers it there, which focus
    //    may not escape; then focus stays, as for `move to workspace`.
    if (auto o = output_toward(s, out, d)) {
        const auto to = output(s, *o).shown;
        if (workspace(s, to).fullscreen) return move_to(s, to);
        transfer(s, f, to);
        focus(s, f);
    }
    return {};
}

// -- commands -------------------------------------------------------------------

Result act(State& s, const cmd::FocusDirection& c, Outcome&) { return focus_direction(s, c.direction); }

Result act(State& s, const cmd::FocusParent&, Outcome&) {
    const auto f = *s.focus;
    if (is_floating(s, f) || is_root(s, f)) return fail("already at the top");
    if (workspace(s, con(s, f).workspace).fullscreen == f) return fail("cannot leave a fullscreen container");
    focus(s, *parent(s, f));
    return {};
}

Result act(State& s, const cmd::FocusChild&, Outcome&) {
    const auto f = *s.focus;
    const auto* sp = std::get_if<Split>(&con(s, f).body);
    if (sp == nullptr || sp->mru.empty()) return fail("no child to focus");
    focus(s, sp->mru.front());
    return {};
}

Result act(State& s, const cmd::FocusModeToggle&, Outcome&) {
    const auto f = *s.focus;
    const auto ws = con(s, f).workspace;
    const auto& w = workspace(s, ws);
    if (w.fullscreen) return fail("cannot leave a fullscreen container");
    if (is_floating(s, top(s, f))) {
        focus(s, tiling_focus(s, ws));
        return {};
    }
    for (ConId c : w.mru)
        if (is_floating(s, c)) {
            focus(s, c);
            return {};
        }
    return fail("no floating windows");
}

Result act(State& s, const cmd::FocusOutput& c, Outcome&) {
    const auto o = resolve(s, c.output);
    if (!o) return fail("no such output");
    focus(s, workspace_focus(s, output(s, *o).shown));
    return {};
}

Result act(State& s, const cmd::Move& c, Outcome&) { return move_direction(s, c.direction, c.pixels); }

/// i4 departs from i3 here: moving the root of a workspace is an error
/// rather than moving everything in it.
Result move_to(State& s, WorkspaceId to) {
    const auto f = *s.focus;
    if (is_root(s, f)) {
        gc(s, to);  // resolve() may have created it for us
        return fail("nothing to move: focus is on the workspace");
    }
    const auto from = con(s, f).workspace;
    if (to != from) {
        transfer(s, f, to);
        // Focus stays behind, on whatever was focused before f there.
        s.focus.reset();
        focus(s, workspace_focus(s, from));
    }
    gc(s, to);
    return {};
}

Result act(State& s, const cmd::MoveToWorkspace& c, Outcome&) {
    const auto to = resolve(s, c.workspace);
    if (!to) return fail(to.error());
    return move_to(s, *to);
}

Result act(State& s, const cmd::MoveToOutput& c, Outcome&) {
    const auto o = resolve(s, c.output);
    if (!o) return fail("no such output");
    return move_to(s, output(s, *o).shown);
}

Result act(State& s, const cmd::MoveWorkspaceToOutput& c, Outcome&) {
    const auto o = resolve(s, c.output);
    if (!o) return fail("no such output");
    const auto ws = *focused_workspace(s);
    const auto from = workspace(s, ws).output;
    if (from == *o) return {};

    auto& src = output(s, from);
    std::erase(src.workspaces, ws);
    if (src.shown == ws) {
        // The output it leaves must still show something.
        src.shown = src.workspaces.empty() ? create_workspace(s, from, first_free_number(s)) : src.workspaces.front();
    }
    translate_floating(s, ws, output(s, *o).info.rect.origin - src.info.rect.origin);
    workspace(s, ws).output = *o;
    output(s, *o).workspaces.push_back(ws);
    sort_workspaces(s, *o);
    focus(s, workspace_focus(s, ws));
    return {};
}

Result act(State& s, const cmd::SwitchWorkspace& c, Outcome&) {
    const auto to = resolve(s, c.workspace);
    if (!to) return fail(to.error());
    if (*to != *focused_workspace(s)) focus(s, workspace_focus(s, *to));
    return {};
}

Result act(State& s, const cmd::SplitCon& c, Outcome&) {
    const auto f = *s.focus;
    if (is_floating(s, f)) return fail("cannot split a floating window");
    auto toward = [&](Axis now) {
        switch (c.kind) {
            case cmd::SplitKind::horizontal: return Layout::splith;
            case cmd::SplitKind::vertical: return Layout::splitv;
            case cmd::SplitKind::toggle: return now == Axis::horizontal ? Layout::splitv : Layout::splith;
        }
        return Layout::splith;
    };

    if (is_root(s, f)) {
        auto& r = split(s, f);
        r.layout = r.last_split = toward(axis_of(r.layout));
        return {};
    }

    const auto p = *parent(s, f);
    auto& sp = split(s, p);
    const auto layout = toward(axis_of(sp.layout));
    // A lone child: turning its container is the same split without the
    // extra level (i3 does the same).
    if (sp.children.size() == 1 && !shows_one_child(sp.layout)) {
        sp.layout = sp.last_split = layout;
        return {};
    }

    Split inner;
    inner.layout = inner.last_split = layout;
    inner.children = {f};
    inner.weights = {whole};
    inner.mru = {f};
    const auto w = create_con(s, con(s, f).workspace, Child{p}, std::move(inner));
    auto& ps = split(s, p);
    ps.children[index_in(ps, f)] = w;
    *std::ranges::find(ps.mru, f) = w;
    con(s, f).place = Child{w};
    return {};
}

Result act(State& s, const cmd::SetLayout& c, Outcome&) {
    const auto f = *s.focus;
    ConId target = f;
    if (!is_split(s, f)) {
        const auto p = parent(s, f);
        if (!p) return fail("a floating window has no layout");
        target = *p;
    }
    auto& sp = split(s, target);
    auto set = [&](Layout l) {
        sp.layout = l;
        if (!shows_one_child(l)) sp.last_split = l;
    };
    switch (c.kind) {
        case cmd::LayoutKind::splith: set(Layout::splith); break;
        case cmd::LayoutKind::splitv: set(Layout::splitv); break;
        case cmd::LayoutKind::tabbed: set(Layout::tabbed); break;
        case cmd::LayoutKind::stacking: set(Layout::stacking); break;
        case cmd::LayoutKind::toggle_split:
            if (shows_one_child(sp.layout))
                set(sp.last_split);
            else
                set(sp.layout == Layout::splith ? Layout::splitv : Layout::splith);
            break;
        case cmd::LayoutKind::toggle:  // stacking -> tabbed -> split -> stacking
            set(sp.layout == Layout::stacking ? Layout::tabbed
                : sp.layout == Layout::tabbed ? sp.last_split
                                              : Layout::stacking);
            break;
        case cmd::LayoutKind::toggle_all:  // splith -> splitv -> stacking -> tabbed
            switch (sp.layout) {
                case Layout::splith: set(Layout::splitv); break;
                case Layout::splitv: set(Layout::stacking); break;
                case Layout::stacking: set(Layout::tabbed); break;
                case Layout::tabbed: set(Layout::splith); break;
            }
            break;
    }
    return {};
}

bool wanted(Toggle t, bool now) {
    switch (t) {
        case Toggle::enable: return true;
        case Toggle::disable: return false;
        case Toggle::toggle: return !now;
    }
    return now;
}

Result act(State& s, const cmd::SetFloating& c, Outcome&) {
    const auto f = *s.focus;
    const bool now = is_floating(s, f);
    if (wanted(c.mode, now) == now) return {};
    if (is_split(s, f)) return fail("only windows can float");

    const auto ws = con(s, f).workspace;
    const bool fullscreen = workspace(s, ws).fullscreen == f;
    if (const auto fs = workspace(s, ws).fullscreen; fs && *fs != f)
        return fail("cannot float a window out of a fullscreen container");
    if (now) {
        const auto anchor = tiling_focus(s, ws);
        unlink(s, f);
        place_tiled(s, f, anchor);
    } else {
        const auto window = std::get<Leaf>(con(s, f).body).window;
        const auto rect = floating_rect(s, ws, s.windows.at(window).info);
        const auto old = unlink(s, f);
        float_in(s, ws, f, rect);
        if (old) normalise(s, *old);
    }
    if (fullscreen) workspace(s, ws).fullscreen = f;
    focus(s, f);
    return {};
}

Result act(State& s, const cmd::SetFullscreen& c, Outcome&) {
    const auto f = *s.focus;
    if (is_root(s, f)) return fail("cannot fullscreen a workspace");
    auto& w = workspace(s, con(s, f).workspace);
    const bool now = w.fullscreen == f;
    if (wanted(c.mode, now))
        w.fullscreen = f;
    else if (now)
        w.fullscreen.reset();
    return {};
}

Result act(State& s, const cmd::Kill&, Outcome& out) {
    const auto windows = windows_in(s, *s.focus);
    if (windows.empty()) return fail("nothing to kill");
    for (auto w : windows) out.effects.push_back(fx::Close{w});
    return {};
}

Result act(State& s, const cmd::Resize& c, Outcome&) {
    const auto f = *s.focus;
    if (is_floating(s, f)) {
        constexpr std::int32_t smallest = 20;
        auto& r = std::get<Floating>(con(s, f).place).rect;
        auto& side = c.axis == Axis::horizontal ? r.size.width : r.size.height;
        side = std::max(smallest, side + (c.grow ? c.pixels : -c.pixels));
        return {};
    }
    // The nearest container running along that axis: take from (or give
    // to) the next sibling, the previous one for the last child.
    for (ConId cur = f; auto p = parent(s, cur); cur = *p) {
        auto& sp = split(s, *p);
        if (shows_one_child(sp.layout) || axis_of(sp.layout) != c.axis || sp.children.size() < 2) continue;
        constexpr Share smallest = whole / 20;
        const auto i = index_in(sp, cur);
        const auto j = i + 1 < sp.children.size() ? i + 1 : i - 1;
        const std::int64_t pair = std::int64_t{sp.weights[i]} + sp.weights[j];
        if (pair <= 2 * std::int64_t{smallest}) return fail("too small to resize");
        const std::int64_t delta = std::int64_t{whole} * c.points / 100;
        const std::int64_t want = sp.weights[i] + (c.grow ? delta : -delta);
        sp.weights[i] = static_cast<Share>(std::clamp<std::int64_t>(want, smallest, pair - smallest));
        sp.weights[j] = static_cast<Share>(pair - sp.weights[i]);
        return {};
    }
    return fail("nothing to resize along that axis");
}

Result act(State&, const cmd::Exec&, Outcome&) { return {}; }  // handled in command()
Result act(State&, const cmd::Nop&, Outcome&) { return {}; }

Result command(State& s, const Command& c, Outcome& out) {
    if (const auto* e = std::get_if<cmd::Exec>(&c)) {
        out.effects.push_back(fx::Spawn{e->command});
        return {};
    }
    if (is<cmd::Nop>(c)) return {};
    if (!s.focus) return fail("no outputs");
    // One act() per alternative: a command without one fails to compile.
    return std::visit([&](const auto& x) { return act(s, x, out); }, c);
}

// -- outputs, as they come and go ------------------------------------------------

void change_output(State& s, OutputId id, const OutputInfo& info) {
    auto& o = output(s, id);
    const auto by = info.rect.origin - o.info.rect.origin;
    for (auto ws : o.workspaces) translate_floating(s, ws, by);
    o.info = info;
}

void add_output(State& s, const OutputInfo& info) {
    if (auto id = output_named(s, info.name)) {
        change_output(s, *id, info);
        return;
    }
    const auto id = fresh<OutputId>(s);
    s.outputs.emplace(id, Output{info, {}, {}});
    s.output_mru.push_back(id);
    const auto ws = create_workspace(s, id, first_free_number(s));
    output(s, id).shown = ws;
    if (!s.focus) focus(s, workspace_focus(s, ws));

    auto waiting = std::move(s.unplaced);
    s.unplaced.clear();
    for (auto& u : waiting) map_window(s, u.id, std::move(u.info));
}

void remove_output(State& s, std::string_view name) {
    const auto id = output_named(s, name);
    if (!id) return;

    if (s.outputs.size() == 1) {
        // Nowhere to go: every window waits for the next output.
        for (const auto& [wid, win] : s.windows) s.unplaced.push_back({wid, win.info});
        s.windows.clear();
        s.cons.clear();
        s.workspaces.clear();
        s.outputs.clear();
        s.output_mru.clear();
        s.focus.reset();
        return;
    }

    const bool had_focus = focused_output(s) == *id;
    if (had_focus) s.focus.reset();
    std::erase(s.output_mru, *id);
    const auto dest = s.output_mru.front();
    const auto by = output(s, dest).info.rect.origin - output(s, *id).info.rect.origin;

    for (auto ws : std::vector(output(s, *id).workspaces)) {
        if (is_empty(s, ws)) {
            erase(s, workspace(s, ws).tiling);
            s.workspaces.erase(ws);
            continue;
        }
        translate_floating(s, ws, by);
        workspace(s, ws).output = dest;
        output(s, dest).workspaces.push_back(ws);
    }
    sort_workspaces(s, dest);
    s.outputs.erase(*id);
    if (had_focus) focus(s, workspace_focus(s, output(s, dest).shown));
}

Unplaced* unplaced(State& s, WindowId id) {
    auto it = std::ranges::find(s.unplaced, id, &Unplaced::id);
    return it == s.unplaced.end() ? nullptr : &*it;
}

}  // namespace

Outcome apply(State& s, const Event& e) {
    Outcome out;
    match(
        e, [&](const ev::OutputAdded& x) { add_output(s, x.info); },
        [&](const ev::OutputChanged& x) {
            if (auto id = output_named(s, x.info.name)) change_output(s, *id, x.info);
        },
        [&](const ev::OutputRemoved& x) { remove_output(s, x.name); },
        [&](const ev::WindowMapped& x) {
            if (auto it = s.windows.find(x.id); it != s.windows.end())
                it->second.info = x.info;
            else if (auto* u = unplaced(s, x.id))
                u->info = x.info;
            else if (s.outputs.empty())
                s.unplaced.push_back({x.id, x.info});
            else
                map_window(s, x.id, x.info);
        },
        [&](const ev::WindowChanged& x) {
            if (auto it = s.windows.find(x.id); it != s.windows.end())
                it->second.info = x.info;
            else if (auto* u = unplaced(s, x.id))
                u->info = x.info;
        },
        [&](const ev::WindowUnmapped& x) {
            if (s.windows.contains(x.id))
                unmap_window(s, x.id);
            else
                std::erase_if(s.unplaced, [&](const Unplaced& u) { return u.id == x.id; });
        },
        [&](const ev::WindowActivated& x) {
            auto it = s.windows.find(x.id);
            if (it == s.windows.end()) return;
            const auto c = it->second.con;
            auto& w = workspace(s, con(s, c).workspace);
            if (w.fullscreen && !inside(s, c, *w.fullscreen)) w.fullscreen.reset();
            focus(s, c);
        },
        [&](const ev::Run& x) {
            for (const auto& c : x.script) out.results.push_back(command(s, c, out));
        },
        [&](const ev::Configure& x) { s.settings = x.settings; });
    return out;
}

Outcome run(State& s, std::string_view commands) {
    auto script = parse(commands);
    if (!script) {
        Outcome out;
        out.results.push_back(fail(script.error().message));
        return out;
    }
    return apply(s, ev::Run{std::move(*script)});
}

}  // namespace i4::wm
