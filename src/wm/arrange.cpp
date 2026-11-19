// src/wm/arrange.cpp — the tree, as rectangles.
#include "i4/wm/arrange.hpp"

#include <algorithm>
#include <map>
#include <span>

#include "i4/base/match.hpp"
#include "tree.hpp"

namespace i4::wm {

namespace tree {

Size<Desktop> decorated(const Settings& st, Size<Desktop> c) {
    const auto bw = st.border_width;
    switch (st.border) {
        case Border::none: return c;
        case Border::pixel: return {c.width + 2 * bw, c.height + 2 * bw};
        case Border::normal: return {c.width + 2 * bw, c.height + st.title_height + bw};
    }
    return c;
}

Rect<Desktop> undecorated(const Settings& st, Rect<Desktop> f, bool titled) {
    auto shrink = [&](std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
        return Rect<Desktop>{Point<Desktop>{f.left() + l, f.top() + t},
                             Size<Desktop>{std::max(0, f.size.width - l - r), std::max(0, f.size.height - t - b)}};
    };
    const auto bw = st.border_width;
    switch (st.border) {
        case Border::none: return f;
        case Border::pixel: return shrink(bw, bw, bw, bw);
        case Border::normal: return titled ? shrink(bw, st.title_height, bw, bw) : shrink(bw, bw, bw, bw);
    }
    return f;
}

}  // namespace tree

namespace {

/// Share `total` pixels out by weight.
///
/// Lifted from tapestry (compositor/src/program/tree.cpp): rounding happens
/// once, on the running edge, never per child. Rounding each share on its own
/// loses up to n-1 pixels and leaves a line of desktop between two windows
/// that moves as windows come and go. Here the shares always sum to total.
/// All integer, so every head computes the same pixels.
std::vector<std::int32_t> divide(std::int32_t total, std::span<const Share> weights) {
    std::vector<std::int32_t> out(weights.size(), 0);
    if (weights.empty() || total <= 0) return out;
    std::uint64_t edge = 0;
    std::int32_t used = 0;
    for (std::size_t i = 0; i + 1 < weights.size(); ++i) {
        edge += weights[i];
        const auto at = static_cast<std::int32_t>((edge * static_cast<std::uint64_t>(total) + whole / 2) / whole);
        out[i] = std::clamp(at - used, 0, total - used);
        used += out[i];
    }
    out.back() = total - used;
    return out;
}

std::vector<Share> equal_shares(std::size_t n) {
    std::vector<Share> out(n, static_cast<Share>(whole / n));
    out.front() += static_cast<Share>(whole - out.front() * n);
    return out;
}

struct Builder {
    const State& s;
    Arrangement out;
    std::map<ConId, Rect<Desktop>> frames;

    [[nodiscard]] bool lit(ConId c) const { return s.focus && tree::inside(s, c, *s.focus); }

    [[nodiscard]] std::string label(ConId c) const {
        if (const auto* l = std::get_if<Leaf>(&tree::con(s, c).body)) {
            const auto& info = s.windows.at(l->window).info;
            return info.title.empty() ? info.app_id : info.title;
        }
        return tree::describe(s, c);
    }

    /// `screen` null: placed but hidden. `titled`: a window's own title bar
    /// (not under a tab). `bare`: fullscreen, no decoration at all.
    void leaf(Screen* screen, ConId c, WindowId window, Rect<Desktop> frame, Layer layer, bool titled, bool bare) {
        frames[c] = frame;
        const Tile tile{window, c, frame, bare ? frame : tree::undecorated(s.settings, frame, titled), layer, lit(c),
                        label(c)};
        if (screen == nullptr) {
            out.hidden.push_back(tile);
            return;
        }
        screen->tiles.push_back(tile);
        if (!bare && titled && s.settings.border == Border::normal) {
            const Rect<Desktop> bar{frame.origin, Size<Desktop>{frame.size.width, std::min(s.settings.title_height, frame.size.height)}};
            screen->titles.push_back(Title{c, bar, label(c), lit(c), false});
        }
    }

    void place(Screen* screen, ConId c, Rect<Desktop> r, Layer layer, bool titled, std::optional<ConId> skip) {
        if (c == skip) return;  // the fullscreen con, placed on its own
        match(
            tree::con(s, c).body, [&](const Leaf& l) { leaf(screen, c, l.window, r, layer, titled, false); },
            [&](const Split& sp) { children(screen, c, sp, r, layer, skip); });
    }

    void children(Screen* screen, ConId c, const Split& sp, Rect<Desktop> r, Layer layer, std::optional<ConId> skip) {
        frames[c] = r;
        const auto n = static_cast<std::int32_t>(sp.children.size());
        if (n == 0) return;
        const auto gap = s.settings.gaps_inner;
        const auto th = s.settings.title_height;

        switch (sp.layout) {
            case Layout::splith: {
                const auto widths = divide(r.size.width - gap * (n - 1), sp.weights);
                auto x = r.left();
                for (std::size_t i = 0; i < sp.children.size(); ++i) {
                    place(screen, sp.children[i], {x, r.top(), widths[i], r.size.height}, layer, true, skip);
                    x += widths[i] + gap;
                }
                return;
            }
            case Layout::splitv: {
                const auto heights = divide(r.size.height - gap * (n - 1), sp.weights);
                auto y = r.top();
                for (std::size_t i = 0; i < sp.children.size(); ++i) {
                    place(screen, sp.children[i], {r.left(), y, r.size.width, heights[i]}, layer, true, skip);
                    y += heights[i] + gap;
                }
                return;
            }
            case Layout::tabbed:
            case Layout::stacking: {
                // A row of tabs, or a column of title rows; under them the
                // one visible child. The rest keep the same size, hidden.
                const bool tabbed = sp.layout == Layout::tabbed;
                const auto bar = std::min(r.size.height, tabbed ? th : th * n);
                const auto widths = divide(r.size.width, equal_shares(sp.children.size()));
                const ConId shown = sp.mru.front();
                const Rect<Desktop> body{r.left(), r.top() + bar, r.size.width, r.size.height - bar};
                auto x = r.left();
                for (std::size_t i = 0; i < sp.children.size(); ++i) {
                    const auto ch = sp.children[i];
                    const auto row = static_cast<std::int32_t>(i);
                    const Rect<Desktop> head = tabbed ? Rect<Desktop>{x, r.top(), widths[i], th}
                                                      : Rect<Desktop>{r.left(), r.top() + row * th, r.size.width, th};
                    x += widths[i];
                    if (screen != nullptr) screen->titles.push_back(Title{ch, head, label(ch), lit(ch), ch == shown});
                    place(ch == shown ? screen : nullptr, ch, body, layer, false, skip);
                }
                return;
            }
        }
    }

    void workspace(Screen* screen, WorkspaceId id, const Output& o) {
        const auto& w = tree::workspace(s, id);
        const auto area = o.info.rect.inset(s.settings.gaps_outer);

        if (!w.fullscreen) {
            place(screen, w.tiling, area, Layer::tiling, true, std::nullopt);
            for (ConId f : w.floating)
                place(screen, f, std::get<Floating>(tree::con(s, f).place).rect, Layer::floating, true, std::nullopt);
            return;
        }

        // Fullscreen covers the output; everything else on it is hidden.
        const ConId fs = *w.fullscreen;
        place(nullptr, w.tiling, area, Layer::tiling, true, fs);
        for (ConId f : w.floating)
            place(nullptr, f, std::get<Floating>(tree::con(s, f).place).rect, Layer::floating, true, fs);
        match(
            tree::con(s, fs).body,
            [&](const Leaf& l) { leaf(screen, fs, l.window, o.info.rect, Layer::fullscreen, false, true); },
            [&](const Split& sp) { children(screen, fs, sp, o.info.rect, Layer::fullscreen, std::nullopt); });
    }
};

Builder build(const State& s) {
    Builder b{s, {}, {}};
    for (const auto& [id, o] : s.outputs) {
        Screen screen{id, o.info.name, o.info.rect, o.shown, tree::workspace(s, o.shown).name, {}, {}};
        b.workspace(&screen, o.shown, o);
        for (auto ws : o.workspaces)
            if (ws != o.shown) b.workspace(nullptr, ws, o);
        b.out.screens.push_back(std::move(screen));
    }
    if (s.focus) {
        if (const auto* l = std::get_if<Leaf>(&tree::con(s, tree::descend(s, *s.focus)).body))
            b.out.keyboard_focus = l->window;
    }
    return b;
}

}  // namespace

Arrangement arrange(const State& s) { return build(s).out; }

std::optional<Rect<Desktop>> frame_of(const State& s, ConId con) {
    auto b = build(s);
    if (auto it = b.frames.find(con); it != b.frames.end()) return it->second;
    return std::nullopt;
}

}  // namespace i4::wm
