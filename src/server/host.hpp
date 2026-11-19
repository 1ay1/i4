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

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <i4/wl/fd.hpp>
#include <i4/wl/proto/wayland.hpp>
#include <i4/wl/proto/xdg_decoration_unstable_v1.hpp>
#include <i4/wl/proto/xdg_shell.hpp>
#include <i4/wl/server/display.hpp>
#include <jaal/jaal.hpp>

#include "../transport/channel.hpp"
#include "keyboard.hpp"
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

// -- surfaces ------------------------------------------------------------------

/// What a client has said since its last commit.
struct Pending {
    /// Outer: attach was called this cycle. Inner: the buffer, or detach.
    std::optional<std::optional<id_t>> attach;
    std::vector<id_t> frames;
};

/// What the surface IS, as of its last commit.
struct Current {
    std::optional<id_t> buffer;
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
    Pending pending;
    Current current;
    std::optional<Toplevel> toplevel;
    /// Frame callbacks owed, by the content generation that satisfies them.
    std::multimap<std::uint64_t, id_t> owed;
};

// -- the host ----------------------------------------------------------------------

class Host {
public:
    using event_type = Event;

    struct Options {
        std::string keyboard_layout = "us";
        /// The session's output, until heads report theirs.
        wm::OutputInfo output{"i4-0", {0, 0, 1920, 1080}};
        std::function<void(const std::string&)> spawn;
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
    void handle(const fx::Button& e);
    void handle(const fx::Send& e);
    void handle(const fx::Share& e);
    void handle(const fx::FramesDone& e);
    void handle(const fx::Drop& e);
    void handle(const fx::Spawn& e);

private:
    using Registration = jaal::platform::native_reactor::registration;
    struct HeadConn {
        transport::Channel channel;
        std::optional<Registration> reg;
        bool writing = false;
    };

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
    void committed(Surface& s);
    void destroyed(Surface& s);
    void forget(srv::Client& c);
    Surface* surface(srv::Client* c, id_t id);
    Surface* surface(wm::WindowId w);
    void answer_frames(srv::Client& c, const std::vector<id_t>& frames, std::uint32_t time);

    // heads (host.cpp)
    void accept_head();
    void read_head(HeadId id);
    void drop_head(HeadId id);
    void rewatch(HeadId id);

    Options options_;
    std::optional<Keymap> keymap_;
    transport::Fd listener_;
    std::optional<Registration> listener_reg_;
    std::function<void(Event)> emit_;
    std::function<jaal::result<Registration>(int, jaal::interest, std::uint64_t)> watch_;

    std::map<std::uint64_t, Registration> wayland_;
    std::map<HeadId, HeadConn> heads_;
    HeadId next_head_{1};
    std::uint32_t next_window_ = 1;

    using Key = std::pair<srv::Client*, id_t>;
    std::map<Key, std::unique_ptr<Surface>> surfaces_;
    std::map<Key, ShmBuffer> buffers_;
    std::map<wm::WindowId, Key> windows_;
    struct Seat {
        std::vector<id_t> keyboards;
        std::vector<std::pair<id_t, std::uint32_t>> pointers;  // id, version
    };
    std::map<srv::Client*, Seat> seats_;
    Surface* keyboard_focus_ = nullptr;
    Surface* pointer_focus_ = nullptr;
    wm::Mods mods_;

    srv::Display display_;  // last: destroyed first; its teardown reads the maps above
};

}  // namespace i4::server
