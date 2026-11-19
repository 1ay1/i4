#pragma once
// i4/link/protocol.hpp — what the server and a head say to each other.
//
// The server streams EVENTS, not state. Every event it applies to its State
// it also sends, numbered, to each attached head; the head applies the same
// event to its replica with the same apply(), and arrives at the same State
// (wm::apply is deterministic, and every wm number is an integer — see
// wm/state.hpp). An event is a few dozen bytes; the State it changes, and the
// Arrangement drawn from it, are derived on the head for free.
//
// Sequence of a session:
//
//   head   -> Hello{protocol fingerprint}
//   server -> Welcome{state, seq}          the whole replica, once
//   server -> Applied{seq+1, event}        then every event, in order
//   head   -> Request{nonce, event}        a keypress, as a wm::Event
//   server -> Applied{..., by nonce}       ...comes back applied, in order
//
// A head that predicted Request #n (applied it locally ahead of time) learns
// the authoritative order when Applied{by = n} arrives — see predict.hpp.
//
// Both directions are plain variants of plain structs, so the codec, its
// totality, and the fingerprint that refuses mismatched builds all come from
// wire.hpp with no code here.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "i4/base/geom.hpp"
#include "i4/base/id.hpp"
#include "i4/link/wire.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/bindings.hpp"
#include "i4/wm/state.hpp"

namespace i4::link {

/// Position in the server's event log. 64 bits: it only grows.
using Seq = Id<struct SeqTag, std::uint64_t>;
/// A head's name for one of its requests, unique per connection.
using Nonce = Id<struct NonceTag, std::uint64_t>;

/// A launchable application on the session's machine.
struct App {
    std::string id;       // its desktop-file id: "org.mozilla.firefox"
    std::string name;     // "Firefox"
    std::string comment;  // "Browse the World Wide Web"
    std::string exec;     // a shell command, field codes already removed
    bool terminal = false;  // runs inside a terminal
    friend bool operator==(const App&, const App&) = default;
};

// -- head -> server ------------------------------------------------------------

namespace up {
struct Hello {
    std::uint64_t fingerprint;
    friend bool operator==(const Hello&, const Hello&) = default;
};
/// Run this on the authoritative State.
struct Request {
    Nonce nonce;
    wm::Event event;
    friend bool operator==(const Request&, const Request&) = default;
};
/// A key the head's bindings did not take: it goes to the focused window.
/// `code` is an evdev keycode, i4's one keyboard vocabulary on every head
/// platform (each head maps its OS's keys into it).
///
/// Presses and releases are separate, real events: a held key stays held
/// in the client until the head lets go of it. `mods` is the coarse state,
/// used only when the head has not sent its own Keyboard (then the
/// session's keymap derives the masks from it).
struct Key {
    std::uint32_t code;
    bool pressed;
    wm::Mods mods;
    friend bool operator==(const Key&, const Key&) = default;
};
/// The head's keyboard: its XKB keymap (text v1) and repeat rate. With it,
/// clients get exactly the layout the user typed on, and Modifiers below
/// are masks into this keymap, forwarded unchanged. Sent when it changes.
struct Keyboard {
    std::string keymap;
    std::int32_t rate = 25;    // keys per second; 0 = no repeat
    std::int32_t delay = 600;  // ms
    friend bool operator==(const Keyboard&, const Keyboard&) = default;
};
/// The exact xkb modifier state (depressed, latched, locked masks and the
/// layout group), in the head's Keyboard keymap.
struct Modifiers {
    std::uint32_t depressed, latched, locked, group;
    friend bool operator==(const Modifiers&, const Modifiers&) = default;
};
/// The pointer is here, in desktop pixels.
struct Motion {
    Point<space::Desktop> at;
    friend bool operator==(const Motion&, const Motion&) = default;
};
/// One pointer frame of scrolling, as the wl_pointer axis stream has it:
/// smooth deltas (wl_fixed, 24.8), high-resolution wheel steps (120 per
/// detent; 0 when the source is not a wheel), and "stopped" for kinetic
/// scrolling. `source` is wl_pointer.axis_source (0 wheel, 1 finger,
/// 2 continuous, 3 wheel tilt).
struct Scroll {
    std::uint32_t source = 0;
    std::int32_t dx = 0, dy = 0;
    std::int32_t dx120 = 0, dy120 = 0;
    bool stop_x = false, stop_y = false;
    friend bool operator==(const Scroll&, const Scroll&) = default;
};
/// An evdev button code (BTN_LEFT = 0x110).
struct Button {
    std::uint32_t button;
    bool pressed;
    friend bool operator==(const Button&, const Button&) = default;
};
/// The head has shown content `generation` of `window`. This is what paces
/// the client: its frame callbacks fire only now, so an app draws at the
/// rate the head (local or across a network) actually takes frames.
struct Shown {
    wm::WindowId window;
    std::uint64_t generation;
    friend bool operator==(const Shown&, const Shown&) = default;
};
/// The head no longer reads the memory of frames up to `generation`: the
/// client may write it again. For a copied frame that is at once; for a GPU
/// buffer the head samples in place, only once the GPU has finished with it.
struct Released {
    wm::WindowId window;
    std::uint64_t generation;
    friend bool operator==(const Released&, const Released&) = default;
};
/// A GPU buffer layout the head can sample in place: a DRM fourcc and
/// format modifier.
struct Format {
    std::uint32_t fourcc;
    std::uint64_t modifier;
    friend bool operator==(const Format&, const Format&) = default;
};
/// What the head can show without copying, sent once after Hello. The
/// session offers clients exactly this (linux-dmabuf), so an app renders
/// into a buffer the presenter can scan or sample as it is.
struct Caps {
    std::vector<Format> formats;
    friend bool operator==(const Caps&, const Caps&) = default;
};
}  // namespace up

using Input = std::variant<up::Key, up::Keyboard, up::Modifiers, up::Motion, up::Button, up::Scroll>;
using Up = std::variant<up::Hello, up::Request, up::Key, up::Motion, up::Button, up::Shown, up::Released, up::Caps,
                        up::Keyboard, up::Modifiers, up::Scroll>;

// -- server -> head ------------------------------------------------------------

namespace down {
struct Welcome {
    Seq seq;
    wm::State state;
    friend bool operator==(const Welcome&, const Welcome&) = default;
};
/// The fingerprints differ: this head was built against another protocol.
struct Refused {
    std::uint64_t server_fingerprint;
    friend bool operator==(const Refused&, const Refused&) = default;
};
struct Applied {
    Seq seq;
    wm::Event event;
    /// Set when this event is the head's own Request, coming back.
    std::optional<Nonce> by;
    friend bool operator==(const Applied&, const Applied&) = default;
};
/// A window's pixels changed. The pixels themselves are NOT in the message:
/// a transport carries them beside it (locally the client's own memory,
/// passed as fds, zero bytes on the wire; remotely, compressed tiles). This
/// is the data plane, kept out of the replicated log on purpose: a frame is
/// never part of the window manager's state.
/// CPU memory (wl_shm): one fd; 32-bit little-endian B,G,R,A words.
struct ShmLayout {
    std::int32_t offset;
    std::int32_t stride;
    /// argb8888: alpha is meaningful (premultiplied); xrgb8888: padding.
    bool alpha;
    friend bool operator==(const ShmLayout&, const ShmLayout&) = default;
};
/// A GPU buffer (linux-dmabuf): one fd per plane, in order.
struct DmabufLayout {
    struct Plane {
        std::uint32_t offset;
        std::uint32_t stride;
        friend bool operator==(const Plane&, const Plane&) = default;
    };
    /// The client buffer, stable for its life and never reused: clients
    /// cycle two or three buffers, so a head imports each ONCE and rebinds.
    std::uint64_t buffer;
    std::uint32_t fourcc;
    std::uint64_t modifier;
    std::vector<Plane> planes;
    friend bool operator==(const DmabufLayout&, const DmabufLayout&) = default;
};
/// Pixels in the message itself, for a head across a network (no fds can
/// get there). Lossless: the frame is cut into `tile` x `tile` squares and
/// only those that changed since the last frame THIS head was sent travel,
/// zstd-compressed. A keystroke in a terminal is a few hundred bytes.
///
/// `tiles` are tile indices, row-major over ceil(width/tile) columns, rising.
/// `data` (once decompressed) is each listed tile's pixels in that order,
/// its rows tight (edge tiles are narrower/shorter), as 32-bit little-endian
/// B,G,R,A words like ShmLayout. `keyframe`: every tile is listed and the
/// head need not have seen any earlier frame (a new head, a resize).
struct Encoded {
    std::uint32_t tile = 64;
    bool alpha = true;
    bool keyframe = true;
    std::vector<std::uint32_t> tiles;
    std::uint32_t raw_size = 0;  // decompressed bytes
    std::string data;            // zstd
    [[nodiscard]] bool valid() const { return tile >= 8 && tile <= 512 && raw_size <= (256u << 20); }
    friend bool operator==(const Encoded&, const Encoded&) = default;
};
/// A frame is sent only once it is READY: a client using explicit sync
/// (linux-drm-syncobj) has its acquire point waited for by the session, and
/// its release point signalled by the session once every head has released
/// the frame. A head never sees a client's timelines (a remote one could not
/// use them, and drivers will not import another driver's syncobjs anyway).
struct Content {
    wm::WindowId window;
    std::uint64_t generation;
    Size<space::Buffer> size;
    std::variant<ShmLayout, DmabufLayout, Encoded> layout;
    friend bool operator==(const Content&, const Content&) = default;
};
/// What can be launched on the session's machine (its .desktop files).
/// Sent at Welcome and whenever it changes. A head's launcher lists THESE,
/// not its own: the apps run where the session runs.
struct Apps {
    std::vector<App> apps;
    friend bool operator==(const Apps&, const Apps&) = default;
};
/// Something the person at the head should know, said once (shown briefly).
struct Notice {
    std::string text;
    friend bool operator==(const Notice&, const Notice&) = default;
};

// The pointer's look, as the client under it asked. The head shows it on
// its own platform cursor (a hardware plane where there is one), so the
// pointer moves at the speed of the machine the head runs on and never
// waits on the link.

/// A named shape: wp_cursor_shape_device_v1.shape, the protocol's enum
/// value (1 = default ... 34 = zoom-out). The head's platform draws it
/// from the user's own theme.
struct CursorShape {
    std::uint32_t shape = 1;
    friend bool operator==(const CursorShape&, const CursorShape&) = default;
};
/// An image the client drew (wl_pointer.set_cursor): ARGB8888, premultiplied,
/// tightly packed (stride = 4 * width), hotspot in its pixels.
struct CursorImage {
    std::int32_t width = 0, height = 0;
    std::int32_t hot_x = 0, hot_y = 0;
    std::string argb;
    /// Decoding refuses anything else: a head never sizes a cursor from a
    /// length it did not check.
    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 && width <= 512 && height <= 512 &&
               argb.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    }
    friend bool operator==(const CursorImage&, const CursorImage&) = default;
};
/// No cursor (a game, a video playing).
struct CursorHidden {
    friend bool operator==(const CursorHidden&, const CursorHidden&) = default;
};
struct Cursor {
    std::variant<CursorShape, CursorImage, CursorHidden> look;
    friend bool operator==(const Cursor&, const Cursor&) = default;
};
}  // namespace down

using Down = std::variant<down::Welcome, down::Refused, down::Applied, down::Content, down::Apps, down::Notice,
                          down::Cursor>;

// -- the protocol's identity --------------------------------------------------------
//
// Both message sets, their every field down to the last enum: change any of
// it and old and new builds refuse each other at Hello rather than misread.

inline constexpr std::uint64_t protocol = detail::mix(fingerprint<Up>, fingerprint<Down>);

static_assert(Wire<Up> && Wire<Down>);
static_assert(Wire<wm::State>, "the replica must cross the wire whole");

}  // namespace i4::link
