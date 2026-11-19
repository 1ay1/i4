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
/// One `exec`: a process i4d started.
using LaunchId = Id<struct LaunchTag>;

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
/// fx::Spawn's answer: the process started (or could not).
struct Launched { LaunchId launch; std::string command; bool started; };
/// A launched process ended, and was reaped.
struct Exited { LaunchId launch; bool signalled; std::int32_t code; };
/// The client under the pointer changed how the pointer looks (or the
/// pointer moved to another client / to nothing: the default arrow).
struct CursorChanged { link::down::Cursor cursor; };
}  // namespace ev

using Event = std::variant<ev::OutputReady, ev::Mapped, ev::Retitled, ev::Unmapped, ev::Committed,
                           ev::HeadJoined, ev::HeadSaid, ev::HeadLeft, ev::AppsFound, ev::Launched, ev::Exited,
                           ev::CursorChanged>;

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
/// The keymap and repeat rate clients should use (the typing head's own).
struct Keyboard { link::up::Keyboard keyboard; };
/// Exact xkb modifier masks in that keymap.
struct Modifiers { link::up::Modifiers mods; };
/// One pointer frame of scrolling for whoever has the pointer.
struct Scroll { link::up::Scroll scroll; };
/// A link message to these heads.
struct Send { std::vector<HeadId> to; link::Down message; };
/// A window's current pixels to these heads (the host owns the buffer).
struct Share { std::vector<HeadId> to; wm::WindowId window; };
/// Frames up to `generation` have been shown: fire the frame callbacks.
struct FramesDone { wm::WindowId window; std::uint64_t generation; };
/// Every head is done reading frames up to `generation`: their buffers may
/// go back to the client (wl_buffer.release), the current one excepted.
struct Release { wm::WindowId window; std::uint64_t generation; };
/// What clients should render into: the GPU buffer layouts every attached
/// head can sample in place (linux-dmabuf feedback). Empty: shm only.
struct Formats { std::vector<link::up::Format> formats; };
/// Disconnect a head that broke the protocol.
struct Drop { HeadId head; };
/// Start a process in the session (answered by ev::Launched).
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
using do_keyboard = jaal::pure_fx<fx::Keyboard, "i4d.keyboard">;
using do_modifiers = jaal::pure_fx<fx::Modifiers, "i4d.modifiers">;
using do_scroll = jaal::pure_fx<fx::Scroll, "i4d.scroll">;
using do_send = jaal::pure_fx<fx::Send, "i4d.send">;
using do_share = jaal::pure_fx<fx::Share, "i4d.share">;
using do_frames_done = jaal::pure_fx<fx::FramesDone, "i4d.frames_done">;
using do_release = jaal::pure_fx<fx::Release, "i4d.release">;
using do_formats = jaal::pure_fx<fx::Formats, "i4d.formats">;
using do_drop = jaal::pure_fx<fx::Drop, "i4d.drop">;
using do_spawn = jaal::pure_fx<fx::Spawn, "i4d.spawn">;

}  // namespace i4::server

static_assert(jaal::Sendable<i4::server::Event>, "every event crosses into the program");
