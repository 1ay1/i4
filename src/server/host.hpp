#pragma once
// src/server/host.hpp — i4d's imperative shell: Wayland clients and heads.
//
// A jaal host. It owns every fd and every protocol object, turns what
// clients and heads do into server::Event, and carries out the Session's
// effects. It decides nothing: no layout, no focus, no pacing — those come
// in as effects (fx::Configure, fx::KeyboardFocus, fx::FramesDone).
//
// Protocol state is typed:
//   * a surface's state is double-buffered as two TYPES, Pending and
//     Current, and commit() is the only way between them
//   * a toplevel's life is a sum type, Unmapped | Mapped{WindowId}: the
//     window manager's id exists only while it knows the window
//   * every object is an i4::wl Resource<I, V>: an event the bound version
//     lacks, or a request sent from the server, does not compile
//
// Accepted but not yet composited (each is protocol i4::wl already speaks):
// subsurface and popup content, clipboard, layer-shell, dmabuf.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include <i4/wl/fd.hpp>
#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/proto/xdg_decoration_unstable_v1.hpp>
#include <i4/wl/proto/xdg_shell.hpp>
#include <i4/wl/proto/linux_dmabuf_v1.hpp>
#include <i4/wl/proto/linux_drm_syncobj_v1.hpp>
#include <i4/wl/server/display.hpp>
#include <jaal/jaal.hpp>

#include "../transport/channel.hpp"
#include "../transport/datagram.hpp"
#include "../remote/tiles.hpp"
#include "keyboard.hpp"
#include "encoder.hpp"
#include "syncobj.hpp"
#include "vocab.hpp"

namespace i4::server {

namespace srv = i4::wl::server;
using i4::wl::wire::id_t;

// -- buffers -----------------------------------------------------------------

/// A client's wl_shm_pool. The server never maps it: heads do.
struct Pool {
    wl::OwnedFd fd;
};

struct ShmBuffer {
    std::shared_ptr<Pool> pool;
    std::int32_t offset = 0, width = 0, height = 0, stride = 0;
    wl::proto::wl_shm::format format{};
};

/// A client's GPU buffer (linux-dmabuf). The server never touches the
/// memory either: its plane fds go to the head, which samples them where
/// they are.
struct DmabufBuffer {
    struct Plane {
        std::shared_ptr<wl::OwnedFd> fd;  // shared: planes may share one fd
        std::uint32_t offset = 0, stride = 0;
    };
    std::vector<Plane> planes;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::int32_t width = 0, height = 0;
    /// Stable for the buffer's life, never reused (see DmabufLayout::buffer).
    std::uint64_t id = 0;
};

using AnyBuffer = std::variant<ShmBuffer, DmabufBuffer>;

/// zwp_linux_buffer_params_v1, collecting planes until create.
struct Params {
    std::map<std::uint32_t, DmabufBuffer::Plane> planes;  // by plane index
    std::uint64_t modifier = 0;
    bool modifier_set = false;
    bool used = false;
};

// -- surfaces ------------------------------------------------------------------

/// A client timeline (linux-drm-syncobj), imported on i4d's render node.
/// Shared: commits waiting on it, or holding a release point on it, keep it
/// alive after the client destroys its object.
struct Timeline {
    Syncobjs* objs = nullptr;
    std::uint32_t handle = 0;
    Timeline(Syncobjs* o, std::uint32_t h) : objs(o), handle(h) {}
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;
    ~Timeline() { objs->destroy(handle); }
};

/// A point on a client timeline.
struct SyncPoint {
    std::shared_ptr<Timeline> timeline;
    std::uint64_t value = 0;
};

/// What a client has said since its last commit.
struct Pending {
    /// Outer: attach was called this cycle. Inner: the buffer, or detach.
    std::optional<std::optional<id_t>> attach;
    std::vector<id_t> frames;
    std::optional<SyncPoint> acquire, release;
    /// Rectangles (x, y, w, h) the client says changed.
    std::vector<std::array<std::int32_t, 4>> damage;
};

/// What the surface IS, as of its last commit.
struct Current {
    std::optional<id_t> buffer;
    /// Explicit sync for the committed buffer (both, or neither).
    std::optional<SyncPoint> acquire, release;
};

/// The one way from Pending to Current. Returns the frame callbacks this
/// commit asked for; `pending` is left empty.
[[nodiscard]] std::vector<id_t> commit(Current& current, Pending& pending);

namespace life {
struct Unmapped {};
struct Mapped {
    wm::WindowId window;
};
}  // namespace life
using Life = std::variant<life::Unmapped, life::Mapped>;

struct Toplevel {
    id_t xdg_surface = 0;
    id_t xdg_toplevel = 0;
    std::uint32_t version = 1;
    Life life = life::Unmapped{};
    wm::WindowInfo info;
    std::uint64_t generation = 0;
};

struct Surface {
    srv::Client* client = nullptr;
    id_t id = 0;
    /// wl_surface's bound version (preferred_buffer_scale needs 6).
    std::uint32_t version = 1;
    /// wl_surface.set_buffer_scale: buffer pixels per surface unit. Pending
    /// until commit, like everything else the client sets.
    std::int32_t buffer_scale = 1, pending_scale = 1;
    Pending pending;
    Current current;
    std::optional<Toplevel> toplevel;
    /// Frame callbacks owed, by the content generation that satisfies them.
    std::multimap<std::uint64_t, id_t> owed;
    /// It has a wp_linux_drm_syncobj_surface_v1: every buffer it commits
    /// must carry acquire and release points.
    bool explicit_sync = false;
    /// Buffers a head may still be reading, each with the generation it was
    /// last committed at. One goes back to the client (wl_buffer.release, or
    /// its release points under explicit sync) once a frame at or after that
    /// generation has been shown and it is no longer the content.
    struct Held {
        std::uint64_t generation = 0;
        std::vector<SyncPoint> release;
    };
    std::map<id_t, Held> held;
    /// The buffer that is this window's content: the newest commit that is
    /// READY (implicit sync: at once; explicit: once its acquire point has
    /// signalled). Heads are only ever sent this.
    std::optional<id_t> content;
    /// Explicit-sync commits waiting for their acquire point, by commit
    /// number. The newest to become ready is the content; older ones are
    /// superseded and released unshown.
    struct Staged {
        std::optional<id_t> buffer;
        std::optional<SyncPoint> release;
        std::vector<id_t> frames;
        std::uint64_t wait = 0;  // the eventfd's loop token, 0 = none
    };
    std::map<std::uint64_t, Staged> staged;
    std::uint64_t commits = 0;        // commits so far
    std::uint64_t content_commit = 0; // the commit `content` came from
    /// wl_pointer.set_cursor gave it the cursor role, with this hotspot
    /// (moved by wl_surface.offset, as the protocol says).
    bool cursor = false;
    std::int32_t hot_x = 0, hot_y = 0;
    std::int32_t offset_x = 0, offset_y = 0;  // pending wl_surface.offset
};

// -- the host ----------------------------------------------------------------------

class Host {
public:
    using event_type = Event;

    struct Options {
        std::string keyboard_layout = "us";
        /// The session's output, until heads report theirs.
        wm::OutputInfo output{"i4-0", {0, 0, 1920, 1080}};
        /// WAYLAND_DISPLAY for everything the session launches.
        std::string wayland_display = "wayland-i4";
        /// Where launched apps' stdout/stderr go.
        std::string app_log = "/dev/null";
        /// Heads across a network connect here (tcp:HOST:PORT), if given.
        std::string remote;
        /// The pairing key every remote head must have (transport::Key).
        transport::Key remote_key{};
    };

    Host(Options options, transport::Fd link_listener);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    [[nodiscard]] srv::Display& display() { return display_; }

    // -- jaal ------------------------------------------------------------------
    template <class Cx>
    void attach(Cx& cx) {
        emit_ = [&cx](Event e) { (void)cx.emit(e); };
        watch_ = [&cx](int fd, jaal::interest what, std::uint64_t token) { return cx.watch(fd, what, token); };
        start();
    }
    template <class Cx>
    void on_ready(Cx&, const jaal::readiness& r) {
        ready(r);
    }
    template <class K>
    void present(K&) {
        flush();
    }
    void release();

    // -- effects -------------------------------------------------------------------
    void handle(const fx::Configure& e);
    void handle(const fx::Close& e);
    void handle(const fx::KeyboardFocus& e);
    void handle(const fx::PointerAt& e);
    void handle(const fx::Key& e);
    void handle(const fx::Keyboard& e);
    void handle(const fx::Modifiers& e);
    void handle(const fx::Scroll& e);
    void handle(const fx::Button& e);
    void handle(const fx::Send& e);
    void handle(const fx::Share& e);
    void handle(const fx::FramesDone& e);
    void handle(const fx::Release& e);
    void handle(const fx::Formats& e);
    void handle(const fx::Drop& e);
    /// Answers with ev::Launched: the program learns its launch's id.
    Event handle(const fx::Spawn& e);

private:
    /// A protocol object, as (client, object id).
    using Key = std::pair<srv::Client*, id_t>;
    using Registration = jaal::platform::native_reactor::registration;
    struct HeadConn {
        transport::Channel channel;
        std::optional<Registration> reg;
        bool writing = false;
        /// Across a network: frames go as changed tiles (remote/tiles.hpp),
        /// against what this head was last sent of each window.
        std::map<wm::WindowId, remote::Canvas> shadow;
        /// A window's newest frame, waiting while the link is backed up: a
        /// slow network skips frames instead of queueing stale ones.
        std::set<wm::WindowId> behind;
        /// It takes video (up::Caps.video): GPU buffers go through the
        /// encoder, one stream per window.
        bool video = false;
        /// Its decoder is hardware (up::Caps.video_hw). If not, every change
        /// goes as tiles read off the GPU; video only when that fails.
        bool video_hw = true;
        /// Its backing scale (up::Caps.scale): 2 on a Retina screen.
        std::int32_t scale = 1;
        std::map<wm::WindowId, Encoder::StreamPtr> streams;
        /// Frames by datagram (secure remote heads): the session's keys, and
        /// where the head is, once it has said hello over UDP.
        std::optional<transport::DatagramSession> udp;
        std::optional<transport::DatagramSocket::Peer> udp_peer;
        std::uint32_t next_frame = 0;
        std::uint64_t refreshes = 0;
        /// up::Sequenced input: the next number to apply, and inputs that
        /// arrived ahead of a gap (a datagram lost; its link copy is coming).
        std::uint64_t next_input = 1;
        std::map<std::uint64_t, link::up::Sequenced> early;
    };
    /// One numbered input from either path: applied once, in order.
    void sequenced(HeadId id, HeadConn& h, link::up::Sequenced s);
    /// Clients render at the largest scale any head shows: told by
    /// wl_surface.preferred_buffer_scale (and wl_output.scale for old
    /// clients) whenever that changes. 1 with no heads.
    void rescale();
    std::int32_t scale_ = 1;
    /// Encode `window`'s content for remote head `id` and send it.
    void share_remote(HeadId id, HeadConn& h, wm::WindowId window);
    static constexpr std::size_t kBacklog = 256u << 10;  // bytes in flight before frames are skipped

    void start();
    void ready(const jaal::readiness& r);
    void flush();
    void emit(Event e) { if (emit_) emit_(std::move(e)); }

    // protocol (host_protocol.cpp)
    void greet(srv::Client& c);
    void publish_globals();
    void bind_compositor(srv::Client& c, std::uint32_t version, id_t id);
    void bind_shm(srv::Client& c, std::uint32_t version, id_t id);
    void bind_xdg(srv::Client& c, std::uint32_t version, id_t id);
    void bind_seat(srv::Client& c, std::uint32_t version, id_t id);
    void bind_output(srv::Client& c, std::uint32_t version, id_t id);
    void bind_subcompositor(srv::Client& c, std::uint32_t version, id_t id);
    void bind_decoration(srv::Client& c, std::uint32_t version, id_t id);
    void bind_dmabuf(srv::Client& c, std::uint32_t version, id_t id);
    void bind_data_device_manager(srv::Client& c, std::uint32_t version, id_t id);
    void bind_syncobj(srv::Client& c, std::uint32_t version, id_t id);
    void bind_cursor_shape(srv::Client& c, std::uint32_t version, id_t id);
    /// The pointer's look, to the session (which dedups and tells heads).
    void publish_cursor(link::down::Cursor c);
    /// A cursor surface committed: copy its (small) image out and give the
    /// buffer straight back, so a client's cursor never waits on a head.
    void cursor_committed(Surface& s);
    /// Explicit sync: commit `n` of `s` has its acquire point signalled.
    void acquired(Surface& s, std::uint64_t n);
    /// A ready commit `n` replaces everything still staged (released unshown).
    void supersede(Surface& s, std::uint64_t n);
    /// `buffer` is now the window's content (or none: detached).
    void show(Surface& s, std::optional<id_t> buffer, std::optional<SyncPoint> release,
              const std::vector<id_t>& frames);
    /// Loop tokens for acquire waits: [kSyncBase, kSyncBase + kSyncSpan),
    /// below the heads' range.
    static constexpr std::uint64_t kSyncBase = 1u << 8;
    static constexpr std::uint64_t kSyncSpan = (1u << 16) - (1u << 8);
    /// Offer the current selection to `c` (its data devices), or none.
    void offer_selection(srv::Client& c);
    void set_selection(std::optional<Key> source);
    void send_feedback(srv::Client& c, id_t feedback);
    void rebuild_format_table();
    void committed(Surface& s);
    void destroyed(Surface& s);
    void forget(srv::Client& c);
    Surface* surface(srv::Client* c, id_t id);
    Surface* surface(wm::WindowId w);
    void answer_frames(srv::Client& c, const std::vector<id_t>& frames, std::uint32_t time);
    void release_held(Surface& s, std::uint64_t shown);

    // heads (host.cpp)
    void accept_head(const transport::Fd& listener, const transport::Key* key);
    void read_head(HeadId id);
    void drop_head(HeadId id);
    void rewatch(HeadId id);

    // launches (host.cpp)
    void reap(LaunchId id);
    struct Running {
        jaal::platform::native_process process;
        std::optional<Registration> exit;
    };
    std::map<LaunchId, Running> launches_;
    LaunchId next_launch_{1};

    Options options_;
    std::optional<Keymap> keymap_;
    transport::Fd listener_;
    std::optional<Registration> listener_reg_;
    transport::Fd remote_listener_;
    std::optional<Registration> remote_listener_reg_;
    /// Frames to remote heads, by datagram (the same port as --remote, UDP).
    std::optional<transport::DatagramSocket> udp_;
    std::optional<Registration> udp_reg_;
    void read_udp();
    /// A frame to a remote head: by datagram (fragmented, with parity) once
    /// it is reachable on UDP, else on its link. false: the link is dead.
    bool send_frame(HeadConn& h, wm::WindowId window, const std::vector<std::byte>& bytes);
    std::function<void(Event)> emit_;
    std::function<jaal::result<Registration>(int, jaal::interest, std::uint64_t)> watch_;

    std::map<std::uint64_t, Registration> wayland_;
    std::map<HeadId, HeadConn> heads_;
    HeadId next_head_{1};
    std::uint32_t next_window_ = 1;

    // Declared before everything that can hold a Timeline (surfaces' staged
    // and held release points, timelines_): destroyed after them.
    Syncobjs syncobjs_;
    /// Heads across a network: client GPU buffers to video, on the GPU.
    Encoder encoder_;
    std::map<Key, std::unique_ptr<Surface>> surfaces_;
    std::map<Key, AnyBuffer> buffers_;
    std::map<Key, Params> params_;

    // linux-dmabuf: what the heads can sample in place, as the format table
    // the protocol shares by fd (16 bytes per entry: u32 fourcc, u32 pad,
    // u64 modifier), and the render node clients should allocate on.
    std::vector<link::up::Format> formats_;
    wl::OwnedFd format_table_;
    std::uint32_t format_table_size_ = 0;
    std::uint64_t main_device_ = 0;  // dev_t of the render node
    std::optional<std::uint32_t> dmabuf_global_;  // the registry name, while advertised
    std::uint64_t next_buffer_id_ = 1;

    // linux-drm-syncobj, kept by the session itself (syncobj.hpp): client
    // timelines by protocol object; acquire waits, by the eventfd's token.
    // Advertised while clients may render on the GPU (dmabuf is offered).
    std::map<Key, std::shared_ptr<Timeline>> timelines_;
    struct Waiting {
        Surface* surface;
        std::uint64_t commit;  // which of its commits (Surface::commits)
        wl::OwnedFd event;
        Registration reg;
    };
    std::map<std::uint64_t, Waiting> waits_;  // token -> the acquire it waits for
    std::uint64_t next_wait_ = 0;
    std::optional<std::uint32_t> syncobj_global_;
    std::vector<std::pair<srv::Client*, id_t>> feedbacks_;  // to re-send on change
    std::map<wm::WindowId, Key> windows_;
    struct Seat {
        std::vector<std::pair<id_t, std::uint32_t>> keyboards;  // id, version
        std::vector<std::pair<id_t, std::uint32_t>> pointers;  // id, version
        std::vector<id_t> data_devices;
    };
    std::map<srv::Client*, Seat> seats_;
    /// Every client's wl_output objects (id, version): surfaces enter them,
    /// and a scale change is re-announced on them.
    std::map<srv::Client*, std::vector<std::pair<id_t, std::uint32_t>>> outputs_;
    void enter_outputs(Surface& s);

    // The clipboard. A source is a client's offer of data in some MIME
    // types; the selection is at most one source; every other client reads
    // it through its own wl_data_offer, created when it gains keyboard
    // focus (what every compositor does: no snooping by unfocused clients).
    struct DataSource {
        std::vector<std::string> mime_types;
    };
    std::map<Key, DataSource> sources_;
    std::optional<Key> selection_;
    /// Offers handed out for the selection, so they can be told when it
    /// changes (a stale offer's receive is dropped, not forwarded).
    std::map<Key, Key> offers_;  // offer -> the source it reads
    Surface* keyboard_focus_ = nullptr;
    Surface* pointer_focus_ = nullptr;
    /// The surface the pointer-focused client set as its cursor, if any.
    Surface* cursor_surface_ = nullptr;
    wm::Mods mods_;
    /// The typing head's exact xkb state; until a head sends one, masks
    /// are derived from mods_ in the session's own keymap.
    std::optional<link::up::Modifiers> xkb_mods_;
    std::int32_t repeat_rate_ = 25, repeat_delay_ = 600;
    /// Keys down right now, for wl_keyboard.enter's array.
    std::vector<std::uint32_t> pressed_;
    void send_modifiers(srv::Client& c, id_t keyboard);
    void send_keymap(srv::Client& c, id_t keyboard, std::uint32_t version);

    srv::Display display_;  // last: destroyed first; its teardown reads the maps above
};

}  // namespace i4::server
