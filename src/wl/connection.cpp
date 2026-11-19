// connection.cpp — AF_UNIX transport. The only file in this library that
// performs I/O, and the only one that knows Wayland has a socket at all.
#include <fcntl.h>
#include <poll.h>

#include <cerrno>
#include <cstring>

#include "i4/wl/connection.hpp"

namespace i4::wl {

namespace {

constexpr std::size_t kMaxFdsPerMessage = 28;  // MAX_FDS_OUT in libwayland

bool set_cloexec(int fd) {
    return fcntl(fd, F_SETFD, FD_CLOEXEC) != -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------
Connection::Connection(int fd) : fd_(fd) {
    default_queue_ = std::make_shared<EventQueue>("default");
    queues_.push_back(default_queue_);
}

std::unique_ptr<Connection> Connection::adopt(int fd) {
    return std::unique_ptr<Connection>(new Connection(fd));
}

std::unique_ptr<Connection> Connection::connect(Error& err, const char* name) {
    // $WAYLAND_SOCKET means a parent already connected for us.
    if (const char* sock = std::getenv("WAYLAND_SOCKET")) {
        char* end = nullptr;
        const long fd = std::strtol(sock, &end, 10);
        if (end && *end == '\0' && fd >= 0) {
            unsetenv("WAYLAND_SOCKET");
            set_cloexec(static_cast<int>(fd));
            return adopt(static_cast<int>(fd));
        }
    }

    const char* display = name ? name : std::getenv("WAYLAND_DISPLAY");
    if (!display || !*display) display = "wayland-0";

    std::string path;
    if (display[0] == '/') {
        path = display;  // absolute path: used as-is
    } else {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) {
            err = {Error::Kind::no_display, "XDG_RUNTIME_DIR is not set"};
            return nullptr;
        }
        path = std::string(runtime) + "/" + display;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        err = {Error::Kind::connect_failed, "socket path too long: " + path};
        return nullptr;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        err = {Error::Kind::connect_failed, std::string("socket(): ") + std::strerror(errno)};
        return nullptr;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int e = errno;
        ::close(fd);
        err = {Error::Kind::connect_failed, "connect(" + path + "): " + std::strerror(e)};
        return nullptr;
    }

    err = {};
    return adopt(fd);
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    // Descriptors the compositor sent that no handler claimed. SCM_RIGHTS put
    // them in this process, so they are ours to close or they leak.
    for (std::size_t i = in_fd_head_; i < in_fds_.size(); ++i) {
        if (in_fds_[i] >= 0) ::close(in_fds_[i]);
    }
    in_fds_.clear();
    in_fd_head_ = 0;
}

// ---------------------------------------------------------------------------
// flush
// ---------------------------------------------------------------------------
bool Connection::flush() {
    std::lock_guard send_lock(out_mutex_);
    if (out_.empty() && out_fds_.empty()) return true;
    if (fd_ < 0) return false;

    std::size_t sent_words = 0;
    while (sent_words < out_.size()) {
        iovec iov{};
        iov.iov_base = out_.data() + sent_words;
        iov.iov_len = (out_.size() - sent_words) * sizeof(wire::word);

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        // Attach up to kMaxFdsPerMessage descriptors to this sendmsg.
        alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)]{};
        const std::size_t nfds = std::min(out_fds_.size(), kMaxFdsPerMessage);
        if (nfds) {
            msg.msg_control = cbuf;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
            cmsghdr* cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
            std::memcpy(CMSG_DATA(cm), out_fds_.data(), sizeof(int) * nfds);
        }

        ssize_t n;
        do {
            n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EAGAIN) {
                pollfd p{fd_, POLLOUT, 0};
                if (::poll(&p, 1, -1) > 0) continue;
            }
            fail(Error::Kind::io, std::string("sendmsg(): ") + std::strerror(errno));
            return false;
        }

        // fds are consumed by the first successful sendmsg; close our copies.
        for (std::size_t i = 0; i < nfds; ++i) ::close(out_fds_[i]);
        out_fds_.erase(out_fds_.begin(), out_fds_.begin() + static_cast<long>(nfds));

        sent_words += static_cast<std::size_t>(n) / sizeof(wire::word);
    }

    out_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// read
// ---------------------------------------------------------------------------
bool Connection::read_some() {
    // Compact the input buffer before growing it.
    if (in_head_ > 0) {
        in_.erase(in_.begin(), in_.begin() + static_cast<long>(in_head_));
        in_head_ = 0;
    }
    if (in_fd_head_ > 0) {
        in_fds_.erase(in_fds_.begin(), in_fds_.begin() + static_cast<long>(in_fd_head_));
        in_fd_head_ = 0;
    }

    constexpr std::size_t kChunkWords = 1024;
    const std::size_t old = in_.size();
    in_.resize(old + kChunkWords);

    iovec iov{};
    iov.iov_base = in_.data() + old;
    iov.iov_len = kChunkWords * sizeof(wire::word);

    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t n;
    for (;;) {
        // Driven by a runtime, the read must not wait even on a blocking
        // socket: MSG_DONTWAIT makes "nothing there" an EAGAIN, handled below.
        n = ::recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC | (nonblocking_read_ ? MSG_DONTWAIT : 0));
        if (n >= 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            // Nothing there. Driven by a runtime (ready()), that means the
            // readiness was spurious: return with nothing read, never wait,
            // the runtime will say when there is more.
            if (nonblocking_read_) {
                in_.resize(old);
                return true;
            }
            // A standalone client blocks here rather than spinning: this is
            // the point where its thread waits for the compositor.
            pollfd p{fd_, POLLIN, 0};
            int r;
            do {
                r = ::poll(&p, 1, -1);
            } while (r < 0 && errno == EINTR);
            if (r < 0) {
                in_.resize(old);
                fail(Error::Kind::io, std::string("poll(): ") + std::strerror(errno));
                return false;
            }
            continue;
        }
        in_.resize(old);
        fail(Error::Kind::io, std::string("recvmsg(): ") + std::strerror(errno));
        return false;
    }

    if (n == 0) {
        in_.resize(old);
        fail(Error::Kind::io, "compositor closed the connection");
        return false;
    }

    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cm));
        in_fds_.insert(in_fds_.end(), fds, fds + count);
    }

    in_.resize(old + static_cast<std::size_t>(n) / sizeof(wire::word));
    return true;
}

// ---------------------------------------------------------------------------
// wl_display's own events.
//
// These are hardcoded by OPCODE rather than by generated type, because the
// connection layer sits below the protocol headers and must work for any
// generated `wayland.hpp`. The two opcodes are fixed by the protocol for all
// time (they are object 1's entire event set), so this is stable.
// ---------------------------------------------------------------------------
namespace {
constexpr std::uint32_t kDisplayError = 0;
constexpr std::uint32_t kDisplayDeleteId = 1;
constexpr wire::id_t kDisplayId = 1;
}  // namespace

bool Connection::handle_display_event(const RawEvent& ev) {
    if (ev.object != kDisplayId) return false;

    switch (ev.opcode) {
        case kDisplayError: {
            // error(object_id, code, message)
            wire::Decoder d{ev.body, {}, 0, 0, false};
            const std::uint32_t object = d.get_u32();
            const std::uint32_t code = d.get_u32();
            const std::string_view message = d.get_string();
            if (d.bad) {
                fail(Error::Kind::malformed, "malformed wl_display.error");
            } else {
                {
                    // Keep the structured form too: a client branches on the
                    // code and the object, not on a formatted string.
                    std::lock_guard lock(state_mutex_);
                    protocol_error_.object_id = object;
                    protocol_error_.code = code;
                }
                fail(Error::Kind::protocol,
                     "object " + std::to_string(object) + ": " +
                         std::string(message) + " (code " + std::to_string(code) + ")");
            }
            // A protocol error is fatal by definition: the server has already
            // stopped processing this client.
            return true;
        }
        case kDisplayDeleteId: {
            // delete_id(id): the server confirms the id may be reused.
            //
            // This must NOT drop the handler yet. The server sends delete_id
            // immediately after a one-shot object's final event (wl_callback
            // fires `done` then dies), and the reader routes both in the same
            // pass — so erasing the handler here would delete it before the
            // owning thread ever dispatched the event sitting in its queue.
            // Queue the reclamation instead; it runs once the queue drains.
            wire::Decoder d{ev.body, {}, 0, 0, false};
            const wire::id_t id = d.get_u32();
            if (d.bad) return true;
            {
                std::lock_guard lock(state_mutex_);
                pending_reclaim_.push_back(id);
            }
            return true;
        }
        default:
            return false;  // a future display event: ignore, stay compatible
    }
}

// ---------------------------------------------------------------------------
// drain — split the byte stream into messages and SORT them into queues.
//
// This runs on whichever thread holds the reader role. It does not run
// handlers: it copies each event into its object's queue, so the owning
// thread dispatches it later. That separation is what makes a worker thread
// able to own objects without the main loop ever touching them.
// ---------------------------------------------------------------------------
int Connection::route_incoming() {
    int routed = 0;
    for (;;) {
        const std::size_t avail = in_.size() - in_head_;
        if (avail < 2) break;  // not even a header

        const wire::word object = in_[in_head_];
        const wire::word meta = in_[in_head_ + 1];
        const std::uint32_t opcode = meta & 0xffffu;
        const std::size_t size_bytes = meta >> 16;

        if (size_bytes < 8 || size_bytes % 4 != 0) {
            fail(Error::Kind::malformed, "bad message length from compositor");
            return -1;
        }
        const std::size_t size_words = size_bytes / 4;
        if (avail < size_words) break;  // message still in flight

        const std::size_t fds_available = in_fds_.size() - in_fd_head_;
        const RawEvent ev{
            object, opcode,
            std::span<const wire::word>(in_.data() + in_head_ + 2, size_words - 2),
            std::span<const int>(in_fds_.data() + in_fd_head_, fds_available),
            nullptr,
        };

        // wl_display's bookkeeping is handled inline: it belongs to the
        // connection, not to any application queue, and must take effect
        // before any thread observes the consequences.
        if (!handle_display_event(ev)) {
            const std::size_t nfds = fd_count_for(object, opcode);

            std::shared_ptr<EventQueue> q;
            {
                std::lock_guard lock(state_mutex_);
                auto it = object_queue_.find(object);
                q = (it != object_queue_.end()) ? it->second : default_queue_;
            }
            if (q) {
                q->push(QueuedEvent(ev, nfds));
            } else {
                // No queue: close any descriptors so they do not leak.
                for (std::size_t i = 0; i < nfds && i < fds_available; ++i)
                    if (in_fds_[in_fd_head_ + i] >= 0) ::close(in_fds_[in_fd_head_ + i]);
            }
            // The fds now belong to the queued copy (or were closed above).
            in_fd_head_ += std::min(nfds, fds_available);
        }

        in_head_ += size_words;
        ++routed;

        {
            std::lock_guard lock(state_mutex_);
            if (err_.kind == Error::Kind::protocol) break;  // fatal
        }
    }
    return routed;
}

std::size_t Connection::fd_count_for(wire::id_t object, std::uint32_t opcode) const {
    std::lock_guard lock(state_mutex_);
    auto it = fd_counts_.find(object);
    if (it == fd_counts_.end() || opcode >= it->second.size()) return 0;
    return it->second[opcode];
}

// Kept for the single-threaded path: read, route, then dispatch the default
// queue, which is what dispatch()/dispatch_pending() mean.
int Connection::drain() { return route_incoming(); }

// ---------------------------------------------------------------------------
// Dispatch. Running a handler and reading the socket are separate operations,
// which is the whole point: one thread can be blocked in recvmsg while
// another runs handlers for events already routed to its queue.
// ---------------------------------------------------------------------------
int Connection::dispatch_queue_pending(EventQueue& q) {
    auto batch = q.take();
    int n = 0;
    for (auto& ev : batch) {
        RawHandler h;
        {
            std::lock_guard lock(state_mutex_);
            auto it = handlers_.find(ev.object);
            if (it != handlers_.end()) h = it->second;  // copy: may self-destroy
        }
        if (h) {
            const RawEvent raw{ev.object, ev.opcode, ev.body, ev.fds, nullptr};
            h(raw);
        } else {
            // The object died while this event was in flight. Wayland says
            // ignore it, but its descriptors are still ours.
            for (int fd : ev.fds)
                if (fd >= 0) ::close(fd);
        }
        ++n;
    }

    // Now that handlers have run, ids the server retired can go back in the
    // pool. Doing it here rather than at route time is what lets a one-shot
    // object's final event reach its handler.
    reclaim_retired_ids();
    return n;
}

void Connection::reclaim_retired_ids() {
    std::vector<wire::id_t> ids;
    {
        std::lock_guard lock(state_mutex_);
        if (pending_reclaim_.empty()) return;
        ids.swap(pending_reclaim_);
    }

    std::lock_guard lock(state_mutex_);
    for (wire::id_t id : ids) {
        // Only reclaim an id we actually retired. A delete_id can name an id
        // that has since been handed out again (the server confirms an old
        // object while a new one already holds the number), and releasing it
        // then would give two live objects the same id — which the compositor
        // rejects with "invalid arguments" on the next use, far from here.
        if (zombies_.erase(id) == 0) continue;
        handlers_.erase(id);
        fd_counts_.erase(id);
        object_queue_.erase(id);
        ids_.release(id);
    }
}

int Connection::dispatch_queue(EventQueue& q) {
    if (!ok()) return -1;

    // Anything already queued runs without touching the socket.
    if (!q.empty()) return dispatch_queue_pending(q);

    // Nothing queued: we have to go to the socket. Flush first, because the
    // events we are about to wait for are usually replies to requests still
    // sitting in the output buffer — waiting before flushing is a deadlock.
    if (!flush()) return -1;

    ReadGuard guard = prepare_read(q);
    while (!guard) {
        // Another thread queued events for us while we were arranging to
        // read. Dispatch those instead of blocking.
        const int n = dispatch_queue_pending(q);
        if (n > 0) return n;
        guard = prepare_read(q);
    }

    if (!guard.read()) return -1;
    return dispatch_queue_pending(q);
}

int Connection::dispatch_pending() {
    if (!default_queue_) return -1;
    return dispatch_queue_pending(*default_queue_);
}

// Run at most one handler. `take_one()` pops a single event under the queue's
// lock, so a caller interleaving Wayland with other work never gets stuck
// draining an unbounded burst.
int Connection::dispatch_queue_pending_single(EventQueue& q) {
    auto ev = q.take_one();
    if (!ev) {
        reclaim_retired_ids();
        return 0;
    }

    RawHandler h;
    {
        std::lock_guard lock(state_mutex_);
        auto it = handlers_.find(ev->object);
        if (it != handlers_.end()) h = it->second;
    }
    if (h) {
        const RawEvent raw{ev->object, ev->opcode, ev->body, ev->fds, nullptr};
        h(raw);
    } else {
        for (int fd : ev->fds)
            if (fd >= 0) ::close(fd);
    }

    reclaim_retired_ids();
    return 1;
}

int Connection::dispatch_pending_single() {
    if (!default_queue_) return -1;
    return dispatch_queue_pending_single(*default_queue_);
}

int Connection::dispatch() {
    if (!default_queue_) return -1;
    return dispatch_queue(*default_queue_);
}

// ---------------------------------------------------------------------------
// Bounded waiting.
//
// An event loop that can only block forever is unusable inside another event
// loop, which is why libwayland grew wl_display_dispatch_timeout. The poll
// happens here rather than inside read_some(), so the read path stays a
// straight line.
// ---------------------------------------------------------------------------
int Connection::dispatch_queue_timeout(EventQueue& q,
                                       std::chrono::milliseconds timeout) {
    if (!ok()) return -1;
    if (!q.empty()) return dispatch_queue_pending(q);
    if (!flush()) return -1;

    pollfd p{fd_, POLLIN, 0};
    const int ms = static_cast<int>(timeout.count());
    int r;
    do {
        r = ::poll(&p, 1, ms);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        fail(Error::Kind::io, std::string("poll(): ") + std::strerror(errno));
        return -1;
    }
    if (r == 0) return 0;  // timed out with nothing to show: not an error

    ReadGuard guard = prepare_read(q);
    while (!guard) {
        const int n = dispatch_queue_pending(q);
        if (n > 0) return n;
        guard = prepare_read(q);
    }
    if (!guard.read()) return -1;
    return dispatch_queue_pending(q);
}

int Connection::dispatch_timeout(std::chrono::milliseconds timeout) {
    if (!default_queue_) return -1;
    return dispatch_queue_timeout(*default_queue_, timeout);
}

int Connection::ready() {
    if (!ok() || !default_queue_) return -1;
    EventQueue& q = *default_queue_;
    // Events already queued (read on an earlier call) go first, as the read
    // protocol requires before anyone reads again.
    ReadGuard guard = prepare_read(q);
    while (!guard) {
        if (dispatch_queue_pending(q) < 0) return -1;
        guard = prepare_read(q);
    }
    // The runtime said the socket is readable, so the read takes what is
    // there. If the readiness was spurious it returns with nothing rather
    // than waiting: the runtime owns waiting.
    nonblocking_read_ = true;
    const bool ok_read = guard.read();
    nonblocking_read_ = false;
    if (!ok_read) return -1;
    const int n = dispatch_queue_pending(q);
    // Requests the handlers queued (acks, binds) go out now, not on some
    // later call: nothing else will flush them.
    if (!flush()) return -1;
    return n;
}

// ---------------------------------------------------------------------------
// The read handshake.
//
// Invariant: at most one thread is inside recvmsg at a time, and every thread
// that wanted to read returns once that read completes. Threads that still
// have queued events get an UNARMED guard, telling them to drain those first
// rather than block — that is the race libwayland's prepare_read exists to
// prevent.
// ---------------------------------------------------------------------------
ReadGuard Connection::prepare_read(EventQueue& q) {
    std::unique_lock lock(reader_mutex_);
    if (!q.empty()) return ReadGuard(this, false);  // dispatch those first
    ++readers_waiting_;
    return ReadGuard(this, true);
}

ReadGuard Connection::prepare_read() {
    if (!default_queue_) return ReadGuard(this, false);
    return prepare_read(*default_queue_);
}

bool Connection::do_read() {
    std::unique_lock lock(reader_mutex_);

    if (reader_active_) {
        // Another thread owns the socket. Wait for its read to finish; the
        // events we wanted will have been routed into our queue by then.
        const std::uint64_t epoch = read_epoch_;
        reader_cv_.wait(lock, [&] { return read_epoch_ != epoch || !reader_active_; });
        if (readers_waiting_ > 0) --readers_waiting_;
        return ok();
    }

    reader_active_ = true;
    lock.unlock();

    // The actual socket read happens without reader_mutex_ held, so other
    // threads can queue up behind us instead of serializing on the syscall.
    const bool got = read_some();
    const int routed = got ? route_incoming() : -1;

    lock.lock();
    reader_active_ = false;
    ++read_epoch_;  // tells waiters a read completed, even if it yielded 0
    if (readers_waiting_ > 0) --readers_waiting_;
    lock.unlock();
    reader_cv_.notify_all();

    return got && routed >= 0;
}

void Connection::release_reader(bool did_read) {
    if (did_read) return;  // do_read already accounted for it
    std::lock_guard lock(reader_mutex_);
    if (readers_waiting_ > 0) --readers_waiting_;
    reader_cv_.notify_all();
}

ReadGuard::~ReadGuard() { cancel(); }

bool ReadGuard::read() {
    if (!armed_ || !conn_) return false;
    armed_ = false;
    auto* c = std::exchange(conn_, nullptr);
    return c->do_read();
}

void ReadGuard::cancel() {
    if (!armed_ || !conn_) return;
    armed_ = false;
    std::exchange(conn_, nullptr)->release_reader(false);
}

// ---------------------------------------------------------------------------
// roundtrip — wl_display.sync, then pump until the callback fires.
// Written against the generated types, so it is the first real client of the
// algebra inside the library itself.
// ---------------------------------------------------------------------------
bool Connection::roundtrip() {
    return roundtrip_queue(default_queue_);
}

bool Connection::roundtrip_queue(const std::shared_ptr<EventQueue>& q) {
    if (!ok() || !q) return false;

    wire::id_t cb = 0;
    {
        auto msg = message(1, 0);  // wl_display@1.sync, opcode 0
        cb = msg.allocate_id();
        msg.encoder().put_u32(cb);
    }

    // Route the callback to the caller's queue, so a worker thread can
    // round-trip without the main loop running.
    set_queue(cb, q);

    bool done = false;
    set_handler(cb, [&](const RawEvent&) { done = true; });

    if (!flush()) {
        retire_id(cb);
        return false;
    }
    while (!done) {
        if (dispatch_queue(*q) < 0) {
            retire_id(cb);
            return false;
        }
    }
    // The callback destroys itself after firing, and the server sends
    // delete_id for it. Retire rather than release, so the id comes back
    // through the same path as every other object and can never be handed out
    // twice.
    retire_id(cb);
    return true;
}

}  // namespace i4::wl
