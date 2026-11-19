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
#include "launcher.hpp"
#include "scene.hpp"

namespace i4::head {

// -- what the link host emits --------------------------------------------------

/// A decoded message from the session. For Content, the frame the host
/// prepared: copied pixels (shm), or a GPU buffer the host already imported
/// into this window's texture (dmabuf, zero copy).
struct Heard {
    link::Down message;
    std::optional<jaal::shared<mayag::ImageData>> pixels;
    /// The dmabuf is on the GPU as this texture (imported once per client
    /// buffer, then rebound): nothing to upload. 0 = not imported.
    std::uint32_t texture = 0;
    /// Remote frames: only what changed since the last one, uploaded in
    /// place (the whole picture rides along for when that cannot happen).
    std::optional<jaal::shared<mayag::PatchData>> patch = std::nullopt;
};
/// The GPU has finished sampling frames up to `generation` of `window`.
struct GpuDone {
    wm::WindowId window;
    std::uint64_t generation;
};
/// The GPU came up (mayag starts on the CPU and adopts it when ready), and
/// this is what it can sample in place.
struct GpuReady {
    std::vector<link::up::Format> formats;
};
/// The session went away.
struct Lost {};
/// Video from the session decodes in software here (the platform refused
/// it): resend Caps saying so, so the session sends tiles instead.
struct SoftwareVideo {};

// The seat itself, where the platform has a raw tap (Wayland): every key
// press AND release by evdev code, the exact xkb state, and scroll as the
// axis stream it is. Clients get these unchanged, so a held key is held,
// modifiers are the real masks, and the layout is the one the user typed on.

/// A key went down or up. `key` is mayag's reading of it (for bindings).
struct KeyInput {
    std::uint32_t code;
    bool pressed;
    mayag::KeyEvent key;
};
/// The platform's keymap (XKB text v1).
struct KeymapInput {
    std::string keymap;
};
/// The platform's key repeat.
struct RepeatInput {
    std::int32_t rate, delay;
};
/// The exact xkb modifier state.
struct ModsInput {
    link::up::Modifiers mods;
};
/// The head window lost the keyboard: nothing is held any more.
struct KeyboardLeft {};
/// One pointer frame of scrolling.
struct ScrollInput {
    link::up::Scroll scroll;
};

/// Everything the head's program hears: mayag's window, then the link.
using Events = std::variant<mayag::KeyEvent, mayag::TextEvent, mayag::ComposeEvent, mayag::ComposeEndEvent,
                            mayag::MouseMove, mayag::MouseDown, mayag::MouseUp, mayag::ScrollEvent,
                            mayag::ResizeEvent, mayag::FocusEvent, mayag::CloseRequest, mayag::Clicked,
                            mayag::Activated, mayag::Pressed, mayag::Released, mayag::Entered, mayag::Left,
                            mayag::Dragged, mayag::Scrolled, Heard, GpuDone, GpuReady, Lost, KeyInput, KeymapInput,
                            RepeatInput, ModsInput, KeyboardLeft, ScrollInput, SoftwareVideo>;

// -- what the program asks the host for -------------------------------------------

struct Say {
    link::Up message;
};
using do_say = jaal::pure_fx<Say, "head.say">;
/// A window is gone: drop every GPU buffer imported for it.
struct Forget {
    wm::WindowId window;
};
using do_forget = jaal::pure_fx<Forget, "head.forget">;
/// Show the pointer like this, on the platform's own cursor (zero added
/// latency: the pointer never waits for a frame or the link).
struct ShowCursor {
    link::down::Cursor cursor;
};
using do_cursor = jaal::pure_fx<ShowCursor, "head.cursor">;

// -- the program -----------------------------------------------------------------------

struct Options {
    wm::Mod mod = wm::Mod::alt;
    std::string terminal = "foot";
    /// Offer clients GPU buffers at all (I4_NO_DMABUF=1 turns it off).
    bool zero_copy = true;
    /// Attached across a network (--link tcp:...): frames arrive as bytes.
    bool remote = false;
};

/// Read once by init(): jaal's init takes no arguments.
inline Options options;

struct Head {
    struct Model {
        link::Replica replica;
        wm::Bindings bindings;
        wm::Mod mod = wm::Mod::alt;
        Size<space::Desktop> screen{1280, 800};
        /// The screen's backing scale (2 on Retina), from the window.
        std::int32_t scale = 1;
        /// What up::Caps said last (resent with a new scale), once the GPU is up.
        std::optional<link::up::Caps> caps;
        bool lost = false;

        /// Each window's newest frame: the texture it is uploaded as, and
        /// the generation, for the Shown that paces the client.
        std::map<wm::WindowId, std::uint32_t> textures;

        wm::Arrangement arranged;
        Motion motion;

        /// $mod+d: the session's apps, and the launcher over them.
        std::vector<link::App> apps;
        Launcher launcher;
        std::string terminal;
        /// mayag reports a printable key twice: the KeyEvent, then its
        /// TextEvent. When the KeyEvent was a shortcut ($mod+d), the text
        /// that follows is part of it, not typing: this is that key's name,
        /// and a TextEvent equal to it (any case) is dropped. Every KeyEvent
        /// resets it, so it can never eat the text of a different key.
        std::string swallow;
        /// A notice from the session, shown until a timer clears it.
        std::string notice;

        /// The platform gives real key-up and xkb state (Wayland's raw tap):
        /// clients get the seat as it is. Without it, a key to a client is a
        /// press and an immediate release.
        bool raw_keyboard = false;
        /// This head's keymap and repeat rate, as clients should get them.
        link::up::Keyboard keyboard;
        /// Keys sent to clients as pressed and not yet released; and keys a
        /// binding took, whose release must not reach a client either.
        std::set<std::uint32_t> held, taken;
    };

    struct Tick {
        double dt;
    };
    struct ClearNotice {};
    using Msg = std::variant<Heard, GpuDone, GpuReady, Lost, mayag::KeyEvent, mayag::TextEvent, mayag::Clicked, mayag::MouseMove,
                             mayag::MouseDown, mayag::MouseUp, mayag::ResizeEvent, Tick, ClearNotice,
                             mayag::CloseRequest, KeyInput, KeymapInput, RepeatInput, ModsInput, KeyboardLeft,
                             ScrollInput, mayag::ScrollEvent, SoftwareVideo>;
    using Cmd = mayag::Cmd<Msg, do_say, do_forget, do_cursor, mayag::upload_image, mayag::patch_image, mayag::release_image,
                           mayag::set_title>;
    using Sub = mayag::Sub<Msg>;

    static Cmd init(Model& m);
    static Cmd update(Model& m, Heard h);
    static Cmd update(Model& m, GpuDone d);
    static Cmd update(Model& m, GpuReady g);
    static Cmd update(Model& m, Lost);
    static Cmd update(Model& m, SoftwareVideo);
    static Cmd update(Model& m, KeyInput k);
    static Cmd update(Model& m, KeymapInput k);
    static Cmd update(Model& m, RepeatInput r);
    static Cmd update(Model& m, ModsInput k);
    static Cmd update(Model& m, KeyboardLeft);
    static Cmd update(Model& m, ScrollInput s);
    static Cmd update(Model& m, mayag::ScrollEvent s);
    static Cmd update(Model& m, mayag::KeyEvent k);
    static Cmd update(Model& m, mayag::TextEvent t);
    static Cmd update(Model& m, mayag::Clicked c);
    static Cmd update(Model& m, mayag::MouseMove e);
    static Cmd update(Model& m, mayag::MouseDown e);
    static Cmd update(Model& m, mayag::MouseUp e);
    static Cmd update(Model& m, mayag::ResizeEvent e);
    static Cmd update(Model& m, Tick t);
    static Cmd update(Model& m, ClearNotice);
    static Cmd update(Model& m, mayag::CloseRequest);

    template <class E>
        requires std::constructible_from<Msg, E>
    static std::optional<Msg> on(const Model&, const E& e) {
        return Msg{e};
    }

    static Sub subscribe(const Model& m);
    static mayag::Node view(const Model& m);
};

/// A window's texture id: fixed, so the host (which imports GPU buffers
/// into it) and the view agree without asking each other. Offset so it
/// never meets ids the DSL uses for its own images.
[[nodiscard]] constexpr std::uint32_t texture_of(wm::WindowId w) noexcept { return 0x1000'0000u + w.value; }

}  // namespace i4::head
