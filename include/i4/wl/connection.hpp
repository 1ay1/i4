// connection.hpp — the Wayland socket, an id allocator, and generic dispatch.
//
// No libwayland. This talks AF_UNIX/SOCK_STREAM to the compositor directly,
// which is the whole point: the protocol is already fully described by types,
// so the transport is ~400 lines and everything above it is generic.
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "proxy.hpp"
#include "queue.hpp"

// ---------------------------------------------------------------------------
// I4_WL_NO_BLOCKING — make "don't block under a runtime" a compile error.
//
// The blocking entry points below (dispatch, roundtrip, dispatch_timeout)
// are correct for a STANDALONE client: a one-shot tool, a test client on its
// own thread. Inside a program that already has a runtime they are a second
// loop smuggled in — while one waits, no timer fires and no signal is
// handled, so Ctrl+C stops working.
//
// A comment saying so is not enforcement. Define I4_WL_NO_BLOCKING and every
// one of them is deprecated, so -Werror turns the mistake into a build
// failure naming the call. i4::wl::runtime::Connect defines it for the
// translation unit that includes loom/client_jaal.hpp, which is exactly the
// code that must never block.
// ---------------------------------------------------------------------------
#if defined(I4_WL_NO_BLOCKING)
#  define I4_WL_BLOCKING                                                      \
      [[deprecated("i4::wl: this call BLOCKS, and this translation unit runs " \
                   "under a runtime. Use i4::wl::runtime::Connect: watch the " \
                   "connection, then call ready() when jaal says it is "     \
                   "readable. For a sync point, send wl_display.sync and "   \
                   "handle its callback as an ordinary event.")]]
#else
#  define I4_WL_BLOCKING
#endif

namespace i4::wl {

// ---------------------------------------------------------------------------
// Errors are values. No exceptions anywhere in the hot path.
// ---------------------------------------------------------------------------
struct Error {
    enum class Kind { none, no_display, connect_failed, io, protocol, malformed } kind = Kind::none;
    std::string what;
    explicit operator bool() const { return kind != Kind::none; }
};

/// The structured form of wl_display.error, kept alongside the message.
///
/// libwayland exposes this through wl_display_get_protocol_error, because
/// "which object failed, with which code" is what a client branches on — a
/// formatted string is for humans.
struct ProtocolError {
    wire::id_t object_id = 0;
    std::uint32_t code = 0;
    std::string_view interface;  // empty when the object is already gone
    explicit operator bool() const { return object_id != 0; }
};

// ---------------------------------------------------------------------------
// Id allocator. Clients own 0x00000001..0xfeffffff; freed ids are recycled,
// which is what a compositor expects.
// ---------------------------------------------------------------------------
class IdAllocator {
public:
    wire::id_t allocate() {
        if (!free_.empty()) {
            auto id = free_.back();
            free_.pop_back();
            in_use_.insert(id);
            return id;
        }
        const wire::id_t id = next_++;
        in_use_.insert(id);
        return id;
    }

    /// Return an id to the pool. Idempotent: releasing an id that is already
    /// free is a no-op rather than a second entry in the free list.
    ///
    /// This matters because an id can be released down two paths — an owning
    /// handle giving it up, and the server's wl_display.delete_id — and a
    /// double entry would hand the SAME id to two live objects, which the
    /// compositor rejects with "invalid arguments" on the second use. That
    /// failure appears thousands of messages later and looks like anything but
    /// an allocator bug, so the invariant is enforced here.
    void release(wire::id_t id) {
        if (!id || id >= next_) return;
        if (in_use_.erase(id) == 0) return;  // not currently allocated
        free_.push_back(id);
    }

    bool is_allocated(wire::id_t id) const { return in_use_.count(id) != 0; }
    std::size_t live_count() const { return in_use_.size(); }

    static constexpr wire::id_t client_max = 0xfeffffffu;

private:
    wire::id_t next_ = 2;  // 1 is always wl_display
    std::vector<wire::id_t> free_;
    std::unordered_set<wire::id_t> in_use_;
};

// ---------------------------------------------------------------------------
// Connection
//
// Thread-safe. The rules, matching libwayland's:
//   * requests may be sent from any thread (serialized by out_mutex_)
//   * each object's events dispatch on whichever thread pumps its queue
//   * exactly one thread reads the socket at a time; see prepare_read()
// ---------------------------------------------------------------------------
class Connection {
public:
    ~Connection() { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /// Connect to $WAYLAND_DISPLAY (or `name`) under $XDG_RUNTIME_DIR.
    static std::unique_ptr<Connection> connect(Error& err, const char* name = nullptr);

    /// Adopt an already-connected socket (e.g. $WAYLAND_SOCKET).
    static std::unique_ptr<Connection> adopt(int fd);

    int fd() const { return fd_; }
    bool ok() const { return fd_ >= 0 && !err_; }
    const Error& error() const { return err_; }

    /// Structured detail for the last protocol error. Equivalent to
    /// wl_display_get_protocol_error: which object, which code.
    const ProtocolError& protocol_error() const { return protocol_error_; }

    /// Cap the unflushed output buffer. When queued requests exceed this, the
    /// connection flushes rather than growing without bound — a client that
    /// floods the compositor should apply backpressure to itself, not consume
    /// all its own memory. 0 means unlimited. (wl_display_set_max_buffer_size)
    void set_max_buffer_size(std::size_t bytes) {
        std::lock_guard lock(out_mutex_);
        max_buffer_bytes_ = bytes;
    }
    std::size_t max_buffer_size() const {
        std::lock_guard lock(out_mutex_);
        return max_buffer_bytes_;
    }
    std::size_t pending_output_bytes() const {
        std::lock_guard lock(out_mutex_);
        return out_.size() * sizeof(wire::word);
    }

    // -- object lifetime ----------------------------------------------------
    wire::id_t allocate_id() {
        std::lock_guard lock(state_mutex_);
        return ids_.allocate();
    }
    void release_id(wire::id_t id) {
        std::lock_guard lock(state_mutex_);
        handlers_.erase(id);
        object_queue_.erase(id);
        ids_.release(id);
    }
    /// Register an object's handler.
    ///
    /// `fd_counts` is the per-opcode number of descriptors that event carries,
    /// read off the interface's signatures at compile time. The reader thread
    /// needs it to know how many fds belong to a message it is routing, which
    /// it must do WITHOUT running the handler.
    void set_handler(wire::id_t id, RawHandler h,
                     std::span<const std::size_t> fd_counts = {}) {
        std::lock_guard lock(state_mutex_);
        handlers_[id] = std::move(h);
        if (!fd_counts.empty())
            fd_counts_[id].assign(fd_counts.begin(), fd_counts.end());
    }

    // -- outgoing -----------------------------------------------------------
    //
    // The send path is serialized by out_mutex_. `ScopedMessage` holds that
    // lock for exactly one message, so concurrent sends from different threads
    // cannot interleave halfway through a body — and, because it is RAII, a
    // caller cannot forget to release it. (An earlier hand-paired
    // begin/end_message did exactly that in roundtrip() and deadlocked the
    // next flush.)
    class ScopedMessage {
    public:
        ScopedMessage(Connection& c, wire::id_t target, std::uint32_t opcode)
            : conn_(c), lock_(c.out_mutex_), enc_{c.out_, c.out_fds_, 0} {
            enc_.begin(target, opcode);
        }
        ~ScopedMessage() { enc_.end(); }

        ScopedMessage(const ScopedMessage&) = delete;
        ScopedMessage& operator=(const ScopedMessage&) = delete;

        /// Allocate the id for this message's new_id argument.
        ///
        /// This happens WHILE the send lock is held, which matters: the id a
        /// message creates and the message itself must reach the server in the
        /// same order. Allocating outside the lock lets two threads swap the
        /// order of "take id 8" and "send the message that defines id 8", and
        /// the compositor rejects the second one with a confusing "invalid
        /// arguments" much later. libwayland holds its display mutex across
        /// exactly this pair for the same reason.
        wire::id_t allocate_id() { return conn_.allocate_id(); }

        wire::Encoder& encoder() { return enc_; }
        operator wire::Encoder&() { return enc_; }  // NOLINT(*-explicit)

    private:
        Connection& conn_;
        std::unique_lock<std::mutex> lock_;
        wire::Encoder enc_;
    };

    /// Build one message. The returned object holds the send lock until it is
    /// destroyed, and patches the message header on the way out.
    [[nodiscard]] ScopedMessage message(wire::id_t target, std::uint32_t opcode) {
        maybe_flush_for_backpressure();
        return ScopedMessage(*this, target, opcode);
    }

    /// Push queued messages to the compositor. fds ride along via SCM_RIGHTS.
    bool flush();

    // -- the blocking calls -------------------------------------------------
    //
    // These WAIT, and waiting is the runtime's job. Under jaal use
    // i4::wl::runtime::Connect (loom/client_jaal.hpp): one registration, the
    // readiness handed back, ready() below doing the dispatch.
    //
    // A blocking call inside a program that already has a runtime is a
    // second loop smuggled in: while it waits, no timer fires, no signal is
    // handled, and the only way out of the program is to kill it. That is
    // not a style preference, it is why Ctrl+C stops working.
    //
    // They stay because a STANDALONE client is a real thing — a one-shot
    // tool, a test client on its own thread, a probe with no loop of its own
    // — and for those this is the simplest correct code. The rule is only
    // that a program with a runtime does not reach for them.

    /// Block until at least one event arrives, then dispatch everything
    /// queued. Returns the number of events dispatched, or -1 on error.
    /// STANDALONE ONLY: under a runtime, see ready().
    I4_WL_BLOCKING int dispatch();

    /// Dispatch whatever has already arrived; never blocks.
    int dispatch_pending();

    /// Run at most ONE queued handler. A compositor-side event loop that also
    /// drives other work uses this to stay responsive rather than draining an
    /// unbounded burst in one go.
    /// (wl_display_dispatch_pending_single / _queue_pending_single)
    int dispatch_pending_single();
    int dispatch_queue_pending_single(EventQueue& q);

    /// Block for at most `timeout`, then dispatch. Returns 0 if nothing came.
    /// STANDALONE ONLY: under a runtime, see ready().
    /// (wl_display_dispatch_timeout)
    I4_WL_BLOCKING int dispatch_timeout(std::chrono::milliseconds timeout);
    I4_WL_BLOCKING int dispatch_queue_timeout(EventQueue& q,
                                             std::chrono::milliseconds timeout);

    /// Flush, then block until the server has handled everything sent so far.
    /// STANDALONE ONLY: under a runtime, send wl_display.sync and let its
    /// callback arrive as a message like any other event.
    I4_WL_BLOCKING bool roundtrip();

    /// The same, but the sync callback lands on `q`, so a worker thread can
    /// round-trip without the main loop running.
    /// (wl_display_roundtrip_queue)
    I4_WL_BLOCKING bool roundtrip_queue(const std::shared_ptr<EventQueue>& q);

    // -- event queues -------------------------------------------------------

    /// Create a queue that a worker thread can own.
    std::shared_ptr<EventQueue> create_queue(std::string name = {}) {
        auto q = std::make_shared<EventQueue>(std::move(name));
        std::lock_guard lock(state_mutex_);
        queues_.push_back(q);
        return q;
    }

    /// Route an object's events to `q` instead of the default queue.
    void set_queue(wire::id_t id, std::shared_ptr<EventQueue> q) {
        std::lock_guard lock(state_mutex_);
        if (q) object_queue_[id] = std::move(q);
        else object_queue_.erase(id);
    }

    /// Attach opaque state to an object, the way wl_proxy_set_user_data does.
    ///
    /// Prefer capturing it in the handler lambda: that keeps the type, and the
    /// compiler checks it. This exists for code being ported from libwayland,
    /// where a void* per proxy is the established idiom.
    void set_user_data(wire::id_t id, void* p) {
        std::lock_guard lock(state_mutex_);
        if (p) user_data_[id] = p;
        else user_data_.erase(id);
    }
    void* user_data(wire::id_t id) const {
        std::lock_guard lock(state_mutex_);
        auto it = user_data_.find(id);
        return it == user_data_.end() ? nullptr : it->second;
    }

    /// A tag identifies which library created an object, so two libraries
    /// sharing one connection can tell their proxies apart. libwayland uses a
    /// `const char *const *` for pointer-identity comparison; same here.
    /// (wl_proxy_set_tag / wl_proxy_get_tag)
    void set_tag(wire::id_t id, const char* const* tag) {
        std::lock_guard lock(state_mutex_);
        if (tag) tags_[id] = tag;
        else tags_.erase(id);
    }
    const char* const* tag(wire::id_t id) const {
        std::lock_guard lock(state_mutex_);
        auto it = tags_.find(id);
        return it == tags_.end() ? nullptr : it->second;
    }

    /// Whether an object currently has a handler installed.
    /// (wl_proxy_get_listener, as a predicate)
    bool has_handler(wire::id_t id) const {
        std::lock_guard lock(state_mutex_);
        return handlers_.count(id) != 0;
    }

    /// The queue an object's events are routed to.
    std::shared_ptr<EventQueue> queue_of(wire::id_t id) const {
        std::lock_guard lock(state_mutex_);
        auto it = object_queue_.find(id);
        return it == object_queue_.end() ? default_queue_ : it->second;
    }

    /// The queue dispatched by dispatch() / dispatch_pending().
    const std::shared_ptr<EventQueue>& default_queue() const { return default_queue_; }

    /// Run the handlers for everything sitting in `q`. Safe to call from the
    /// thread that owns the queue while other threads read the socket.
    int dispatch_queue_pending(EventQueue& q);

    /// Dispatch `q`, blocking until it has something. Uses the read handshake
    /// so several threads may wait on different queues at once.
    I4_WL_BLOCKING int dispatch_queue(EventQueue& q);

    /// The socket is readable (a runtime such as jaal said so): read what is
    /// there and run the handlers for it. Never waits. Returns the number of
    /// events dispatched, or -1 if the connection is gone.
    ///
    /// This is how a client is driven from a runtime that owns waiting, the
    /// counterpart of server::Display::ready(). The blocking calls (dispatch,
    /// roundtrip) are for standalone clients with no runtime around them.
    int ready();

    // -- the multi-threaded read protocol -----------------------------------

    /// Announce an intent to read. Returns an unarmed guard when another
    /// thread is already mid-read AND this thread still has events pending,
    /// which is the caller's signal to dispatch those first:
    ///
    ///     while (!(guard = conn->prepare_read(q)))
    ///         conn->dispatch_queue_pending(q);
    ///     conn->flush();
    ///     poll(...);
    ///     guard.read();
    ///     conn->dispatch_queue_pending(q);
    [[nodiscard]] ReadGuard prepare_read(EventQueue& q);
    [[nodiscard]] ReadGuard prepare_read();

    void close();

    /// Called by an owning handle after it has sent a destructor request.
    ///
    /// The id is NOT recycled here. Wayland requires the client to wait for
    /// wl_display.delete_id: until that arrives, events already in flight may
    /// still name this id, and reusing it early would misroute them. The id
    /// returns to the pool when the server confirms.
    void retire_id(wire::id_t id) {
        std::lock_guard lock(state_mutex_);
        handlers_.erase(id);
        object_queue_.erase(id);
        zombies_.insert(id);
    }

    /// How many ids are awaiting the server's delete_id. A healthy client
    /// keeps this bounded; a growing value means destructors are being sent
    /// for objects the server does not agree exist.
    std::size_t pending_deletions() const {
        std::lock_guard lock(state_mutex_);
        return zombies_.size();
    }

private:
    explicit Connection(int fd);

    bool read_some();
    int drain();
    void fail(Error::Kind k, std::string what) {
        std::lock_guard lock(state_mutex_);
        if (!err_) err_ = Error{k, std::move(what)};
    }

    /// wl_display is object 1 and its two events are the connection's own
    /// bookkeeping, not the application's. Handling them here means every
    /// client gets correct id recycling and a visible protocol error without
    /// having to remember to ask for it.
    bool handle_display_event(const RawEvent& ev);

    /// Read whatever has arrived and sort it into per-object queues. Called
    /// with reader_mutex_ held, so exactly one thread is ever inside it.
    int route_incoming();

    /// How many fds the event for (object, opcode) carries. Known from the
    /// listener's signature; 0 when no handler is registered.
    std::size_t fd_count_for(wire::id_t object, std::uint32_t opcode) const;

    /// Release ids the server confirmed via delete_id. Deferred until after
    /// handlers run, so a one-shot object's final event is not orphaned.
    void reclaim_retired_ids();

    /// Flush early when the unflushed buffer exceeds the configured cap, so a
    /// client that never calls flush() applies backpressure to itself instead
    /// of growing its output buffer without bound.
    void maybe_flush_for_backpressure() {
        std::size_t pending = 0;
        {
            std::lock_guard lock(out_mutex_);
            if (max_buffer_bytes_ == 0) return;
            pending = out_.size() * sizeof(wire::word);
            if (pending < max_buffer_bytes_) return;
        }
        flush();
    }

    friend class ReadGuard;
    bool do_read();
    // Set by ready() for the length of one read: EAGAIN returns instead of
    // waiting. Only the reading thread touches it (the read protocol).
    bool nonblocking_read_ = false;
    void release_reader(bool did_read);

    int fd_ = -1;

    // state_mutex_ guards the object tables, the id allocator and err_.
    mutable std::mutex state_mutex_;
    Error err_;
    ProtocolError protocol_error_;
    IdAllocator ids_;
    std::unordered_map<wire::id_t, RawHandler> handlers_;
    std::unordered_map<wire::id_t, std::vector<std::size_t>> fd_counts_;
    std::unordered_map<wire::id_t, std::shared_ptr<EventQueue>> object_queue_;
    std::unordered_map<wire::id_t, void*> user_data_;
    std::unordered_map<wire::id_t, const char* const*> tags_;
    std::unordered_set<wire::id_t> zombies_;  // destroyed, awaiting delete_id
    std::vector<wire::id_t> pending_reclaim_;  // delete_id'd, awaiting dispatch
    std::vector<std::weak_ptr<EventQueue>> queues_;
    std::shared_ptr<EventQueue> default_queue_;

    // out_mutex_ serializes message construction and flushing.
    mutable std::mutex out_mutex_;
    std::vector<wire::word> out_;
    std::vector<int> out_fds_;
    std::size_t max_buffer_bytes_ = 0;  // 0 = unlimited

    // reader_mutex_ + reader_cv_ implement the one-reader-many-waiters rule.
    std::mutex reader_mutex_;
    std::condition_variable reader_cv_;
    int readers_waiting_ = 0;   // threads inside prepare_read/read_events
    bool reader_active_ = false;  // a thread is in the actual recvmsg
    std::uint64_t read_epoch_ = 0;  // bumped after each completed read

    // in_* are touched only by the thread holding the reader role.
    std::vector<wire::word> in_;
    std::size_t in_head_ = 0;  // words already consumed
    std::vector<int> in_fds_;
    std::size_t in_fd_head_ = 0;

    template <Interface I, std::uint32_t V>
    friend class Proxy;
};

// ---------------------------------------------------------------------------
// send() — the one function that turns a message type into bytes.
//
// It folds the encoder over M::sig. Non-new_id arguments are consumed from the
// caller's pack in order; the new_id argument (if any) is filled with a freshly
// allocated id and handed back as a typed Proxy.
// ---------------------------------------------------------------------------
namespace detail {

// An object argument may be given as a Proxy, a Unique handle, a NonNull, a
// Ref (nullable slots only), or nullptr.
template <class I, std::uint32_t V>
constexpr wire::id_t object_id_of(const Proxy<I, V>& p) { return p.id(); }
template <class I>
constexpr wire::id_t object_id_of(const Ref<I>& r) { return r.id; }
template <class I>
constexpr wire::id_t object_id_of(const NonNull<I>& n) { return n.id(); }
constexpr wire::id_t object_id_of(std::nullptr_t) { return 0; }
template <class T>
constexpr wire::id_t object_id_of(const std::optional<T>& o) {
    return o ? object_id_of(*o) : wire::id_t{0};
}

// True when atom A is carried on the wire as a bare object id.
template <class A>
concept ObjectLike = requires { typename A::iface; } && !is_new_id_v<A>;
template <class A>
concept NullableObjectLike = is_nullable_v<A> && requires { typename A::inner::iface; };

// Which C++ types may be supplied for a NON-NULLABLE object argument of
// interface I: something that proves liveness. Not a bare Ref, and never
// nullptr — the protocol said this argument must be present.
template <class I, class G>
struct nonnull_arg_ok : std::false_type {};
template <class I>
struct nonnull_arg_ok<I, NonNull<I>> : std::true_type {};
template <class I, std::uint32_t V>
struct nonnull_arg_ok<I, Proxy<I, V>> : std::true_type {};

// For a NULLABLE argument, a Ref (including a null one) and nullptr are fine.
template <class I, class G>
struct nullable_arg_ok : std::false_type {};
template <class I>
struct nullable_arg_ok<I, Ref<I>> : std::true_type {};
template <class I>
struct nullable_arg_ok<I, NonNull<I>> : std::true_type {};
template <class I>
struct nullable_arg_ok<I, std::nullptr_t> : std::true_type {};
template <class I, std::uint32_t V>
struct nullable_arg_ok<I, Proxy<I, V>> : std::true_type {};

// Is one caller argument admissible for the parameter the protocol declares?
// `Expected` is the lowered value type from params_t.
template <class Expected, class Given>
constexpr bool arg_ok() {
    using G = std::remove_cvref_t<Given>;
    using E = std::remove_cvref_t<Expected>;
    if constexpr (std::is_same_v<E, G>) {
        return true;
    } else if constexpr (is_nonnull_v<E>) {
        return nonnull_arg_ok<typename E::interface_type, G>::value;
    } else if constexpr (is_ref_v<E>) {
        return nullable_arg_ok<typename E::interface_type, G>::value;
    } else if constexpr (std::is_enum_v<E>) {
        // Protocol enums are nominal too, so an int never stands in for one.
        return std::is_same_v<E, G>;
    } else if constexpr (std::is_same_v<E, BorrowedFd>) {
        // A raw int is not an fd: it carries no ownership discipline at all.
        return std::is_same_v<G, BorrowedFd>;
    } else {
        return std::is_convertible_v<G, E>;
    }
}

// Walk the declared parameter list and the caller's pack together.
template <class... Expected, class... Given>
constexpr void check_args(std::tuple<Expected...>*, std::tuple<Given...>*) {
    static_assert(sizeof...(Expected) == sizeof...(Given),
                  "i4::wl: wrong number of arguments for this request. new_id "
                  "arguments are supplied by the library, not by you.");
    if constexpr (sizeof...(Expected) == sizeof...(Given)) {
        static_assert((arg_ok<Expected, Given>() && ...),
                      "i4::wl: an argument has the wrong type. Object arguments "
                      "must reference the exact interface the protocol declares "
                      "(and must be non-null unless the protocol allows null); "
                      "enums must use the protocol's enum type, not a raw int; "
                      "fds must be a BorrowedFd, not a bare descriptor.");
    }
}

// The fold. `Ci` is the caller-tuple cursor, tracked as a template parameter so
// each argument's source index is a compile-time constant: new_id positions
// consume an allocated id and do NOT advance the cursor, everything else reads
// tuple element Ci and advances. The whole encoder is this one recursion.
template <std::size_t Ci, class Tup>
void encode_fold(wire::Encoder&, Sig<>, const Tup&, wire::id_t) {}

template <std::size_t Ci, class A, class... Rest, class Tup>
void encode_fold(wire::Encoder& e, Sig<A, Rest...>, const Tup& t, wire::id_t new_id) {
    if constexpr (is_dyn_new_id_v<A>) {
        // wl_registry.bind: (string interface, uint version, uint new_id).
        // The caller passes a {name, version} pair; the id is ours.
        const auto& b = std::get<Ci>(t);
        e.put_string(b.interface);
        e.put_u32(b.version);
        e.put_u32(new_id);
        encode_fold<Ci + 1>(e, Sig<Rest...>{}, t, new_id);
    } else if constexpr (is_new_id_v<A>) {
        e.put_u32(new_id);
        encode_fold<Ci>(e, Sig<Rest...>{}, t, new_id);  // cursor unchanged
    } else if constexpr (ObjectLike<A> || NullableObjectLike<A>) {
        e.put_u32(object_id_of(std::get<Ci>(t)));
        encode_fold<Ci + 1>(e, Sig<Rest...>{}, t, new_id);
    } else {
        wire::codec<A>::put(e, std::get<Ci>(t));
        encode_fold<Ci + 1>(e, Sig<Rest...>{}, t, new_id);
    }
}

// The decode fold, mirror image of the above.
//
// Enum arguments are VALIDATED here. The generator emits a `valid()` overload
// per enum from the XML's entry list, found by ADL, so an out-of-domain value
// on the wire is rejected before a handler ever sees it. A compositor that
// sends one is buggy or hostile; either way it is not our handler's problem.
template <class A>
decode_value_t<A> decode_one(wire::Decoder& d) {
    if constexpr (ObjectLike<A>) {
        return Ref<typename A::iface>{d.get_u32()};
    } else if constexpr (NullableObjectLike<A>) {
        return Ref<typename A::inner::iface>{d.get_u32()};
    } else if constexpr (is_dyn_new_id_v<A>) {
        return d.get_u32();
    } else if constexpr (is_new_id_v<A>) {
        return Ref<typename A::iface>{d.get_u32()};
    } else if constexpr (is_enum_atom_v<A>) {
        auto v = wire::codec<A>::get(d);
        if (!valid(v)) d.bad = true;
        return v;
    } else {
        return wire::codec<A>::get(d);
    }
}

template <class... A>
decoded_t<Sig<A...>> decode_fold(wire::Decoder& d, Sig<A...>) {
    // Braced init guarantees left-to-right evaluation, which the wire requires.
    return decoded_t<Sig<A...>>{decode_one<A>(d)...};
}

}  // namespace detail

template <Interface I, std::uint32_t V>
template <class M, class... Args>
decltype(auto) Proxy<I, V>::send(Args&&... args) const {
    detail::check_owner<M, I>();
    detail::check_version<M, V>();
    using S = typename M::sig;
    detail::check_args(static_cast<params_t<S>*>(nullptr),
                       static_cast<std::tuple<Args...>*>(nullptr));

    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);

    wire::id_t new_id = 0;
    {
        // One message, one lock, released by the destructor no matter how we
        // leave this scope. The id is allocated inside, so it cannot be
        // handed out before the message that defines it is queued.
        auto msg = conn_->message(id_, M::opcode);
        if constexpr (creates_object_v<S>) new_id = msg.allocate_id();
        detail::encode_fold<0>(msg.encoder(), S{}, tup, new_id);
    }

    if constexpr (creates_object_v<S>) {
        // A wrapper's children inherit its queue, so a worker thread receives
        // the replies to its own requests.
        if (child_queue_) conn_->set_queue(new_id, child_queue_);
        using R = send_result_t<M, V>;
        R child{conn_, new_id};
        return child;
    }
}

}  // namespace i4::wl
