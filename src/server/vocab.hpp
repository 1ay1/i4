#pragma once
// src/server/vocab.hpp — what i4d's host and its program say to each other.
//
// i4d is a jaal program (session.hpp) and a jaal host (host.hpp), and this
// is the whole conversation between them:
//
//   host  --event-->  Session::on(model, event) -> Msg -> update -> Cmd
//   host  <--effect-- the Cmd's effects, as values, carried out by handle()
//
// The PROGRAM is pure: the replicated window manager, every decision about
// who is told what, focus, pacing. No fd, no protocol object, no syscall.
// The HOST is the imperative shell: the Wayland display (i4::wl), each
// client's objects, the link sockets to heads. It turns what clients and
// heads do into the events below, and does what the effects below say.
//
// So everything i4d decides is testable with jaal::headless and no socket:
// send events, read the recorded effects (tests/test_session.cpp).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <jaal/jaal.hpp>

#include "../transport/sendable.hpp"
#include "i4/base/flags.hpp"
#include "i4/base/geom.hpp"
#include "i4/base/id.hpp"
#include "i4/link/protocol.hpp"
#include "i4/wm/state.hpp"

namespace i4::server {

/// A head attached over the link: one connection.
using HeadId = Id<struct HeadTag>;

// -- events: host -> program -------------------------------------------------

namespace ev {
/// The session's output, as configured (until heads report theirs).
struct OutputReady { wm::OutputInfo info; };
/// A toplevel's first commit: the client wants to be a window.
struct Mapped { wm::WindowId window; wm::WindowInfo info; };
/// Its title, app id or transient-ness changed.
struct Retitled { wm::WindowId window; wm::WindowInfo info; };
/// Destroyed, or its client went away.
struct Unmapped { wm::WindowId window; };
/// It committed new pixels: content generation `generation`.
struct Committed { wm::WindowId window; std::uint64_t generation; };
struct HeadJoined { HeadId head; };
/// A decoded message from a head (the host refuses undecodable ones).
struct HeadSaid { HeadId head; link::Up message; };
struct HeadLeft { HeadId head; };
/// What can be launched here (the .desktop files), scanned off the loop.
struct AppsFound { std::vector<link::App> apps; };
}  // namespace ev

using Event = std::variant<ev::OutputReady, ev::Mapped, ev::Retitled, ev::Unmapped, ev::Committed,
                           ev::HeadJoined, ev::HeadSaid, ev::HeadLeft, ev::AppsFound>;

// -- effects: program -> host ---------------------------------------------------
//
// Each is a jaal::pure_fx: a plain payload the host has a handle() for. A
// program that asks for an effect the host cannot run does not compile.

namespace fx {
/// Tell a window its size and whether it is the active one (xdg configure).
struct Configure { wm::WindowId window; Size<space::Desktop> size; bool activated; };
/// Ask a window to close (xdg_toplevel.close).
struct Close { wm::WindowId window; };
/// Keyboard enter/leave: this window, or none.
struct KeyboardFocus { std::optional<wm::WindowId> window; };
/// Under the pointer: this window at this point in it, or none.
struct PointerAt { std::optional<wm::WindowId> window; Point<space::Surface> at; };
/// Input for whoever has keyboard / pointer focus.
struct Key { std::uint32_t code; bool pressed; wm::Mods mods; };
struct Button { std::uint32_t button; bool pressed; };
/// A link message to these heads.
struct Send { std::vector<HeadId> to; link::Down message; };
/// A window's current pixels to these heads (the host owns the buffer).
struct Share { std::vector<HeadId> to; wm::WindowId window; };
/// Frames up to `generation` have been shown: fire the frame callbacks.
struct FramesDone { wm::WindowId window; std::uint64_t generation; };
/// Disconnect a head that broke the protocol.
struct Drop { HeadId head; };
struct Spawn { std::string command; };
}  // namespace fx

// The effect rows. Named `do_*` so they can never collide with POSIX
// functions (close, send) pulled in by any header.
using do_configure = jaal::pure_fx<fx::Configure, "i4d.configure">;
using do_close = jaal::pure_fx<fx::Close, "i4d.close">;
using do_keyboard_focus = jaal::pure_fx<fx::KeyboardFocus, "i4d.keyboard_focus">;
using do_pointer_at = jaal::pure_fx<fx::PointerAt, "i4d.pointer_at">;
using do_key = jaal::pure_fx<fx::Key, "i4d.key">;
using do_button = jaal::pure_fx<fx::Button, "i4d.button">;
using do_send = jaal::pure_fx<fx::Send, "i4d.send">;
using do_share = jaal::pure_fx<fx::Share, "i4d.share">;
using do_frames_done = jaal::pure_fx<fx::FramesDone, "i4d.frames_done">;
using do_drop = jaal::pure_fx<fx::Drop, "i4d.drop">;
using do_spawn = jaal::pure_fx<fx::Spawn, "i4d.spawn">;

}  // namespace i4::server

static_assert(jaal::Sendable<i4::server::Event>, "every event crosses into the program");
