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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "i4/base/match.hpp"
#include "keys.hpp"

namespace i4::head {

namespace {

using Cmd = Head::Cmd;

Cmd say(link::Up u) { return Say{std::move(u)}; }

/// Run the launcher's selection: `exec`, predicted, as a binding would.
Cmd launch(Head::Model& m) {
    const auto pick = m.launcher.choice();
    m.launcher.hide();
    if (!pick || !m.replica.ready()) return {};
    auto req = m.replica.act(wm::ev::Run{wm::Script{wm::cmd::Exec{command(m.apps[*pick], m.terminal)}}});
    return say(std::move(req));
}

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
    m.terminal = options.terminal;
    m.bindings = wm::default_bindings(m.mod, options.terminal);
    // Hello first. What this head can sample in place follows as up::Caps
    // once the GPU is up (GpuReady); until then clients get shm.
    return Cmd::batch(say(link::Replica::hello()), Cmd{mayag::SetTitle{"i4"}});
}

Cmd Head::update(Model& m, Heard h) {
    std::vector<Cmd> out;
    const bool first = !m.replica.ready();

    if (const auto* c = std::get_if<link::down::Content>(&h.message)) {
        // A new frame. A GPU buffer was imported by the host as this
        // window's texture already (zero copy) and is released when the GPU
        // has finished with it (GpuDone). Copied pixels are uploaded, and
        // the client's memory is free again at once.
        const auto tex = texture_of(c->window);
        if (h.texture != 0) {
            m.textures[c->window] = h.texture;
        } else if (h.pixels) {
            m.textures[c->window] = tex;
            out.push_back(mayag::upload(tex, std::move(*h.pixels)));
            out.push_back(say(link::up::Released{c->window, c->generation}));
        } else {
            out.push_back(say(link::up::Released{c->window, c->generation}));
        }
        out.push_back(say(link::up::Shown{c->window, c->generation}));
        return batch(std::move(out));
    }

    if (const auto* a = std::get_if<link::down::Apps>(&h.message)) {
        m.apps = a->apps;
        if (m.launcher.open) m.launcher.refilter(m.apps);
        return {};
    }
    if (const auto* n = std::get_if<link::down::Notice>(&h.message)) {
        m.notice = n->text;
        // Keyed: a newer notice replaces this timer, so it gets its full time.
        return Cmd::after("notice", std::chrono::milliseconds{6000}, ClearNotice{});
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
        out.push_back(mayag::ReleaseImage{texture_of(it->first)});  // its copied frames
        out.push_back(Forget{it->first});                           // its imported buffers
        it = m.textures.erase(it);
    }
    rearrange(m);
    if (first && m.replica.ready()) out.push_back(report_output(m));
    return batch(std::move(out));
}

Cmd Head::update(Model&, GpuDone d) { return say(link::up::Released{d.window, d.generation}); }

/// The session offers clients exactly this over linux-dmabuf, so they render
/// straight into memory this GPU shows as it is.
Cmd Head::update(Model&, GpuReady g) {
    if (!options.zero_copy) g.formats.clear();
    std::fprintf(stderr, "i4-head: GPU ready, %zu zero-copy buffer layouts, explicit sync %s\n", g.formats.size(),
                 g.explicit_sync ? "on" : "off");
    if (!options.zero_copy) g.explicit_sync = false;
    return say(link::up::Caps{std::move(g.formats), g.explicit_sync});
}

Cmd Head::update(Model& m, Lost) {
    m.lost = true;
    std::fprintf(stderr, "i4-head: the session is gone\n");
    return Cmd::quit(0);
}

namespace {

/// What the head itself does with a key press: a binding, the launcher.
/// Sets `took` when the key is the head's, so it never reaches a client.
Cmd command(Head::Model& m, const mayag::KeyEvent& k, bool& took) {
    took = true;
    const auto c = chord(k);
    m.swallow.clear();

    // $mod+d toggles the launcher; while it is open it has the keyboard.
    if (c && *c == wm::Chord{m.mod, "d"}) {
        m.swallow = c->key;
        if (m.launcher.open)
            m.launcher.hide();
        else
            m.launcher.show(m.apps);
        return {};
    }
    if (m.launcher.open) {
        using K = mayag::Key;
        switch (k.key) {
            case K::escape: m.launcher.hide(); return {};
            case K::enter: return launch(m);
            case K::up: m.launcher.move(-1); return {};
            case K::down:
            case K::tab: m.launcher.move(+1); return {};
            case K::backspace: m.launcher.erase(m.apps); return {};
            default: return {};  // printable keys arrive as TextEvent
        }
    }

    if (c) {
        if (*c == wm::Chord{m.mod | wm::Mod::shift, "e"}) return Cmd::quit(0);  // i3's exit
        if (const auto* script = wm::lookup(m.bindings, *c)) {
            m.swallow = c->key;
            auto req = m.replica.act(wm::ev::Run{*script});
            rearrange(m);
            return say(std::move(req));
        }
    }
    took = false;
    return {};
}

}  // namespace

Cmd Head::update(Model& m, mayag::KeyEvent k) {
    if (!m.replica.ready()) return {};
    if (m.raw_keyboard) {
        // Presses came through KeyInput already. Only the launcher uses
        // mayag's repeat (holding Down scrolls the list); clients repeat
        // keys themselves from wl_keyboard.repeat_info.
        if (!k.repeat || !m.launcher.open) return {};
        using K = mayag::Key;
        if (k.key != K::up && k.key != K::down && k.key != K::tab && k.key != K::backspace) return {};
        bool took = false;
        return command(m, k, took);
    }
    bool took = false;
    auto cmd = command(m, k, took);
    if (took) return cmd;
    const auto code = evdev(k.key);
    if (!code) return {};
    const auto mods = head::mods(k.mods);
    return Cmd::batch(say(link::up::Key{*code, true, mods}), say(link::up::Key{*code, false, mods}));
}

Cmd Head::update(Model& m, KeyInput k) {
    m.raw_keyboard = true;
    if (!m.replica.ready()) return {};
    if (!k.pressed) {
        // A release goes where its press went.
        if (m.taken.erase(k.code) != 0) return {};
        if (m.held.erase(k.code) == 0) return {};
        return say(link::up::Key{k.code, false, head::mods(k.key.mods)});
    }
    if (m.held.contains(k.code) || m.taken.contains(k.code)) return {};  // a platform repeat: clients repeat
    bool took = false;
    auto cmd = command(m, k.key, took);
    if (took) {
        m.taken.insert(k.code);
        return cmd;
    }
    m.held.insert(k.code);
    return say(link::up::Key{k.code, true, head::mods(k.key.mods)});
}

Cmd Head::update(Model& m, KeymapInput k) {
    m.raw_keyboard = true;
    m.keyboard.keymap = std::move(k.keymap);
    return say(m.keyboard);
}

Cmd Head::update(Model& m, RepeatInput r) {
    m.keyboard.rate = r.rate;
    m.keyboard.delay = r.delay;
    if (m.keyboard.keymap.empty()) return {};
    return say(m.keyboard);
}

Cmd Head::update(Model&, ModsInput k) { return say(k.mods); }

Cmd Head::update(Model& m, KeyboardLeft) {
    // The head window lost the keyboard (the user alt-tabbed away on the
    // machine it runs on): whatever was down is up, for clients too.
    std::vector<Cmd> out;
    for (auto code : m.held) out.push_back(say(link::up::Key{code, false, {}}));
    m.held.clear();
    m.taken.clear();
    out.push_back(say(link::up::Modifiers{0, 0, 0, 0}));
    return batch(std::move(out));
}

Cmd Head::update(Model& m, ScrollInput s) {
    if (!m.replica.ready() || m.launcher.open) return {};
    return say(s.scroll);
}

Cmd Head::update(Model& m, mayag::ScrollEvent s) {
    // Platforms without the raw tap: mayag's coalesced pixels, sent as
    // smooth scrolling (wl_fixed; wl_pointer's positive is down/right).
    if (m.raw_keyboard || !m.replica.ready() || m.launcher.open) return {};
    link::up::Scroll out;
    out.dx = static_cast<std::int32_t>(-s.delta.x * 256.0f);
    out.dy = static_cast<std::int32_t>(-s.delta.y * 256.0f);
    return say(out);
}

Cmd Head::update(Model& m, mayag::TextEvent t) {
    if (false) std::fprintf(stderr, "i4-head: text '%s'\n", t.text.c_str());
    if (const auto twin = std::exchange(m.swallow, {});
        twin.size() == 1 && t.text.size() == 1 && (t.text[0] | 0x20) == (twin[0] | 0x20))
        return {};
    if (m.launcher.open) m.launcher.type(m.apps, t.text);
    return {};
}

Cmd Head::update(Model& m, mayag::Clicked c) {
    if (!m.launcher.open) return {};
    for (std::size_t i = 0; i < m.launcher.shown.size(); ++i)
        if (c.node == launcher_row(i)) {
            m.launcher.selected = i;
            return launch(m);
        }
    return {};
}

Cmd Head::update(Model& m, mayag::MouseMove e) {
    if (!m.replica.ready() || m.launcher.open) return {};
    return say(link::up::Motion{desktop(e.position)});
}

Cmd Head::update(Model& m, mayag::MouseDown e) {
    if (!m.replica.ready() || m.launcher.open) return {};
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

Cmd Head::update(Model& m, ClearNotice) {
    m.notice.clear();
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
    auto frame = head::view(m.arranged, m.motion, hud, m.textures);
    if (m.launcher.open) frame.add_child(launcher_view(m.launcher, m.apps, m.screen));
    if (!m.notice.empty()) frame.add_child(notice_view(m.notice, m.screen));
    return frame;
}

}  // namespace i4::head
