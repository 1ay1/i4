// queue.hpp — event queues and the multi-threaded read protocol.
//
// libwayland's threading model, which any replacement has to match:
//
//   * every proxy belongs to an event QUEUE; events dispatch on whichever
//     thread pumps that queue, so a worker thread can own its own objects
//     without touching the main loop.
//   * exactly one thread may read from the socket at a time, but ALL threads
//     must be able to block waiting for events. That is what prepare_read /
//     read_events / cancel_read solve:
//
//       while (wl_display_prepare_read(d) != 0)   // another thread is mid-read
//           wl_display_dispatch_pending(d);
//       wl_display_flush(d);
//       poll(...);
//       wl_display_read_events(d);                // one reader, many waiters
//       wl_display_dispatch_pending(d);
//
//     Without it, the classic race is: thread A checks the queue (empty),
//     thread B reads events and dispatches them, thread A blocks in poll()
//     forever on data that already arrived.
//
// This header implements that, plus the per-queue dispatch that goes with it.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "wire.hpp"

namespace i4::wl {

// ---------------------------------------------------------------------------
/// A decoded, not-yet-typed event: one message as it came off the wire.
// ---------------------------------------------------------------------------
struct RawEvent {
    wire::id_t object = 0;
    std::uint32_t opcode = 0;
    std::span<const wire::word> body;
    std::span<const int> fds;
    std::size_t* fds_consumed = nullptr;  // reserved; routing uses fd_count_for

    void took_fds(std::size_t n) const {
        if (fds_consumed) *fds_consumed = n;
    }
};

// The erased handler an object registers. Generic code builds these; user code
// never writes one by hand.
using RawHandler = std::function<void(const RawEvent&)>;

// ---------------------------------------------------------------------------
// A queued event. The wire bytes are COPIED off the input buffer, because the
// queue outlives the read that produced it and may be dispatched on another
// thread entirely.
// ---------------------------------------------------------------------------
struct QueuedEvent {
    wire::id_t object = 0;
    std::uint32_t opcode = 0;
    std::vector<wire::word> body;
    std::vector<int> fds;

    QueuedEvent() = default;
    QueuedEvent(const RawEvent& ev, std::size_t fd_count)
        : object(ev.object), opcode(ev.opcode),
          body(ev.body.begin(), ev.body.end()) {
        fds.assign(ev.fds.begin(), ev.fds.begin() + static_cast<long>(
                                       std::min(fd_count, ev.fds.size())));
    }
};

// ---------------------------------------------------------------------------
// EventQueue — a thread's inbox.
//
// Proxies are assigned to a queue; the connection's reader sorts incoming
// events into the right one, and whichever thread calls dispatch() on that
// queue runs the handlers.
// ---------------------------------------------------------------------------
class EventQueue {
public:
    explicit EventQueue(std::string name = {}) : name_(std::move(name)) {}

    ~EventQueue() {
        // Anything undispatched still owns its descriptors.
        std::lock_guard lock(mutex_);
        for (auto& ev : pending_)
            for (int fd : ev.fds)
                if (fd >= 0) ::close(fd);
    }

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    const std::string& name() const { return name_; }

    void push(QueuedEvent ev) {
        {
            std::lock_guard lock(mutex_);
            pending_.push_back(std::move(ev));
        }
        cv_.notify_one();
    }

    bool empty() const {
        std::lock_guard lock(mutex_);
        return pending_.empty();
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

    /// Take everything queued. Returning a batch rather than one event keeps
    /// the lock held for O(1) instead of once per handler.
    std::deque<QueuedEvent> take() {
        std::lock_guard lock(mutex_);
        return std::exchange(pending_, {});
    }

    /// Take exactly one event, for callers that want to interleave Wayland
    /// with other work rather than drain a whole burst.
    std::optional<QueuedEvent> take_one() {
        std::lock_guard lock(mutex_);
        if (pending_.empty()) return std::nullopt;
        auto ev = std::move(pending_.front());
        pending_.pop_front();
        return ev;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<QueuedEvent> pending_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// ReadGuard — RAII for the prepare_read / read_events handshake.
//
// Constructed from Connection::prepare_read(). While any guard is alive, the
// connection knows a thread intends to read, so a second thread blocks in
// read_events() rather than racing for the socket.
// ---------------------------------------------------------------------------
class Connection;

class ReadGuard {
public:
    ReadGuard() = default;
    ~ReadGuard();

    ReadGuard(ReadGuard&& o) noexcept
        : conn_(std::exchange(o.conn_, nullptr)), armed_(std::exchange(o.armed_, false)) {}
    ReadGuard& operator=(ReadGuard&& o) noexcept {
        if (this != &o) {
            cancel();
            conn_ = std::exchange(o.conn_, nullptr);
            armed_ = std::exchange(o.armed_, false);
        }
        return *this;
    }
    ReadGuard(const ReadGuard&) = delete;
    ReadGuard& operator=(const ReadGuard&) = delete;

    explicit operator bool() const { return armed_; }

    /// Perform the read. Exactly one waiting thread touches the socket; the
    /// rest return once that read completes. Consumes the guard.
    bool read();

    /// Abandon the intent to read, waking any thread waiting on us.
    void cancel();

private:
    friend class Connection;
    ReadGuard(Connection* c, bool armed) : conn_(c), armed_(armed) {}

    Connection* conn_ = nullptr;
    bool armed_ = false;
};

}  // namespace i4::wl
