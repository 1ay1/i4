// src/server/session.cpp — i4d's decisions.
#include "session.hpp"

#include <algorithm>

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

/// The newest generation of `w` that EVERY welcomed head has released: only
/// those buffers are free, since any head may still be sampling the rest.
std::uint64_t released_by_all(const Model& m, wm::WindowId w) {
    std::optional<std::uint64_t> low;
    for (const auto& [id, h] : m.heads) {
        if (!h.welcomed) continue;
        const auto it = h.released.find(w);
        const std::uint64_t g = it == h.released.end() ? 0 : it->second;
        low = low ? std::min(*low, g) : g;
    }
    return low.value_or(0);
}

/// Offer clients the GPU layouts every head that has said can sample in
/// place (each fourcc+modifier in all of them). A head that has not sent
/// its caps yet does not narrow the offer; with none known nothing changes.
void offer(Model& m, std::vector<Cmd>& out) {
    std::optional<std::vector<link::up::Format>> common;
    bool sync = true;
    for (const auto& [id, h] : m.heads) {
        if (!h.welcomed || !h.caps_known) continue;
        sync = sync && h.explicit_sync;
        if (!common) {
            common = h.formats;
            continue;
        }
        std::erase_if(*common, [&](const auto& f) { return std::ranges::find(h.formats, f) == h.formats.end(); });
    }
    if (!common) return;
    sync = sync && !common->empty();
    if (*common == m.offered && sync == m.offered_sync) return;
    m.offered = std::move(*common);
    m.offered_sync = sync;
    out.push_back(fx::Formats{m.offered, sync});
}

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

namespace {

/// The program's name, for a person: "google-chrome-stable" from
/// "/usr/bin/google-chrome-stable --incognito".
std::string program(const std::string& command) {
    auto first = command.substr(0, command.find(' '));
    if (auto slash = first.rfind('/'); slash != std::string::npos) first = first.substr(slash + 1);
    return first;
}

Cmd tell(const Model& m, std::string text) {
    auto to = welcomed(m);
    if (to.empty()) return {};
    return fx::Send{std::move(to), link::Down{link::down::Notice{std::move(text)}}};
}

}  // namespace

Cmd Session::update(Model& m, ev::Launched e) {
    if (!e.started) return tell(m, "could not start " + program(e.command));
    m.launches[e.launch] = {std::move(e.command), m.maps};
    return {};
}

Cmd Session::update(Model& m, ev::Exited e) {
    auto it = m.launches.find(e.launch);
    if (it == m.launches.end()) return {};
    const auto launch = std::move(it->second);
    m.launches.erase(it);
    if (m.maps != launch.maps_at_start) return {};  // something opened here: fine
    const auto name = program(launch.command);
    if (e.signalled) return tell(m, name + " was killed by signal " + std::to_string(e.code));
    if (e.code != 0) return tell(m, name + " failed (exit " + std::to_string(e.code) + ")");
    // Exited cleanly having opened nothing: almost always a single-instance
    // app (Chrome, Firefox, VS Code) passing the request to a copy already
    // running on another display.
    return tell(m, name + " opened nothing here: is it already running on another display?");
}

Cmd Session::update(Model& m, ev::OutputReady e) {
    std::vector<Cmd> out;
    step(m, wm::ev::OutputAdded{std::move(e.info)}, std::nullopt, out);
    return done(std::move(out));
}

Cmd Session::update(Model& m, ev::Mapped e) {
    ++m.maps;
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
    // Nobody to show it to: the frame is as shown, and as released, as it
    // will ever be.
    if (to.empty()) return Cmd::batch(Cmd{fx::FramesDone{e.window, e.generation}}, Cmd{fx::Release{e.window, e.generation}});
    return fx::Share{std::move(to), e.window};
}

Cmd Session::update(Model& m, ev::HeadJoined e) {
    m.heads[e.head] = Head{};
    return {};
}

Cmd Session::update(Model& m, ev::HeadLeft e) {
    m.heads.erase(e.head);
    std::vector<Cmd> out;
    if (welcomed(m).empty()) {
        // The last head is gone: nothing will show or read a frame again, so
        // every client is released from waiting and from its buffers.
        for (const auto& [w, g] : m.generation) {
            out.push_back(fx::FramesDone{w, g});
            out.push_back(fx::Release{w, g});
        }
    } else {
        // It may have been the one holding buffers back, or narrowing formats.
        for (const auto& [w, g] : m.generation) out.push_back(fx::Release{w, released_by_all(m, w)});
        offer(m, out);
    }
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
    if (reply.released) {
        auto& mine = m.heads[e.head].released[reply.released->window];
        mine = std::max(mine, reply.released->generation);
        out.push_back(fx::Release{reply.released->window, released_by_all(m, reply.released->window)});
    }
    if (reply.caps) {
        auto& h = m.heads[e.head];
        h.formats = std::move(reply.caps->formats);
        h.explicit_sync = reply.caps->explicit_sync;
        h.caps_known = true;
        offer(m, out);
    }
    return done(std::move(out));
}

}  // namespace i4::server
