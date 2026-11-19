// src/server/session.cpp — i4d's decisions.
#include "session.hpp"

#include "apps.hpp"
#include "i4/base/match.hpp"

namespace i4::server {

namespace {

using Cmd = Session::Cmd;
using Model = Session::Model;

std::vector<HeadId> welcomed(const Model& m) {
    std::vector<HeadId> out;
    for (const auto& [id, h] : m.heads)
        if (h.welcomed) out.push_back(id);
    return out;
}

/// After the State changed: tell clients what arrange() now says about them.
void sync(Model& m, std::vector<Cmd>& out) {
    m.arranged = wm::arrange(m.session.state());
    auto tell = [&](const wm::Tile& t, bool shown) {
        const Session::Told now{t.content.size, shown && m.arranged.keyboard_focus == t.window};
        auto it = m.told.find(t.window);
        if (it != m.told.end() && it->second == now) return;
        m.told[t.window] = now;
        out.push_back(fx::Configure{t.window, now.size, now.activated});
    };
    for (const auto& sc : m.arranged.screens)
        for (const auto& t : sc.tiles) tell(t, true);
    for (const auto& t : m.arranged.hidden) tell(t, false);

    if (m.arranged.keyboard_focus != m.keyboard) {
        m.keyboard = m.arranged.keyboard_focus;
        out.push_back(fx::KeyboardFocus{m.keyboard});
    }
}

/// Apply one event to the truth: broadcast it, carry out its effects, sync.
void step(Model& m, wm::Event e, std::optional<link::Nonce> by, std::vector<Cmd>& out) {
    auto s = m.session.apply(std::move(e), by);
    if (auto to = welcomed(m); !to.empty()) out.push_back(fx::Send{std::move(to), link::Down{std::move(s.broadcast)}});
    for (const auto& effect : s.outcome.effects)
        match(
            effect, [&](const wm::fx::Spawn& sp) { out.push_back(fx::Spawn{sp.command}); },
            [&](const wm::fx::Close& c) { out.push_back(fx::Close{c.window}); });
    sync(m, out);
}

Cmd done(std::vector<Cmd> out) { return out.empty() ? Cmd::none() : Cmd::batch(std::move(out)); }

/// The window under a desktop point, top-most first, and the point in it.
fx::PointerAt hit(const wm::Arrangement& a, Point<space::Desktop> at) {
    for (const auto& sc : a.screens)
        for (auto t = sc.tiles.rbegin(); t != sc.tiles.rend(); ++t)
            if (t->content.contains(at)) {
                const auto d = at - t->content.origin;
                return {t->window, Point<space::Surface>{d.x, d.y}};
            }
    return {std::nullopt, {}};
}

}  // namespace

Cmd Session::init(Model&) { return Cmd::perform<ev::AppsFound>(&scan_apps); }

Cmd Session::update(Model& m, ev::AppsFound e) {
    m.apps = std::move(e.apps);
    if (auto to = welcomed(m); !to.empty()) return fx::Send{std::move(to), link::Down{link::down::Apps{m.apps}}};
    return {};
}

Cmd Session::update(Model& m, ev::OutputReady e) {
    std::vector<Cmd> out;
    step(m, wm::ev::OutputAdded{std::move(e.info)}, std::nullopt, out);
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::Mapped e) {
    std::vector<Cmd> out;
    step(m, wm::ev::WindowMapped{e.window, std::move(e.info)}, std::nullopt, out);
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::Retitled e) {
    std::vector<Cmd> out;
    step(m, wm::ev::WindowChanged{e.window, std::move(e.info)}, std::nullopt, out);
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::Unmapped e) {
    m.told.erase(e.window);
    m.generation.erase(e.window);
    if (m.pointer == e.window) m.pointer.reset();
    std::vector<Cmd> out;
    step(m, wm::ev::WindowUnmapped{e.window}, std::nullopt, out);
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::Committed e) {
    m.generation[e.window] = e.generation;
    auto to = welcomed(m);
    // Nobody to show it to: the frame is as shown as it will ever be.
    if (to.empty()) return fx::FramesDone{e.window, e.generation};
    return fx::Share{std::move(to), e.window};
}

Cmd Session::update(Model& m, ev::HeadJoined e) {
    m.heads[e.head] = Head{};
    return {};
}

Cmd Session::update(Model& m, ev::HeadLeft e) {
    m.heads.erase(e.head);
    if (!welcomed(m).empty()) return {};
    // The last head is gone: release every client waiting on a frame.
    std::vector<Cmd> out;
    for (const auto& [w, g] : m.generation) out.push_back(fx::FramesDone{w, g});
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::HeadSaid e) {
    if (!m.heads.contains(e.head)) return {};
    auto reply = m.session.receive(e.message);
    std::vector<Cmd> out;

    if (reply.to_sender) {
        const bool welcome = is<link::down::Welcome>(*reply.to_sender);
        out.push_back(fx::Send{{e.head}, std::move(*reply.to_sender)});
        if (!welcome) {
            out.push_back(fx::Drop{e.head});
            m.heads.erase(e.head);
            return done(std::move(out));
        }
        m.heads[e.head].welcomed = true;
        // What it can launch, then every window's current pixels.
        out.push_back(fx::Send{{e.head}, link::Down{link::down::Apps{m.apps}}});
        for (const auto& [w, g] : m.generation) out.push_back(fx::Share{{e.head}, w});
    }
    if (reply.step) {
        auto& s = *reply.step;
        if (auto to = welcomed(m); !to.empty()) out.push_back(fx::Send{std::move(to), link::Down{s.broadcast}});
        for (const auto& effect : s.outcome.effects)
            match(
                effect, [&](const wm::fx::Spawn& sp) { out.push_back(fx::Spawn{sp.command}); },
                [&](const wm::fx::Close& c) { out.push_back(fx::Close{c.window}); });
        sync(m, out);
    }
    if (reply.input)
        match(
            *reply.input, [&](const link::up::Key& k) { out.push_back(fx::Key{k.code, k.pressed, k.mods}); },
            [&](const link::up::Motion& mo) {
                const auto p = hit(m.arranged, mo.at);
                m.pointer = p.window;
                out.push_back(p);
            },
            [&](const link::up::Button& b) {
                // Click to focus, as in i3.
                if (b.pressed && m.pointer && m.arranged.keyboard_focus != m.pointer)
                    step(m, wm::ev::WindowActivated{*m.pointer}, std::nullopt, out);
                out.push_back(fx::Button{b.button, b.pressed});
            });
    if (reply.shown) out.push_back(fx::FramesDone{reply.shown->window, reply.shown->generation});
    return done(std::move(out));
}

}  // namespace i4::server
