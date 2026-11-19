// src/app/preview.cpp — i4-preview: the whole architecture in one window.
//
// A server, a link and a head, in one process, as mayag + jaal program:
//
//   keys --head::chord--> bindings --Replica::act--> uplink --> link::Server
//                                     (shown now)                   |
//   Node <--head::view-- Motion <--arrange-- Replica <-- downlink <--+
//
// Every message crosses the link ENCODED (wire.hpp) and waits out a
// simulated network delay (F1 cycles 0 / 50 / 150 / 400 ms). Watch the bar:
// at 400 ms, focus and moves still land in the frame you pressed the key,
// because the head predicts; the server's confirmation arrives later and
// changes nothing on screen. That is the remote-session promise, running.
//
// There are no Wayland clients yet (that is the server layer, next), so
// `exec` opens a stand-in window named after the command: $mod+Return gives
// you a "foot" tile. Everything else is real i4: the tree, the commands, the
// bindings, the protocol, the replica.
//
//   i4-preview              a window ($mod = Alt, so a host compositor's
//                           Super bindings don't fight it; --super to swap)
//   i4-preview --headless   self-test, no display (runs in ctest)

#include <mayag/mayag.hpp>

#include <cstdio>
#include <cstring>
#include <deque>
#include <variant>

#include "../head/keys.hpp"
#include "../head/scene.hpp"
#include "i4/link/predict.hpp"
#include "i4/link/protocol.hpp"
#include "i4/link/wire.hpp"
#include "i4/wm/bindings.hpp"

namespace {

using namespace i4;

/// Chosen before the program starts; jaal's init takes no arguments.
wm::Mod g_mod = wm::Mod::alt;

constexpr int latencies[] = {0, 50, 150, 400};

struct Preview {
    /// A message in flight on the simulated link: due at `due` seconds of
    /// model time, carrying the encoded bytes.
    struct Packet {
        double due;
        std::vector<std::byte> bytes;
    };

    struct Model {
        link::Server server;
        link::Replica replica;
        wm::Bindings bindings;
        wm::Mod mod = wm::Mod::alt;

        std::deque<Packet> uplink;    // head -> server
        std::deque<Packet> downlink;  // server -> head
        bool attached = false;        // the server has welcomed the head
        double now = 0.0;
        int latency_index = 0;
        std::uint32_t next_window = 1;
        std::string fault;            // a link error, shown instead of hidden

        wm::Arrangement arranged;     // of replica.shown()
        head::Motion motion;
    };

    struct Act { wm::Script script; };
    struct Resized { std::int32_t width; std::int32_t height; };
    struct Tick { double dt; };
    struct CycleLatency {};
    struct Quit {};
    using Msg = std::variant<Act, Resized, Tick, CycleLatency, Quit>;
    using Cmd = mayag::Cmd<Msg, mayag::set_title>;
    using Sub = mayag::Sub<Msg>;

    // -- the simulated link ----------------------------------------------------

    static double delay(const Model& m) { return latencies[m.latency_index] / 1000.0; }

    static void send_up(Model& m, const link::Up& u) { m.uplink.push_back({m.now + delay(m), link::encode(u)}); }

    static void send_down(Model& m, const link::Down& d) {
        m.downlink.push_back({m.now + delay(m), link::encode(d)});
    }

    /// Something happened on the server: apply, broadcast, carry out.
    static void server_event(Model& m, wm::Event e) { carry(m, m.server.apply(std::move(e))); }

    static void carry(Model& m, link::Server::Step step) {
        if (m.attached) send_down(m, step.broadcast);
        for (const auto& fx : step.outcome.effects) {
            if (const auto* spawn = std::get_if<wm::fx::Spawn>(&fx)) {
                const auto app = spawn->command.substr(0, spawn->command.find(' '));
                server_event(m, wm::ev::WindowMapped{wm::WindowId{m.next_window++},
                                                     wm::WindowInfo{app, app, std::nullopt, false}});
            } else if (const auto* close = std::get_if<wm::fx::Close>(&fx)) {
                server_event(m, wm::ev::WindowUnmapped{close->window});
            }
        }
    }

    /// Deliver everything due, in order, both ways, until nothing moves.
    static void pump(Model& m) {
        for (bool moved = true; moved;) {
            moved = false;
            while (!m.uplink.empty() && m.uplink.front().due <= m.now) {
                auto up = link::decode<link::Up>(m.uplink.front().bytes);
                m.uplink.pop_front();
                moved = true;
                if (!up) {
                    m.fault = "uplink: " + std::string(link::describe(up.error()));
                    continue;
                }
                auto reply = m.server.receive(*up);
                if (reply.to_sender) {
                    m.attached = std::holds_alternative<link::down::Welcome>(*reply.to_sender);
                    send_down(m, *reply.to_sender);
                }
                if (reply.step) carry(m, std::move(*reply.step));
            }
            while (!m.downlink.empty() && m.downlink.front().due <= m.now) {
                auto down = link::decode<link::Down>(m.downlink.front().bytes);
                m.downlink.pop_front();
                moved = true;
                if (!down) {
                    m.fault = "downlink: " + std::string(link::describe(down.error()));
                    continue;
                }
                if (auto ok = m.replica.receive(*down); !ok) m.fault = "replica refused a message";
            }
        }
        m.arranged = wm::arrange(m.replica.shown());
        m.motion.retarget(m.arranged);
    }

    // -- the program ---------------------------------------------------------

    static Cmd init(Model& m) {
        m.mod = g_mod;
        m.bindings = wm::default_bindings(m.mod, "foot");
        server_event(m, wm::ev::OutputAdded{wm::OutputInfo{"preview", {0, 0, 1280, 772}}});
        for (const char* app : {"foot", "firefox"})
            server_event(m, wm::ev::WindowMapped{wm::WindowId{m.next_window++}, wm::WindowInfo{app, app, {}, false}});
        send_up(m, link::Replica::hello());
        pump(m);
        return mayag::SetTitle{"i4 preview"};
    }

    static Cmd update(Model& m, Act a) {
        if (m.replica.ready()) send_up(m, m.replica.act(wm::ev::Run{std::move(a.script)}));
        pump(m);
        return {};
    }

    static Cmd update(Model& m, Resized r) {
        const auto h = std::max(1, r.height - static_cast<std::int32_t>(head::bar_height));
        server_event(m, wm::ev::OutputChanged{wm::OutputInfo{"preview", {0, 0, std::max(1, r.width), h}}});
        pump(m);
        return {};
    }

    static Cmd update(Model& m, Tick t) {
        m.now += t.dt;
        pump(m);
        m.motion.step(t.dt);
        return {};
    }

    static Cmd update(Model& m, CycleLatency) {
        m.latency_index = (m.latency_index + 1) % static_cast<int>(std::size(latencies));
        return {};
    }

    static Cmd update(Model&, Quit) { return Cmd::quit(); }

    static std::optional<Msg> on(const Model& m, const mayag::KeyEvent& k) {
        if (k.key == mayag::Key::f1 && k.mods.none()) return CycleLatency{};
        const auto c = head::chord(k);
        if (!c) return std::nullopt;
        if (*c == wm::Chord{m.mod | wm::Mod::shift, "e"}) return Quit{};
        if (const auto* s = wm::lookup(m.bindings, *c)) return Act{*s};
        return std::nullopt;
    }

    static std::optional<Msg> on(const Model&, const mayag::ResizeEvent& r) {
        return Resized{static_cast<std::int32_t>(r.size.x), static_cast<std::int32_t>(r.size.y)};
    }

    static std::optional<Msg> on(const Model&, const mayag::CloseRequest&) { return Quit{}; }

    static Sub subscribe(const Model& m) {
        const bool busy = m.motion.animating() || !m.uplink.empty() || !m.downlink.empty();
        return mayag::when(busy, Sub::on_frame([](double dt) -> Msg { return Tick{dt}; }));
    }

    static mayag::Node view(const Model& m) {
        head::Hud hud;
        hud.latency_ms = latencies[m.latency_index];
        hud.in_flight = m.replica.pending();
        hud.corrections = m.replica.corrections();
        hud.mod_name = m.mod == wm::Mod::super ? "Super" : "Alt";
        for (const auto& [id, o] : m.replica.shown().outputs)
            for (auto ws : o.workspaces) hud.workspaces.push_back(m.replica.shown().workspaces.at(ws).name);
        for (const auto& sc : m.arranged.screens) hud.focused_workspace = sc.workspace_name;
        if (!m.fault.empty()) hud.mod_name += "  ·  LINK FAULT: " + m.fault;
        return head::view(m.arranged, m.motion, hud);
    }
};

static_assert(jaal::Program<Preview>);

// -- the self-test ---------------------------------------------------------------

int self_test() {
    mayag::AppConfig cfg;
    cfg.size = {1280, 800};
    return mayag::run_headless<Preview>(std::move(cfg), [](auto& app) {
        int fails = 0;
        auto ok = [&](bool c, const char* what) {
            std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what);
            fails += c ? 0 : 1;
        };
        auto shape = [&] { return wm::shape(app.model().replica.shown()); };
        const mayag::Mods alt{.alt = true};
        const mayag::Mods alt_shift{.shift = true, .alt = true};

        app.tick();
        ok(app.model().replica.ready(), "the head attached to the server");
        ok(shape() == "H[foot firefox*]", "it shows the server's windows");

        app.press_key(mayag::Key::enter, alt);
        ok(shape() == "H[foot firefox foot*]", "$mod+Return opens a window");
        app.press_key(mayag::Key::h, alt);
        ok(shape() == "H[foot firefox* foot]", "$mod+h focuses left");
        app.press_key(mayag::Key::v, alt);
        app.press_key(mayag::Key::enter, alt);
        ok(shape() == "H[foot V[firefox foot*] foot]", "$mod+v then $mod+Return splits");
        app.press_key(mayag::Key::q, alt_shift);
        ok(shape() == "H[foot firefox* foot]", "$mod+Shift+q closes it, and the split dissolves");

        // Over a slow link the keypress still shows at once.
        app.press_key(mayag::Key::f1, {});
        app.press_key(mayag::Key::f1, {});
        app.press_key(mayag::Key::f1, {});  // 400 ms
        app.press_key(mayag::Key::l, alt);
        ok(shape() == "H[foot firefox foot*]", "at 400 ms, focus moves in the same frame");
        ok(app.model().replica.pending() == 1, "...while the server has not answered");
        ok(!(app.model().replica.shown() == app.model().replica.confirmed()), "...shown runs ahead of confirmed");
        app.send(Preview::Tick{0.5});
        ok(app.model().replica.pending() == 1, "after 500 ms the request reached the server, not yet the answer");
        app.send(Preview::Tick{0.5});
        ok(app.model().replica.pending() == 0, "the server's answer arrives after the round trip");
        ok(app.model().replica.shown() == app.model().server.state(), "and the head matches the server exactly");
        ok(app.model().replica.corrections() == 0, "with nothing to correct");
        ok(app.model().fault.empty(), "no link faults");
        std::printf("%s\n", fails == 0 ? "PASS" : "FAIL");
        return fails;
    });
}

}  // namespace

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        if (std::strcmp(argv[i], "--super") == 0) g_mod = i4::wm::Mod::super;
    }
    if (headless) return self_test();
    mayag::AppConfig cfg;
    cfg.title = "i4 preview";
    cfg.size = {1280, 800};
    return mayag::run<Preview>(std::move(cfg));
}
