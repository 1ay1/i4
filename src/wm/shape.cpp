// src/wm/shape.cpp — i3's tree notation: "H[a V[b* c]] | f".
#include "i4/base/match.hpp"
#include "i4/wm/arrange.hpp"
#include "tree.hpp"

namespace i4::wm {

namespace {

char letter(Layout l) {
    switch (l) {
        case Layout::splith: return 'H';
        case Layout::splitv: return 'V';
        case Layout::tabbed: return 'T';
        case Layout::stacking: return 'S';
    }
    return '?';
}

/// `marks` adds `*` for focus and `!` for fullscreen (w must be set then).
void write(const State& s, ConId c, std::string& out, const Workspace* marks) {
    match(
        tree::con(s, c).body,
        [&](const Leaf& l) {
            const auto& app = s.windows.at(l.window).info.app_id;
            out += app.empty() ? "#" + std::to_string(l.window.value) : app;
        },
        [&](const Split& sp) {
            out += letter(sp.layout);
            out += '[';
            for (std::size_t i = 0; i < sp.children.size(); ++i) {
                if (i != 0) out += ' ';
                write(s, sp.children[i], out, marks);
            }
            out += ']';
        });
    if (marks != nullptr) {
        if (s.focus == c) out += '*';
        if (marks->fullscreen == c) out += '!';
    }
}

}  // namespace

std::string tree::describe(const State& s, ConId c) {
    std::string out;
    write(s, c, out, nullptr);
    return out;
}

std::string shape(const State& s, WorkspaceId id) {
    const auto& w = tree::workspace(s, id);
    std::string out;
    write(s, w.tiling, out, &w);
    if (!w.floating.empty()) {
        out += " |";
        for (ConId f : w.floating) {
            out += ' ';
            write(s, f, out, &w);
        }
    }
    return out;
}

std::string shape(const State& s) {
    const auto w = tree::focused_workspace(s);
    return w ? shape(s, *w) : std::string{};
}

}  // namespace i4::wm
