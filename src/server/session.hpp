#pragma once
// src/server/session.hpp — i4d's program: every decision, pure.
//
// The window manager's truth (link::Server: the State and its event log),
// what each window was last told, who has focus, which heads are attached.
// Its update functions read the model and return effects as values; the
// host (host.hpp) carries them out. Nothing here touches a socket, so all
// of it runs under jaal::headless in tests/test_session.cpp.
//
// The rules it keeps:
//   * every State change goes out to every welcomed head, in log order
//   * a window is configured only when arrange(state) says something new
//     about it, and only with what arrange says
//   * a frame callback fires only when a head has shown that frame (or at
//     once when no head is attached), so clients draw at the screen's pace

#include <map>
#include <optional>
#include <variant>

#include "i4/link/predict.hpp"
#include "i4/wm/arrange.hpp"
#include "vocab.hpp"

namespace i4::server {

struct Session {
    struct Told {
        Size<space::Desktop> size;
        bool activated;
        friend bool operator==(const Told&, const Told&) = default;
    };
    struct Head {
        bool welcomed = false;
    };

    struct Model {
        link::Server session;
        wm::Arrangement arranged;
        std::map<wm::WindowId, Told> told;
        std::map<wm::WindowId, std::uint64_t> generation;
        std::optional<wm::WindowId> keyboard;
        std::optional<wm::WindowId> pointer;
        std::map<HeadId, Head> heads;
        std::vector<link::App> apps;
    };

    using Msg = Event;
    using Cmd = jaal::Cmd<Msg, do_configure, do_close, do_keyboard_focus, do_pointer_at, do_key, do_button, do_send,
                          do_share, do_frames_done, do_drop, do_spawn>;

    /// Scan the .desktop files off the loop; they arrive as ev::AppsFound.
    static Cmd init(Model& m);

    static Cmd update(Model& m, ev::OutputReady e);
    static Cmd update(Model& m, ev::Mapped e);
    static Cmd update(Model& m, ev::Retitled e);
    static Cmd update(Model& m, ev::Unmapped e);
    static Cmd update(Model& m, ev::Committed e);
    static Cmd update(Model& m, ev::HeadJoined e);
    static Cmd update(Model& m, ev::HeadSaid e);
    static Cmd update(Model& m, ev::HeadLeft e);
    static Cmd update(Model& m, ev::AppsFound e);

    // The host's events are the program's messages, one for one.
    template <class E>
        requires requires(Model& m, E e) { update(m, std::move(e)); }
    static std::optional<Msg> on(const Model&, const E& e) {
        return Msg{e};
    }
};

}  // namespace i4::server
