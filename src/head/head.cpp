// src/head/head.cpp — i4-head's decisions.
//
// Keys: a chord in the bindings is the window manager's (predicted: applied
// to the replica now, sent as a Request); anything else is the focused
// client's (sent as up::Key, evdev). mayag reports presses only, so a key
// sent to a client is a press followed at once by its release — enough for
// typing and shortcuts; held-key semantics (games) need key-up from mayag.
//
// Pointer: positions convert from window pixels to desktop pixels by
// subtracting the bar. A click is the session's to route (it focuses the
// window under the pointer, as in i3).
#include "head.hpp"

#include <cstdio>
#include <cstdlib>

#include "i4/base/match.hpp"
#include "keys.hpp"

namespace i4::head {

namespace {

using Cmd = Head::Cmd;

Cmd say(link::Up u) { return Say{std::move(u)}; }

Cmd batch(std::vector<Cmd> out) { return out.empty() ? Cmd::none() : Cmd::batch(std::move(out)); }

void rearrange(Head::Model& m) {
    m.arranged = wm::arrange(m.replica.shown());
    m.motion.retarget(m.arranged);
}

Point<space::Desktop> desktop(mayag::Vec2 p) {
    return {static_cast<std::int32_t>(p.x), static_cast<std::int32_t>(p.y - bar_height)};
}

/// The session should lay out for this screen: a predicted event like any
/// other, so the tiles move in this frame, not after a round trip.
Cmd report_output(Head::Model& m) {
    if (!m.replica.ready()) return {};
    const auto h = std::max(1, m.screen.height - static_cast<std::int32_t>(bar_height));
    const auto& outs = m.replica.shown().outputs;
    const std::string name = outs.empty() ? std::string("i4-0") : outs.begin()->second.info.name;
    auto req = m.replica.act(wm::ev::OutputChanged{wm::OutputInfo{name, {0, 0, std::max(1, m.screen.width), h}}});
    rearrange(m);
    return say(std::move(req));
}

}  // namespace

Cmd Head::init(Model& m) {
    m.mod = options.mod;
    m.bindings = wm::default_bindings(m.mod, options.terminal);
    return Cmd::batch(say(link::Replica::hello()), Cmd{mayag::SetTitle{"i4"}});
}

Cmd Head::update(Model& m, Heard h) {
    std::vector<Cmd> out;
    const bool first = !m.replica.ready();

    if (const auto* c = std::get_if<link::down::Content>(&h.message)) {
        // A new frame: upload it as this window's texture, and tell the
        // session it is shown (the client may draw the next one).
        if (h.pixels) {
            auto [it, fresh] = m.textures.try_emplace(c->window, m.next_texture);
            if (fresh) ++m.next_texture;
            if (std::getenv("I4_HEAD_TRACE") != nullptr) {
                // The centre pixel, so a trace shows real content arrived.
                const auto& img = h.pixels->get();
                const auto i = (static_cast<std::size_t>(img.height / 2) * img.width + img.width / 2) * 4;
                std::fprintf(stderr, "i4-head: window %u frame %llu %dx%d centre rgba(%u,%u,%u,%u)\n",
                             c->window.value, static_cast<unsigned long long>(c->generation), img.width, img.height,
                             img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]);
            }
            out.push_back(mayag::upload(it->second, std::move(*h.pixels)));
        }
        out.push_back(say(link::up::Shown{c->window, c->generation}));
        return batch(std::move(out));
    }

    if (auto ok = m.replica.receive(h.message); !ok) {
        std::fprintf(stderr, "i4-head: the session refused this head (a different build?)\n");
        return Cmd::quit(1);
    }
    // Windows that are gone: free their textures.
    for (auto it = m.textures.begin(); it != m.textures.end();) {
        if (m.replica.shown().windows.contains(it->first)) {
            ++it;
            continue;
        }
        out.push_back(mayag::ReleaseImage{it->second});
        it = m.textures.erase(it);
    }
    rearrange(m);
    if (first && m.replica.ready()) out.push_back(report_output(m));
    return batch(std::move(out));
}

Cmd Head::update(Model& m, Lost) {
    m.lost = true;
    std::fprintf(stderr, "i4-head: the session is gone\n");
    return Cmd::quit(0);
}

Cmd Head::update(Model& m, mayag::KeyEvent k) {
    if (!m.replica.ready()) return {};
    if (const auto c = chord(k)) {
        if (*c == wm::Chord{m.mod | wm::Mod::shift, "e"}) return Cmd::quit(0);  // i3's exit
        if (const auto* script = wm::lookup(m.bindings, *c)) {
            auto req = m.replica.act(wm::ev::Run{*script});
            rearrange(m);
            return say(std::move(req));
        }
    }
    const auto code = evdev(k.key);
    if (!code) return {};
    const auto mods = head::mods(k.mods);
    return Cmd::batch(say(link::up::Key{*code, true, mods}), say(link::up::Key{*code, false, mods}));
}

Cmd Head::update(Model& m, mayag::MouseMove e) {
    if (!m.replica.ready()) return {};
    return say(link::up::Motion{desktop(e.position)});
}

Cmd Head::update(Model& m, mayag::MouseDown e) {
    if (!m.replica.ready()) return {};
    return Cmd::batch(say(link::up::Motion{desktop(e.position)}), say(link::up::Button{evdev(e.button), true}));
}

Cmd Head::update(Model& m, mayag::MouseUp e) {
    if (!m.replica.ready()) return {};
    return say(link::up::Button{evdev(e.button), false});
}

Cmd Head::update(Model& m, mayag::ResizeEvent e) {
    m.screen = {static_cast<std::int32_t>(e.size.x), static_cast<std::int32_t>(e.size.y)};
    return report_output(m);
}

Cmd Head::update(Model& m, Tick t) {
    m.motion.step(t.dt);
    return {};
}

Cmd Head::update(Model&, mayag::CloseRequest) { return Cmd::quit(0); }

Head::Sub Head::subscribe(const Model& m) {
    return mayag::when(m.motion.animating(), Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
}

mayag::Node Head::view(const Model& m) {
    Hud hud;
    hud.in_flight = m.replica.pending();
    hud.corrections = m.replica.corrections();
    hud.mod_name = m.mod == wm::Mod::super ? "Super" : "Alt";
    if (!m.replica.ready()) hud.mod_name += "  ·  connecting...";
    const auto& s = m.replica.shown();
    for (const auto& [id, o] : s.outputs)
        for (auto ws : o.workspaces) hud.workspaces.push_back(s.workspaces.at(ws).name);
    for (const auto& sc : m.arranged.screens) hud.focused_workspace = sc.workspace_name;
    hud.latency_ms = -1;  // a real link: the bar shows requests in flight instead
    return head::view(m.arranged, m.motion, hud, m.textures);
}

}  // namespace i4::head
