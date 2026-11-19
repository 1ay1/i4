// src/app/watch.cpp — i4-watch: attach to a session as a head.
//
//   i4-watch [--link PATH]                 print the tree on every change
//   i4-watch [--link PATH] --run "CMDS"    i4-msg: run i3 commands, exit
//   i4-watch [--link PATH] --key CODE      type an evdev key into the focus
//
// The smallest real head: it says Hello, keeps a link::Replica of the
// session, and prints the workspaces in i3 notation each time the window
// manager changes. With --run it parses the commands HERE (a typo never
// reaches the session), sends them as one typed Request, and exits when the
// session has applied it. It acknowledges every frame it is sent (so the
// session's clients keep drawing) without drawing anything.
//
// A jaal program on a jaal host, like everything else in i4: the host owns
// the socket and turns packets into events; the program is pure.

#include <jaal/jaal.hpp>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "../transport/channel.hpp"
#include "../transport/sendable.hpp"
#include "i4/base/match.hpp"
#include "i4/link/predict.hpp"
#include "i4/link/protocol.hpp"
#include "i4/wm/arrange.hpp"

namespace {

using namespace i4;

// -- what the host and program say ------------------------------------------------

struct Heard { link::Down message; };
struct Lost {};
using Event = std::variant<Heard, Lost>;

struct Say { link::Up message; };
using do_say = jaal::pure_fx<Say, "watch.say">;
struct Print { std::string line; };
using do_print = jaal::pure_fx<Print, "watch.print">;

/// --run, parsed before the program starts (jaal's init takes no arguments).
std::optional<wm::Script> g_run;
std::vector<std::uint32_t> g_keys;

// -- the program --------------------------------------------------------------------

struct Watch {
    struct Model {
        link::Replica replica;
        std::string last;
        /// --run: the commands, until they are sent; then what to wait for.
        std::optional<wm::Script> to_run;
        std::optional<link::Nonce> waiting;
        /// --key: evdev codes to type into the focused window.
        std::vector<std::uint32_t> keys;
    };
    using Msg = Event;
    using Cmd = jaal::Cmd<Msg, do_say, do_print>;

    static Cmd init(Model& m) {
        m.to_run = std::move(g_run);
        m.keys = std::move(g_keys);
        return Say{link::Replica::hello()};
    }

    static Cmd update(Model& m, Heard h) {
        std::vector<Cmd> out;
        if (const auto* c = std::get_if<link::down::Content>(&h.message))
            out.push_back(Say{link::up::Shown{c->window, c->generation}});
        const bool ours = m.waiting && is<link::down::Applied>(h.message) &&
                          std::get<link::down::Applied>(h.message).by == m.waiting;
        if (auto ok = m.replica.receive(h.message); !ok) {
            out.push_back(Print{"i4-watch: the session refused this head (another build?)"});
            out.push_back(Cmd::quit(1));
            return Cmd::batch(std::move(out));
        }
        if (m.to_run && m.replica.ready()) {
            auto req = m.replica.act(wm::ev::Run{std::move(*m.to_run)});
            m.to_run.reset();
            m.waiting = req.nonce;
            out.push_back(Say{std::move(req)});
        }
        if (!m.keys.empty() && m.replica.ready()) {
            // To the focused client, exactly as a head's keyboard would.
            for (auto code : m.keys) {
                out.push_back(Say{link::up::Key{code, true, {}}});
                out.push_back(Say{link::up::Key{code, false, {}}});
            }
            m.keys.clear();
            out.push_back(Cmd::quit(0));
            return Cmd::batch(std::move(out));
        }
        const auto& s = m.replica.shown();
        std::string line;
        for (const auto& [id, w] : s.workspaces) {
            const bool focused = s.focus && s.cons.at(*s.focus).workspace == id;
            line += (focused ? "[" : " ") + w.name + (focused ? "]" : " ") + " " + wm::shape(s, id) + "   ";
        }
        if (ours) {
            out.push_back(Print{line});
            out.push_back(Cmd::quit(0));
        } else if (!m.waiting && line != m.last) {
            m.last = line;
            out.push_back(Print{line});
        }
        return out.empty() ? Cmd::none() : Cmd::batch(std::move(out));
    }
    static Cmd update(Model&, Lost) {
        return Cmd::batch(Cmd{Print{"i4-watch: session gone"}}, Cmd::quit(0));
    }

    template <class E>
        requires requires(Model& m, E e) { update(m, std::move(e)); }
    static std::optional<Msg> on(const Model&, const E& e) {
        return Msg{e};
    }
};

// -- the host ---------------------------------------------------------------------------

class Host {
public:
    using event_type = Event;
    explicit Host(transport::Channel ch) : ch_(std::move(ch)) {}

    template <class Cx>
    void attach(Cx& cx) {
        if (auto r = cx.watch(ch_.fd(), jaal::interest::read, 1)) reg_.emplace(std::move(*r));
    }
    template <class Cx>
    void on_ready(Cx& cx, const jaal::readiness& r) {
        if (r.writable) (void)ch_.flush();
        auto packets = ch_.receive();
        if (!packets) {
            (void)cx.emit(Event{Lost{}});
            return;
        }
        for (auto& p : *packets)
            if (auto d = link::decode<link::Down>(p.bytes)) (void)cx.emit(Event{Heard{std::move(*d)}});
    }
    template <class K>
    void present(K&) {
        const bool want = ch_.wants_write();
        if (reg_ && want != writing_ && reg_->modify(want ? jaal::interest::read_write : jaal::interest::read))
            writing_ = want;
    }
    void release() { reg_.reset(); }

    void handle(const Say& s) { (void)ch_.send(link::encode(s.message)); }
    void handle(const Print& p) {
        std::printf("%s\n", p.line.c_str());
        std::fflush(stdout);
    }

private:
    transport::Channel ch_;
    std::optional<jaal::platform::native_reactor::registration> reg_;
    bool writing_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(runtime ? runtime : "/tmp") + "/i4-link";
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--link") {
            path = argv[++i];
        } else if (a == "--run") {
            auto s = i4::wm::parse(argv[++i]);
            if (!s) {
                std::fprintf(stderr, "i4-watch: at %zu: %s\n", s.error().offset, s.error().message.c_str());
                return 2;
            }
            g_run = std::move(*s);
        } else if (a == "--key") {
            g_keys.push_back(static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10)));
        }
    }
    auto ch = i4::transport::Channel::connect(path);
    if (!ch) {
        std::fprintf(stderr, "i4-watch: no session at %s\n", path.c_str());
        return 1;
    }
    Host host{std::move(*ch)};
    return jaal::run<Watch>(host);
}
