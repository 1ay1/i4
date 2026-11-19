#pragma once
// src/head/head.hpp — i4-head: a session, drawn by mayag, driven from here.
//
//   i4d --link L  <──link──>  i4-head --link L
//
// Like every i4 binary, a jaal program on a jaal host:
//
//   Head (the program, head.cpp — pure)
//       a link::Replica of the session, a texture per window, the springs
//       that animate tiles, and every input decision: a key that matches a
//       binding becomes a predicted Request (it shows in the same frame,
//       before the server has seen it); any other key, and the pointer, go
//       to the session for the focused client.
//
//   Host (host.hpp — the shell) = mayag's window_host + the link socket
//       mayag owns the window, the GPU and the frame clock; the host adds
//       the link fd to the same jaal reactor, maps each frame's shared
//       memory into an image, and sends what the program says.
//
// The window IS the output: its size goes to the session (as a predicted
// `OutputChanged`), which lays the desktop out to fit it.

#include <mayag/mayag.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>

#include "../transport/sendable.hpp"
#include "i4/link/predict.hpp"
#include "i4/link/protocol.hpp"
#include "i4/wm/bindings.hpp"
#include "scene.hpp"

namespace i4::head {

// -- what the link host emits --------------------------------------------------

/// A decoded message from the session. For Content, `pixels` is the frame,
/// already converted by the host (the fd is closed by then: never held).
struct Heard {
    link::Down message;
    std::optional<jaal::shared<mayag::ImageData>> pixels;
};
/// The session went away.
struct Lost {};

/// Everything the head's program hears: mayag's window, then the link.
using Events = std::variant<mayag::KeyEvent, mayag::TextEvent, mayag::ComposeEvent, mayag::ComposeEndEvent,
                            mayag::MouseMove, mayag::MouseDown, mayag::MouseUp, mayag::ScrollEvent,
                            mayag::ResizeEvent, mayag::FocusEvent, mayag::CloseRequest, mayag::Clicked,
                            mayag::Activated, mayag::Pressed, mayag::Released, mayag::Entered, mayag::Left,
                            mayag::Dragged, mayag::Scrolled, Heard, Lost>;

// -- what the program asks the host for -------------------------------------------

struct Say {
    link::Up message;
};
using do_say = jaal::pure_fx<Say, "head.say">;

// -- the program -----------------------------------------------------------------------

struct Options {
    wm::Mod mod = wm::Mod::alt;
    std::string terminal = "foot";
};

/// Read once by init(): jaal's init takes no arguments.
inline Options options;

struct Head {
    struct Model {
        link::Replica replica;
        wm::Bindings bindings;
        wm::Mod mod = wm::Mod::alt;
        Size<space::Desktop> screen{1280, 800};
        bool lost = false;

        /// Each window's newest frame: the texture it is uploaded as, and
        /// the generation, for the Shown that paces the client.
        std::map<wm::WindowId, std::uint32_t> textures;
        std::uint32_t next_texture = 1;

        wm::Arrangement arranged;
        Motion motion;
    };

    struct Tick {
        double dt;
    };
    using Msg = std::variant<Heard, Lost, mayag::KeyEvent, mayag::MouseMove, mayag::MouseDown, mayag::MouseUp,
                             mayag::ResizeEvent, Tick, mayag::CloseRequest>;
    using Cmd = mayag::Cmd<Msg, do_say, mayag::upload_image, mayag::release_image, mayag::set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m);
    static Cmd update(Model& m, Heard h);
    static Cmd update(Model& m, Lost);
    static Cmd update(Model& m, mayag::KeyEvent k);
    static Cmd update(Model& m, mayag::MouseMove e);
    static Cmd update(Model& m, mayag::MouseDown e);
    static Cmd update(Model& m, mayag::MouseUp e);
    static Cmd update(Model& m, mayag::ResizeEvent e);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, mayag::CloseRequest);

    template <class E>
        requires std::constructible_from<Msg, E>
    static std::optional<Msg> on(const Model&, const E& e) {
        return Msg{e};
    }

    static Sub subscribe(const Model& m);
    static mayag::Node view(const Model& m);
};

}  // namespace i4::head
