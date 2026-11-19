// display.hpp — the compositor side: sockets, clients, globals.
//
// This is what libwayland-server provides, written natively so a C++
// compositor never needs to link it. The pieces:
//
//   Display  owns the listening socket(s) and the global registry
//   Client   one connection: its own object table and id space
//   Global   an advertised interface, with a typed bind callback
//
// A server's id space is disjoint from a client's (0xff000000 and up), which
// is why Role::id_base exists in the algebra rather than as a constant here.
//
// ── Who waits ───────────────────────────────────────────────────────────
//
// Nobody here. loom is a protocol library, and waiting belongs to whatever
// runs the program (jaal, in tapestry). libwayland-server ships its own
// event loop, and so did loom; embedding it in a real runtime meant a second
// loop nested inside the first, and a class of bugs that came with it: a
// client torn down by an "idle" callback that only ran on the NEXT dispatch,
// which a runtime that dispatches on readiness never gave it.
//
// So the Display only SAYS what needs watching and is TOLD what is ready:
//
//   display.on_watch([&](Display::Watch w)   { runtime.watch(w.fd, w.key); });
//   display.on_unwatch([&](std::uint64_t k)  { runtime.unwatch(k); });
//   ...
//   display.ready(key, what);                // the runtime saw key ready
//
// A listen socket is announced when it is added; a client when it is
// accepted. `ready()` accepts, or reads and dispatches one client, and a
// client that hung up, errored, or was sent a protocol error is destroyed
// before `ready()` returns: its destroy listeners have run and it has been
// unwatched. Nothing is deferred to a later call that may never come.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "i4/wl/server/resource.hpp"

namespace i4::wl::server {

/// What a watched fd is ready for, as the runtime reports it.
enum class Ready : std::uint32_t {
    none = 0,
    readable = 1u << 0,
    hangup = 1u << 2,
    error = 1u << 3,
};
constexpr Ready operator|(Ready a, Ready b) {
    return static_cast<Ready>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr Ready operator&(Ready a, Ready b) {
    return static_cast<Ready>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
constexpr bool any(Ready r) { return static_cast<std::uint32_t>(r) != 0; }

class Display;

// ---------------------------------------------------------------------------
// Client — one connected application.
// ---------------------------------------------------------------------------
class Client {
public:
    Client(Display* display, int fd);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    int fd() const { return fd_; }
    Display* display() const { return display_; }
    bool alive() const { return fd_ >= 0 && !disconnecting_; }

    /// The pid/uid/gid on the other end, for a compositor that gates
    /// privileged protocols on who is asking.
    struct Credentials {
        pid_t pid = 0;
        uid_t uid = 0;
        gid_t gid = 0;
    };
    Credentials credentials() const;

    // -- outgoing -----------------------------------------------------------

    /// Build one event. Holds the send lock until destroyed, exactly like the
    /// client side's ScopedMessage.
    class ScopedEvent {
    public:
        ScopedEvent(Client& c, wire::id_t target, std::uint32_t opcode);
        ~ScopedEvent();
        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;

        wire::Encoder& encoder() { return enc_; }
        wire::id_t allocate_id();

    private:
        Client& client_;
        std::unique_lock<std::mutex> lock_;
        wire::Encoder enc_;
    };

    [[nodiscard]] ScopedEvent event(wire::id_t target, std::uint32_t opcode) {
        return ScopedEvent(*this, target, opcode);
    }

    bool flush();

    /// Server-allocated ids start at 0xff000000, per the protocol.
    wire::id_t allocate_id();

    // -- objects ------------------------------------------------------------

    using RequestHandler = std::function<void(std::uint32_t opcode, wire::Decoder&)>;

    void add_object(wire::id_t id, RequestHandler h,
                    std::span<const std::size_t> fd_counts, std::uint32_t version = 1);
    void remove_object(wire::id_t id);
    bool has_object(wire::id_t id) const;
    /// The version object `id` was bound at; 0 if there's no such object.
    std::uint32_t version_of(wire::id_t id) const;

    /// Create a typed resource for an id the client asked us to make.
    ///
    /// A request the protocol marks as a destructor (destroy, release) ends
    /// the object: after the compositor's handler for it runs, the id is
    /// removed and the client is told it may reuse it (delete_id). That's
    /// what libwayland does, and doing it here means no handler can forget,
    /// which leaves the client unable to ever reuse the id.
    template <Interface I, std::uint32_t V = I::wl_version>
    Resource<I, V> bind_resource(wire::id_t id,
                                 std::shared_ptr<RequestListener<I>> listener,
                                 std::uint32_t version = 0) {
        // The version this client speaks on it: what it bound (a global),
        // else its parent's. 0 means "the most this code was written for".
        if (version == 0 || version > V) version = V;
        add_object(
            id,
            [this, id, listener](std::uint32_t opcode, wire::Decoder& d) {
                listener->dispatch(*this, id, opcode, d);
                if (!d.bad && detail::is_destructor_opcode<I>(opcode) && has_object(id)) {
                    remove_object(id);
                    send_delete_id(id);
                }
            },
            RequestListener<I>::fd_counts(), version);
        return Resource<I, V>{this, id};
    }

    /// Tell the client it broke the protocol, then disconnect it.
    void post_error(wire::id_t object, std::uint32_t code, std::string_view message);

    /// The out-of-memory path. libwayland has this because a compositor that
    /// cannot allocate still has to tell the client something rather than
    /// silently wedging: wl_display.error with code 2 (no_memory) is the
    /// protocol's own way of saying so.
    void post_no_memory();

    /// Run `f` when this client goes away, for whatever reason (hangup,
    /// protocol error, explicit disconnect). A compositor uses this to drop
    /// the surfaces and seats it was holding for them.
    /// (wl_client_add_destroy_listener)
    void on_destroy(std::function<void(Client&)> f) {
        std::lock_guard lock(mutex_);
        destroy_listeners_.push_back(std::move(f));
    }

    /// Attach arbitrary compositor state to a client.
    void set_user_data(void* p) { user_data_ = p; }
    void* user_data() const { return user_data_; }

    /// Confirm that a destroyed id may be reused. The client cannot recycle it
    /// until this arrives, which is why it exists.
    void send_delete_id(wire::id_t id);

    void disconnect();

    /// Read and dispatch whatever this client has sent. Returns the number of
    /// requests handled, or -1 if the client is gone.
    int dispatch();

private:
    friend class Display;

    struct Object {
        RequestHandler handler;
        std::vector<std::size_t> fd_counts;
        /// The version the client bound it at. A global's version is the
        /// one the client asked for in wl_registry.bind; any other object's
        /// is its creator's (the protocol's rule).
        std::uint32_t version = 1;
    };

    bool read_some();
    int drain();
    std::size_t fd_count_for(wire::id_t id, std::uint32_t opcode) const;

    Display* display_ = nullptr;
    int fd_ = -1;
    bool disconnecting_ = false;
    void* user_data_ = nullptr;
    std::vector<std::function<void(Client&)>> destroy_listeners_;

    mutable std::mutex mutex_;
    std::map<wire::id_t, Object> objects_;
    std::vector<wire::id_t> registries_;  // for global_remove broadcasts
    wire::id_t next_id_ = 0xff000000u;

    std::mutex out_mutex_;
    std::vector<wire::word> out_;
    std::vector<int> out_fds_;

    std::vector<wire::word> in_;
    std::size_t in_head_ = 0;
    std::vector<int> in_fds_;
    std::size_t in_fd_head_ = 0;

    std::uint64_t key_ = 0;  // what the runtime knows this client's fd as
};

// ---------------------------------------------------------------------------
// Global — an advertised interface.
//
// The bind callback is typed: it receives a Resource at the version the client
// negotiated, so a compositor implementing wl_compositor cannot accidentally
// hand back a wl_output.
// ---------------------------------------------------------------------------
struct GlobalBase {
    virtual ~GlobalBase() = default;
    virtual std::string_view interface_name() const = 0;
    virtual std::uint32_t max_version() const = 0;
    virtual void bind(Client& client, std::uint32_t version, wire::id_t id) = 0;

    std::uint32_t name = 0;      // the registry handle
    Display* display = nullptr;  // who owns it
    void* user_data = nullptr;   // compositor state, if it wants any

    /// Run when this global is removed, so a compositor can drop whatever it
    /// was backing the global with.
    std::function<void(GlobalBase&)> on_withdrawn;
};

template <Interface I>
class Global : public GlobalBase {
public:
    using BindFn = std::function<void(Client&, std::uint32_t version, wire::id_t id)>;

    Global(std::uint32_t version, BindFn on_bind)
        : version_(version), on_bind_(std::move(on_bind)) {}

    std::string_view interface_name() const override { return I::wl_name; }
    std::uint32_t max_version() const override { return version_; }

    void bind(Client& client, std::uint32_t version, wire::id_t id) override {
        on_bind_(client, version, id);
    }

private:
    std::uint32_t version_;
    BindFn on_bind_;
};

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
class Display {
public:
    Display();
    ~Display();

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // -- The runtime's side (see the header comment) -----------------------

    /// Something the runtime should watch for readability. `key` is what it
    /// hands back to ready(); it is never reused, so a readiness that arrives
    /// for a key already gone names nothing rather than someone new.
    struct Watch {
        int fd = -1;
        std::uint64_t key = 0;
    };

    /// Told about every fd to watch: listen sockets as they are added,
    /// clients as they are accepted. Set it BEFORE add_socket(); anything
    /// already open is announced again when it is set, so the order can't
    /// lose one.
    void on_watch(std::function<void(Watch)> f);

    /// Told when a watched fd is gone (a client destroyed). The runtime must
    /// stop watching it; the fd is closed right after this returns.
    void on_unwatch(std::function<void(std::uint64_t key)> f) { on_unwatch_ = std::move(f); }

    /// The runtime saw `key` ready. Accepts on a listen socket, or reads and
    /// dispatches that one client. A client that hung up, failed, or was
    /// sent a protocol error while dispatching is destroyed before this
    /// returns. A key that is no longer watched is ignored.
    void ready(std::uint64_t key, Ready what);

    // -- Sockets and clients -----------------------------------------------

    /// Listen on $XDG_RUNTIME_DIR/<name>. Returns false if the socket is taken.
    bool add_socket(const std::string& name);

    /// Pick the first free wayland-N. Returns the name, or empty on failure.
    std::string add_socket_auto();

    /// Adopt an already-bound, already-listening socket.
    bool add_socket_fd(int fd);

    /// Advertise an interface. The callback runs when a client binds it.
    template <Interface I>
    std::uint32_t add_global(std::uint32_t version,
                             typename Global<I>::BindFn on_bind) {
        static_assert(version_in_range<I>(), "");
        auto g = std::make_unique<Global<I>>(version, std::move(on_bind));
        return register_global(std::move(g));
    }

    /// Adopt an already-connected socket as a client.
    ///
    /// A compositor that spawns its own helper processes hands them one end of
    /// a socketpair and calls this with the other, so the child never has to
    /// find the display by name. (wl_client_create)
    Client* create_client(int fd);

    void remove_global(std::uint32_t name);

    /// Look up a global by its registry name.
    GlobalBase* find_global(std::uint32_t name) const {
        for (auto& g : globals_)
            if (g->name == name) return g.get();
        return nullptr;
    }

    /// Run `f` for every client that connects. This is where a compositor
    /// installs wl_display and wl_registry, so it is the hook that makes the
    /// server usable without polling the client list.
    /// (wl_display_add_client_created_listener)
    void on_client_created(std::function<void(Client&)> f) {
        client_created_.push_back(std::move(f));
    }

    /// Disconnect everyone. A compositor calls this on shutdown so clients
    /// see a clean hangup instead of a vanished socket.
    /// (wl_display_destroy_clients)
    void destroy_clients();

    /// Serials are monotonic and shared across the display: a client compares
    /// them to order events it received from different objects.
    std::uint32_t next_serial() { return ++serial_; }
    std::uint32_t serial() const { return serial_; }

    const std::vector<std::unique_ptr<Client>>& clients() const { return clients_; }
    const std::vector<std::unique_ptr<GlobalBase>>& globals() const { return globals_; }

    /// Called by a client when it binds wl_registry, to send the initial
    /// global advertisements.
    void send_globals_to(Client& client, wire::id_t registry_id);

    /// Handle wl_registry.bind from a client.
    void handle_bind(Client& client, std::uint32_t name, std::string_view interface,
                     std::uint32_t version, wire::id_t new_id);

    void flush_clients();

    /// Destroy a client now: its destroy listeners run, it is unwatched and
    /// its socket closed. Safe to call from inside that client's own request
    /// handler; the destruction then happens as its dispatch unwinds.
    void destroy_client(Client* c);

private:
    template <Interface I>
    static constexpr bool version_in_range() {
        return true;  // the caller's version is checked against I at bind time
    }

    std::uint32_t register_global(std::unique_ptr<GlobalBase> g);
    void accept_on(int listen_fd);
    std::uint64_t watch(int fd, Client* c);
    void unwatch(std::uint64_t key);
    void reap();

    std::vector<std::unique_ptr<Client>> clients_;
    std::vector<std::unique_ptr<GlobalBase>> globals_;

    struct Listener {
        int fd = -1;
        std::string path;
        std::uint64_t key = 0;
    };
    std::vector<Listener> listeners_;

    // Every watched fd by key: a listener, or a client. Keys start at 1 and
    // are never reused.
    struct Watched {
        int listen_fd = -1;         // a listener, or
        Client* client = nullptr;   // a client
    };
    std::map<std::uint64_t, Watched> watched_;
    std::uint64_t next_key_ = 0;
    std::function<void(Watch)> on_watch_;
    std::function<void(std::uint64_t)> on_unwatch_;

    // True while a client's own handlers are running inside ready(): a
    // destroy requested then waits for the dispatch to unwind, then happens
    // before ready() returns.
    bool dispatching_ = false;

    std::uint32_t next_global_name_ = 1;
    std::uint32_t serial_ = 0;
    std::vector<std::function<void(Client&)>> client_created_;
};

template <Interface I>
template <std::uint32_t V>
Resource<I, V> NewObject<I>::bind(std::shared_ptr<RequestListener<I>> listener) const {
    return client_->bind_resource<I, V>(id_, std::move(listener), client_->version_of(creator_));
}

// ---------------------------------------------------------------------------
// Resource::send — the dual of Proxy::send.
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V>
template <class M, class... Args>
auto Resource<I, V>::send(Args&&... args) const {
    static_assert(std::is_same_v<typename M::iface, I>,
                  "i4::wl: this message belongs to a different interface.");
    static_assert(sends<ServerRole, I, M>,
                  "i4::wl: a server sends EVENTS, not requests. (This message is "
                  "in the interface's request set.)");
    static_assert(M::since <= V,
                  "i4::wl: this event is newer than the version the client bound "
                  "this object at. Guard it with `if constexpr (supports<M>)`.");

    using S = typename M::sig;
    i4::wl::detail::check_args(static_cast<params_t<S>*>(nullptr),
                       static_cast<std::tuple<Args...>*>(nullptr));

    // V is what the compositor built this object for; the client may have
    // bound it older. An event newer than the client's version is a protocol
    // error on the CLIENT's side (it can't decode it), so it's dropped here,
    // where the version the client really bound is known, and never sent.
    // That's the invariant: no send, anywhere, can outrun the client.
    if constexpr (M::since > 1) {
        if (client_ && client_->version_of(id_) < M::since) {
            if constexpr (creates_object_v<S>) return wire::id_t{0};
            else return;
        }
    }

    if constexpr (creates_object_v<S>) {
        if (!client_) return wire::id_t{0};
        auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
        auto ev = client_->event(id_, M::opcode);
        const wire::id_t new_id = ev.allocate_id();
        i4::wl::detail::encode_fold<0>(ev.encoder(), S{}, tup, new_id);
        return new_id;
    } else {
        if (!client_) return;
        auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
        auto ev = client_->event(id_, M::opcode);
        i4::wl::detail::encode_fold<0>(ev.encoder(), S{}, tup, wire::id_t{0});
    }
}

template <Interface I, std::uint32_t V>
template <class E>
void Resource<I, V>::post_error(E code, std::string_view message) const {
    if (!client_) return;
    client_->post_error(id_, static_cast<std::uint32_t>(code), message);
}

}  // namespace i4::wl::server
