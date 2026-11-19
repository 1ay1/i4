// src/head/scene.cpp — springs, and the frame as mayag nodes.
#include "scene.hpp"

#include <algorithm>

namespace i4::head {

using namespace mayag;
using namespace mayag::dsl;

namespace {

// Catppuccin Mocha, mauve accent.
constexpr auto crust = rgb<0x11111b>;
constexpr auto mantle = rgb<0x181825>;
constexpr auto base = rgb<0x1e1e2e>;
constexpr auto surface0 = rgb<0x313244>;
constexpr auto surface1 = rgb<0x45475a>;
constexpr auto overlay0 = rgb<0x6c7086>;
constexpr auto subtext = rgb<0xa6adc8>;
constexpr auto text_c = rgb<0xcdd6f4>;
constexpr auto mauve = rgb<0xcba6f7>;
constexpr auto peach = rgb<0xfab387>;
constexpr auto green = rgb<0xa6e3a1>;

constexpr Spring motion_spring{260.0f, 30.0f};

Vec4 vec(Rect<space::Desktop> r) {
    return {static_cast<float>(r.left()), static_cast<float>(r.top()), static_cast<float>(r.size.width),
            static_cast<float>(r.size.height)};
}

Node tile(const wm::Tile& t, Vec4 at, const std::string& label, std::optional<std::uint32_t> texture, float shift = 0.0f) {
    const float inset = 3.0f;  // a hair of desktop between tiles
    const auto accent = t.focused ? mauve : surface1;
    const float w = std::max(0.0f, at.z - 2 * inset), h_ = std::max(0.0f, at.w - 2 * inset);
    if (texture) {
        // The client's own pixels, inside an i4 border. The image fills the
        // tile as it moves, so a window animating to a new size stretches
        // for the few frames until its client has drawn at the new size.
        Node frame = (box() | absolute(at.x + inset, at.y + bar_height + inset) | size(w, h_)
                      | border(2, accent) | radius(6) | clip
                      | shadow(t.layer == wm::Layer::floating ? 24.0f : 6.0f, crust.fade(0.6f)))
                         .build();
        // A predicted scroll moves the pixels we have; the frame clips them.
        frame.add_child((image(*texture) | absolute(2, 2 - shift) | size(std::max(0.0f, w - 4), std::max(0.0f, h_ - 4))).build());
        return frame;
    }
    return (v(text_owned(label) | font(13) | semibold | fg(t.focused ? text_c : subtext) | ellipsis,
              text_owned(t.layer == wm::Layer::floating ? std::string("floating") : std::string()) | font(11) |
                  fg(overlay0))
            | gap(4) | pad(10, 12)
            | absolute(at.x + inset, at.y + bar_height + inset)
            | size(w, h_)
            | bg(t.focused ? surface0 : base) | border(2, accent) | radius(8)
            | shadow(t.layer == wm::Layer::floating ? 24.0f : 8.0f, crust.fade(0.6f))
            | clip)
        .build();
}

Node title(const wm::Title& t) {
    const auto r = vec(t.rect);
    return (h(text_owned(t.text) | font(11) | fg(t.active ? text_c : overlay0) | ellipsis)
            | pad(0, 8) | align(Align::center)
            | absolute(r.x + 3, r.y + bar_height + 3) | size(std::max(0.0f, r.z - 6), std::max(0.0f, r.w - 3))
            | bg(t.active ? (t.focused ? mauve.fade(0.25f) : surface0) : mantle) | radius(6))
        .build();
}

Node bar(const Hud& hud) {
    std::vector<Node> ws;
    for (const auto& name : hud.workspaces) {
        const bool on = name == hud.focused_workspace;
        ws.push_back((h(text_owned(name) | font(12) | bold | fg(on ? crust : subtext))
                      | pad(2, 10) | radius(6) | bg(on ? mauve : surface0))
                         .build());
    }
    const auto net = hud.latency_ms < 0
                         ? "session · " + std::to_string(hud.in_flight) + " in flight · " +
                               std::to_string(hud.corrections) + " corrected"
                     : hud.latency_ms == 0
                         ? std::string("local")
                         : std::to_string(hud.latency_ms) + " ms link · " + std::to_string(hud.in_flight) +
                               " in flight · " + std::to_string(hud.corrections) + " corrected";
    return (h(h_list(std::move(ws), 6.0f),
              box() | grow(),
              text_owned(net) | font(12) | fg(hud.latency_ms == 0 ? green : peach),
              text_owned("  " + hud.mod_name + "+Return open · " + hud.mod_name + "+Shift+q close · " +
                         hud.mod_name + "+Shift+e quit")
                  | font(12) | fg(overlay0))
            | align(Align::center) | pad(0, 10) | absolute(0, 0) | width(pct(100)) | height(bar_height)
            | bg(mantle))
        .build();
}

}  // namespace

void Motion::retarget(const wm::Arrangement& a) {
    std::map<wm::WindowId, Moving> next;
    for (const auto& sc : a.screens)
        for (const auto& t : sc.tiles) {
            const auto target = vec(t.frame);
            auto it = frames_.find(t.window);
            Moving m = it != frames_.end() ? std::move(it->second) : [&] {
                Moving fresh;
                fresh.snap({target.x + target.z / 2, target.y + target.w / 2, 0.0f, 0.0f});
                return fresh;
            }();
            m.to(target);
            next.emplace(t.window, std::move(m));
        }
    frames_ = std::move(next);
}

void Motion::step(double dt) {
    for (auto& [w, m] : frames_) m.step(dt, motion_spring);
}

bool Motion::animating() const {
    return std::ranges::any_of(frames_, [](const auto& kv) { return kv.second.animating(); });
}

std::optional<Vec4> Motion::at(wm::WindowId w) const {
    auto it = frames_.find(w);
    if (it == frames_.end()) return std::nullopt;
    return it->second.value();
}

Node view(const wm::Arrangement& a, const Motion& motion, const Hud& hud, const Textures& textures, const Shifts& shifts) {
    std::vector<Node> layers;
    for (const auto& sc : a.screens) {
        for (const auto& t : sc.tiles) {
            const auto at = motion.at(t.window).value_or(vec(t.frame));
            const auto tex = textures.find(t.window);
            const auto sh = shifts.find(t.window);
            layers.push_back(tile(t, at, t.label, tex == textures.end() ? std::nullopt : std::optional{tex->second},
                                  sh == shifts.end() ? 0.0f : sh->second));
        }
        for (const auto& ti : sc.titles)
            if (ti.rect.size.height > 0) layers.push_back(title(ti));
    }
    layers.push_back(bar(hud));
    return (v_list(std::move(layers)) | width(pct(100)) | height(pct(100)) | bg(crust)).build();
}

std::uint64_t launcher_row(std::size_t i) { return node_id("i4.launcher.row", i); }

Node launcher_view(const Launcher& l, const std::vector<link::App>& apps, Size<space::Desktop> screen) {
    constexpr float width_px = 640.0f;
    constexpr std::size_t visible = 9;

    // A window of rows around the selection, so it scrolls with the arrows.
    const std::size_t first = l.selected >= visible ? l.selected + 1 - visible : 0;
    std::vector<Node> rows;
    for (std::size_t i = first; i < l.shown.size() && i < first + visible; ++i) {
        const auto& app = apps[l.shown[i]];
        const bool on = i == l.selected;
        rows.push_back((h(text_owned(app.name) | font(14) | semibold | fg(on ? crust : text_c),
                          text_owned(app.comment) | font(12) | fg(on ? surface0 : overlay0) | ellipsis | grow())
                        | gap(12) | align(Align::center) | pad(8, 14) | width(pct(100)) | radius(8)
                        | bg(on ? mauve : base.fade(0.0f)) | id_of_raw(launcher_row(i)))
                           .build());
    }
    if (rows.empty())
        rows.push_back((text_owned(apps.empty() ? std::string("no applications found on the session")
                                                : std::string("nothing matches"))
                        | font(13) | fg(overlay0) | pad(10, 14))
                           .build());

    const auto count = std::to_string(l.shown.size()) + " / " + std::to_string(apps.size());
    const float x = std::max(8.0f, (static_cast<float>(screen.width) - width_px) / 2.0f);
    const float y = static_cast<float>(screen.height) * 0.18f;
    return (v(h(text<"❯"> | font(18) | bold | fg(mauve),
                text_owned(l.query.empty() ? std::string("run an app…") : l.query + "▁") | font(18)
                    | fg(l.query.empty() ? overlay0 : text_c) | grow(),
                text_owned(count) | font(12) | fg(overlay0))
                  | gap(12) | align(Align::center) | pad(6, 6),
              box() | height(1) | width(pct(100)) | bg(surface0),
              v_list(std::move(rows), 2.0f) | width(pct(100)))
            | gap(8) | pad(12) | absolute(x, y) | width(width_px)
            | bg(mantle) | border(2, mauve) | radius(12) | shadow(40, crust.fade(0.8f), {0.0f, 12.0f}))
        .build();
}

Node notice_view(const std::string& message, Size<space::Desktop> screen) {
    constexpr float width_px = 560.0f;
    const float x = std::max(8.0f, (static_cast<float>(screen.width) - width_px) / 2.0f);
    const float y = std::max(bar_height + 8.0f, static_cast<float>(screen.height) - 72.0f);
    return (h(text<"!"> | font(15) | bold | fg(crust) | pad(0, 8) | bg(peach) | radius(6),
              text_owned(message) | font(13) | fg(text_c) | grow())
            | gap(12) | align(Align::center) | pad(10, 12) | absolute(x, y) | width(width_px)
            | bg(mantle) | border(1, peach) | radius(10) | shadow(28, crust.fade(0.8f), {0.0f, 8.0f}))
        .build();
}

}  // namespace i4::head
