// display.cpp — the compositor's socket handling, client table and registry.
#include "i4/wl/server/display.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace i4::wl::server {

namespace {
constexpr std::size_t kMaxFdsPerMessage = 28;
constexpr wire::id_t kDisplayId = 1;
constexpr std::uint32_t kDisplayErrorOpcode = 0;
constexpr std::uint32_t kDisplayDeleteIdOpcode = 1;
constexpr wire::id_t kFirstServerId = 0xff000000;
}  // namespace

// ===========================================================================
// Client
// ===========================================================================
Client::Client(Display* display, int fd) : display_(display), fd_(fd) {
    // Nothing to register: the Display announces this fd to whoever runs
    // the program (on_watch) and hands readiness back through ready().
}

Client::~Client() {
    // Tell the compositor before tearing anything down, so its handlers can
    // still read this client's state.
    std::vector<std::function<void(Client&)>> listeners;
    {
        std::lock_guard lock(mutex_);
        listeners.swap(destroy_listeners_);
    }
    for (auto& f : listeners) f(*this);

    if (fd_ >= 0) ::close(fd_);
    for (std::size_t i = in_fd_head_; i < in_fds_.size(); ++i)
        if (in_fds_[i] >= 0) ::close(in_fds_[i]);
    for (int fd : out_fds_)
        if (fd >= 0) ::close(fd);
}

Client::Credentials Client::credentials() const {
    Credentials c;
    ucred uc{};
    socklen_t len = sizeof(uc);
    if (::getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0) {
        c.pid = uc.pid;
        c.uid = uc.uid;
        c.gid = uc.gid;
    }
    return c;
}

wire::id_t Client::allocate_id() {
    std::lock_guard lock(mutex_);
    return next_id_++;
}

// --- ScopedEvent ------------------------------------------------------------
Client::ScopedEvent::ScopedEvent(Client& c, wire::id_t target, std::uint32_t opcode)
    : client_(c), lock_(c.out_mutex_), enc_{c.out_, c.out_fds_, 0} {
    enc_.begin(target, opcode);
}

Client::ScopedEvent::~ScopedEvent() { enc_.end(); }

wire::id_t Client::ScopedEvent::allocate_id() { return client_.allocate_id(); }

// --- objects ----------------------------------------------------------------
void Client::add_object(wire::id_t id, RequestHandler h,
                        std::span<const std::size_t> fd_counts, std::uint32_t version) {
    std::lock_guard lock(mutex_);
    Object o;
    o.handler = std::move(h);
    o.fd_counts.assign(fd_counts.begin(), fd_counts.end());
    o.version = version;
    objects_[id] = std::move(o);
}

std::uint32_t Client::version_of(wire::id_t id) const {
    std::lock_guard lock(mutex_);
    auto it = objects_.find(id);
    return it == objects_.end() ? 0 : it->second.version;
}

void Client::remove_object(wire::id_t id) {
    std::lock_guard lock(mutex_);
    objects_.erase(id);
}

bool Client::has_object(wire::id_t id) const {
    std::lock_guard lock(mutex_);
    return objects_.count(id) != 0;
}

std::size_t Client::fd_count_for(wire::id_t id, std::uint32_t opcode) const {
    std::lock_guard lock(mutex_);
    auto it = objects_.find(id);
    if (it == objects_.end() || opcode >= it->second.fd_counts.size()) return 0;
    return it->second.fd_counts[opcode];
}

// --- protocol errors --------------------------------------------------------
void Client::post_error(wire::id_t object, std::uint32_t code,
                        std::string_view message) {
    {
        auto ev = event(kDisplayId, kDisplayErrorOpcode);
        ev.encoder().put_u32(object);
        ev.encoder().put_u32(code);
        ev.encoder().put_string(message);
    }
    flush();
    // The protocol says the client is finished after an error. Mark it rather
    // than deleting now: we may be inside its own dispatch.
    disconnecting_ = true;
}

void Client::send_delete_id(wire::id_t id) {
    // Only ids the client allocated are the client's to reuse. Ids from
    // 0xff000000 up are ours (a wl_data_offer, say); libwayland just frees
    // them, and clients treat a delete_id for one as a protocol error.
    if (id >= kFirstServerId) return;
    auto ev = event(kDisplayId, kDisplayDeleteIdOpcode);
    ev.encoder().put_u32(id);
}

void Client::post_no_memory() {
    // wl_display.error code 2 is `no_memory` in the core protocol. Saying so
    // beats dropping the connection silently, which leaves the client waiting
    // on a reply that will never come.
    post_error(kDisplayId, 2, "out of memory");
}

void Client::disconnect() { disconnecting_ = true; }

// --- io ---------------------------------------------------------------------
bool Client::flush() {
    std::lock_guard lock(out_mutex_);
    if (out_.empty() && out_fds_.empty()) return true;
    if (fd_ < 0) return false;

    std::size_t sent = 0;
    while (sent < out_.size()) {
        iovec iov{};
        iov.iov_base = out_.data() + sent;
        iov.iov_len = (out_.size() - sent) * sizeof(wire::word);

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

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
            if (errno == EAGAIN) return true;  // try again on the next flush
            return false;
        }
        for (std::size_t i = 0; i < nfds; ++i) ::close(out_fds_[i]);
        out_fds_.erase(out_fds_.begin(), out_fds_.begin() + static_cast<long>(nfds));
        sent += static_cast<std::size_t>(n) / sizeof(wire::word);
    }
    out_.clear();
    return true;
}

bool Client::read_some() {
    if (in_head_ > 0) {
        in_.erase(in_.begin(), in_.begin() + static_cast<long>(in_head_));
        in_head_ = 0;
    }
    if (in_fd_head_ > 0) {
        in_fds_.erase(in_fds_.begin(), in_fds_.begin() + static_cast<long>(in_fd_head_));
        in_fd_head_ = 0;
    }

    constexpr std::size_t kChunk = 1024;
    const std::size_t old = in_.size();
    in_.resize(old + kChunk);

    iovec iov{};
    iov.iov_base = in_.data() + old;
    iov.iov_len = kChunk * sizeof(wire::word);

    alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)]{};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t n;
    do {
        n = ::recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        in_.resize(old);
        return errno == EAGAIN;  // nothing pending is not an error
    }
    if (n == 0) {
        in_.resize(old);
        return false;  // client hung up
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

int Client::drain() {
    int handled = 0;
    for (;;) {
        const std::size_t avail = in_.size() - in_head_;
        if (avail < 2) break;

        const wire::word object = in_[in_head_];
        const wire::word meta = in_[in_head_ + 1];
        const std::uint32_t opcode = meta & 0xffffu;
        const std::size_t size_bytes = meta >> 16;

        if (size_bytes < 8 || size_bytes % 4 != 0) {
            post_error(kDisplayId, 0, "malformed message header");
            return -1;
        }
        const std::size_t words = size_bytes / 4;
        if (avail < words) break;

        const std::size_t nfds = fd_count_for(object, opcode);
        const std::size_t fds_avail = in_fds_.size() - in_fd_head_;

        RequestHandler h;
        {
            std::lock_guard lock(mutex_);
            auto it = objects_.find(object);
            if (it != objects_.end()) h = it->second.handler;
        }

        if (h) {
            wire::Decoder d{
                std::span<const wire::word>(in_.data() + in_head_ + 2, words - 2),
                std::span<const int>(in_fds_.data() + in_fd_head_, fds_avail),
                0, 0, false};
            h(opcode, d);
        } else {
            // A request for an object we do not know is a protocol violation:
            // unlike the client side, the server is the authority on what
            // exists.
            post_error(kDisplayId, 0, "request for an unknown object");
            return -1;
        }

        in_fd_head_ += std::min(nfds, fds_avail);
        in_head_ += words;
        ++handled;

        if (disconnecting_) break;
    }
    return handled;
}

int Client::dispatch() {
    if (fd_ < 0) return -1;
    if (!read_some()) return -1;
    const int n = drain();
    if (n < 0) return -1;
    if (!flush()) return -1;
    return disconnecting_ ? -1 : n;
}

// ===========================================================================
// Display
// ===========================================================================
Display::Display() = default;

Display::~Display() {
    // The runtime is NOT called from here. Whatever watches these fds may
    // already be gone (it can be destroyed before the Display, in any order),
    // and calling into it would be a use after free. It is the runtime's job
    // to drop its registrations when IT goes (Serve::release, run by jaal's
    // teardown before anything is destroyed). So: forget the hooks, then
    // close everything.
    on_watch_ = nullptr;
    on_unwatch_ = nullptr;
    destroy_clients();
    for (auto& l : listeners_) {
        if (l.fd >= 0) ::close(l.fd);
        if (!l.path.empty()) ::unlink(l.path.c_str());
    }
}

// ---------------------------------------------------------------------------
// The runtime's side
// ---------------------------------------------------------------------------

std::uint64_t Display::watch(int fd, Client* c) {
    // Recorded BEFORE the runtime hears of it: a runtime that polls inside
    // on_watch and calls ready() at once must find the key.
    const std::uint64_t key = ++next_key_;
    watched_[key] = Watched{c ? -1 : fd, c};
    if (on_watch_) on_watch_(Watch{fd, key});
    return key;
}

void Display::unwatch(std::uint64_t key) {
    if (watched_.erase(key) && on_unwatch_) on_unwatch_(key);
}

void Display::on_watch(std::function<void(Watch)> f) {
    on_watch_ = std::move(f);
    if (!on_watch_) return;
    // Whatever is already open is announced now, so setting the hook after
    // add_socket() (or after a client connected) can't leave an fd unwatched.
    for (auto& [key, w] : watched_) {
        const int fd = w.client ? w.client->fd() : w.listen_fd;
        on_watch_(Watch{fd, key});
    }
}

void Display::ready(std::uint64_t key, Ready what) {
    auto it = watched_.find(key);
    if (it == watched_.end()) return;  // gone already; its key is never reused
    const Watched w = it->second;

    if (w.listen_fd >= 0) {
        accept_on(w.listen_fd);
        return;
    }

    Client* c = w.client;
    if (any(what & Ready::hangup) || any(what & Ready::error)) {
        // Read what is still buffered first: a client that sends its last
        // requests and closes in one go should have them handled.
        dispatching_ = true;
        (void)c->dispatch();
        dispatching_ = false;
        c->disconnecting_ = true;
    } else {
        dispatching_ = true;
        if (c->dispatch() < 0) c->disconnecting_ = true;
        dispatching_ = false;
    }
    reap();
}

void Display::accept_on(int listen_fd) {
    for (;;) {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) break;  // EAGAIN: no more pending connections
        clients_.push_back(std::make_unique<Client>(this, fd));
        Client& c = *clients_.back();
        c.key_ = watch(fd, &c);
        // Announce it immediately: the compositor installs wl_display here,
        // and a client that sends get_registry before that would be met with
        // "unknown object".
        for (auto& f : client_created_) f(c);
    }
}

/// Destroy every client that is finished: hung up, failed a read, sent a
/// protocol error, or was told to disconnect. Runs at the end of ready(),
/// after any dispatch has unwound, so a handler never has its own client
/// freed under it, and nothing is left for a later call to clean up.
void Display::reap() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        Client& c = **it;
        if (!c.disconnecting_ && c.fd() >= 0) {
            ++it;
            continue;
        }
        unwatch(c.key_);                 // the runtime stops watching first...
        it = clients_.erase(it);         // ...then listeners run, the fd closes
    }
}

void Display::destroy_client(Client* c) {
    if (!c) return;
    c->disconnecting_ = true;
    // Inside a dispatch the client may be on the stack; ready() reaps it as
    // the dispatch unwinds. Outside one, now.
    if (!dispatching_) reap();
}

void Display::destroy_clients() {
    for (auto& c : clients_) c->disconnecting_ = true;
    reap();
}

bool Display::add_socket(const std::string& name) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) return false;

    const std::string path = std::string(runtime) + "/" + name;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;

    // A stale socket from a crashed compositor would block bind(). Only remove
    // it if nothing is actually listening.
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            const bool live =
                probe >= 0 &&
                ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            if (probe >= 0) ::close(probe);
            if (live) {
                ::close(fd);
                return false;  // someone else owns this display name
            }
            ::unlink(path.c_str());
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                ::close(fd);
                return false;
            }
        } else {
            ::close(fd);
            return false;
        }
    }

    if (::listen(fd, 128) < 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }

    Listener l;
    l.fd = fd;
    l.path = path;
    l.key = watch(fd, nullptr);
    listeners_.push_back(std::move(l));
    return true;
}

std::string Display::add_socket_auto() {
    for (int i = 0; i < 32; ++i) {
        const std::string name = "wayland-" + std::to_string(i);
        if (add_socket(name)) return name;
    }
    return {};
}

bool Display::add_socket_fd(int fd) {
    Listener l;
    l.fd = fd;
    l.key = watch(fd, nullptr);
    listeners_.push_back(std::move(l));
    return true;
}

std::uint32_t Display::register_global(std::unique_ptr<GlobalBase> g) {
    const std::uint32_t name = next_global_name_++;
    g->name = name;
    g->display = this;
    globals_.push_back(std::move(g));
    return name;
}

Client* Display::create_client(int fd) {
    // Make it non-blocking: the loop drives it the same way an accepted
    // connection is driven, and a blocking read there would stall everyone.
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    clients_.push_back(std::make_unique<Client>(this, fd));
    Client& c = *clients_.back();
    for (auto& f : client_created_) f(c);
    return &c;
}

void Display::remove_global(std::uint32_t name) {
    for (auto it = globals_.begin(); it != globals_.end(); ++it) {
        if ((*it)->name != name) continue;

        // Let the compositor release whatever backed this global first: its
        // handler may still want to look the global up.
        if ((*it)->on_withdrawn) (*it)->on_withdrawn(**it);

        // Tell every connected client the global is gone, so they can drop
        // anything bound to it. Without this a client keeps advertising a
        // capability the compositor no longer has.
        for (auto& c : clients_) {
            for (wire::id_t reg : c->registries_) {
                auto ev = c->event(reg, 1);  // wl_registry.global_remove
                ev.encoder().put_u32(name);
            }
        }
        globals_.erase(it);
        return;
    }
}

void Display::handle_bind(Client& client, std::uint32_t name,
                          std::string_view interface, std::uint32_t version,
                          wire::id_t new_id) {
    for (auto& g : globals_) {
        if (g->name != name) continue;
        if (g->interface_name() != interface) {
            client.post_error(1, 0, "bind: interface name does not match global");
            return;
        }
        if (version == 0 || version > g->max_version()) {
            client.post_error(1, 0, "bind: unsupported version");
            return;
        }
        g->bind(client, version, new_id);
        return;
    }
    client.post_error(1, 0, "bind: no such global");
}

void Display::send_globals_to(Client& client, wire::id_t registry_id) {
    // Remember which objects are registries, so remove_global() can tell them
    // when a global disappears.
    client.registries_.push_back(registry_id);

    // wl_registry.global(name, interface, version) is opcode 0. Sent by
    // opcode because the server core sits below the generated headers, the
    // same way the client core handles wl_display's two events.
    for (auto& g : globals_) {
        auto ev = client.event(registry_id, 0);
        ev.encoder().put_u32(g->name);
        ev.encoder().put_string(g->interface_name());
        ev.encoder().put_u32(g->max_version());
    }
}

void Display::flush_clients() {
    for (auto& c : clients_) c->flush();
}

}  // namespace i4::wl::server
