// src/server/host.cpp — the fds: Wayland, the link listener, each head.
#include "host.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "i4/base/match.hpp"

extern char** environ;

namespace i4::server {

namespace {

constexpr std::uint64_t kListener = 1;
constexpr std::uint64_t kRemoteListener = 2;
constexpr std::uint64_t kHeadBase = 1u << 16;     // + head id
constexpr std::uint64_t kLaunchBase = 1u << 20;   // + launch id
constexpr std::uint64_t kWaylandBase = 1u << 24;  // + i4::wl's key

}  // namespace

Host::Host(Options options, transport::Fd link_listener)
    : options_(std::move(options)), listener_(std::move(link_listener)) {
    if (auto k = Keymap::compile(options_.keyboard_layout))
        keymap_.emplace(std::move(*k));
    else
        std::fprintf(stderr, "i4d: %s (keyboards get no keymap)\n", k.error().c_str());
    // The GPU clients render on, named in dmabuf feedback from the first
    // client on, so EGL loads the right driver even before a head attaches.
    for (const char* node : {"/dev/dri/renderD128", "/dev/dri/renderD129"}) {
        struct stat st{};
        if (::stat(node, &st) == 0) {
            main_device_ = st.st_rdev;
            break;
        }
    }
    if (!options_.remote.empty()) {
        if (auto l = transport::Channel::listen(options_.remote))
            remote_listener_ = std::move(*l);
        else
            std::fprintf(stderr, "i4d: cannot listen on %s\n", options_.remote.c_str());
    }
    display_.on_client_created([this](srv::Client& c) { greet(c); });
    publish_globals();
}

Host::~Host() { display_.destroy_clients(); }

void Host::start() {
    // Every fd the display opens (listen socket, each client) is one jaal
    // registration; dropping it unwatches. No loop of our own.
    display_.on_watch([this](srv::Display::Watch w) {
        if (auto r = watch_(w.fd, jaal::interest::read, kWaylandBase + w.key)) wayland_.emplace(w.key, std::move(*r));
    });
    display_.on_unwatch([this](std::uint64_t key) { wayland_.erase(key); });
    if (listener_)
        if (auto r = watch_(listener_.get(), jaal::interest::read, kListener)) listener_reg_.emplace(std::move(*r));
    if (remote_listener_)
        if (auto r = watch_(remote_listener_.get(), jaal::interest::read, kRemoteListener))
            remote_listener_reg_.emplace(std::move(*r));
    // The output is the first thing the session hears, like any other event.
    emit(ev::OutputReady{options_.output});
}

void Host::release() {
    heads_.clear();
    wayland_.clear();
    listener_reg_.reset();
    remote_listener_reg_.reset();
    // Launched apps outlive the session's loop (they are the user's), but
    // their exit watches do not.
    for (auto& [id, r] : launches_) r.exit.reset();
    emit_ = nullptr;
    watch_ = nullptr;
}

void Host::ready(const jaal::readiness& r) {
    if (r.token >= kWaylandBase) {
        auto what = srv::Ready::readable;
        if (r.hangup) what = what | srv::Ready::hangup;
        if (r.error) what = what | srv::Ready::error;
        display_.ready(r.token - kWaylandBase, what);
    } else if (r.token >= kLaunchBase) {
        reap(LaunchId{static_cast<std::uint32_t>(r.token - kLaunchBase)});
    } else if (r.token >= kHeadBase) {
        const HeadId id{static_cast<std::uint32_t>(r.token - kHeadBase)};
        auto it = heads_.find(id);
        if (it == heads_.end()) return;
        if (r.writable && !it->second.channel.flush()) {
            drop_head(id);
            return;
        }
        if (r.readable || r.hangup || r.error) read_head(id);
    } else if (r.token >= kSyncBase) {
        // A client's acquire point signalled: that commit is ready to show.
        auto it = waits_.find(r.token);
        if (it == waits_.end()) return;
        auto* s = it->second.surface;
        const auto n = it->second.commit;
        waits_.erase(it);
        if (auto st = s->staged.find(n); st != s->staged.end()) st->second.wait = 0;
        acquired(*s, n);
    } else if (r.token == kListener) {
        accept_head(listener_);
    } else if (r.token == kRemoteListener) {
        accept_head(remote_listener_);
    }
}

void Host::flush() {
    display_.flush_clients();
    std::vector<HeadId> ids;
    for (const auto& [id, h] : heads_) ids.push_back(id);
    for (auto id : ids) {
        // A remote head that skipped frames while the network was backed
        // up gets each window's NEWEST frame once it has drained.
        if (auto it = heads_.find(id); it != heads_.end() && !it->second.behind.empty() &&
                                       it->second.channel.unsent() < kBacklog) {
            const auto behind = std::exchange(it->second.behind, {});
            for (auto w : behind) share_remote(id, it->second, w);
        }
        rewatch(id);
    }
}

// -- heads ---------------------------------------------------------------------

void Host::accept_head(const transport::Fd& listener) {
    auto ch = transport::Channel::accept(listener);
    if (!ch) return;
    const auto id = next_head_;
    next_head_ = next_head_.next();
    if (ch->remote()) std::fprintf(stderr, "i4d: head %u connected over the network\n", id.value);
    HeadConn h{std::move(*ch), std::nullopt, false, {}, {}, false, {}};
    if (auto r = watch_(h.channel.fd(), jaal::interest::read, kHeadBase + id.value)) h.reg.emplace(std::move(*r));
    heads_.emplace(id, std::move(h));
    emit(ev::HeadJoined{id});
}

void Host::read_head(HeadId id) {
    auto it = heads_.find(id);
    if (it == heads_.end()) return;
    auto packets = it->second.channel.receive();
    if (!packets) {
        drop_head(id);
        return;
    }
    for (auto& p : *packets) {
        auto up = link::decode<link::Up>(p.bytes);
        if (!up) {
            std::fprintf(stderr, "i4d: head %u: %s; disconnecting it\n", id.value,
                         std::string(link::describe(up.error())).c_str());
            drop_head(id);
            return;
        }
        if (const auto* caps = std::get_if<link::up::Caps>(&*up)) it->second.video = caps->video && it->second.channel.remote();
        emit(ev::HeadSaid{id, std::move(*up)});
        if (!heads_.contains(id)) return;  // the program dropped it
    }
}

void Host::drop_head(HeadId id) {
    if (heads_.erase(id) != 0) emit(ev::HeadLeft{id});
}

/// Watch for writability exactly while packets are queued.
void Host::rewatch(HeadId id) {
    auto it = heads_.find(id);
    if (it == heads_.end() || !it->second.reg) return;
    const bool want = it->second.channel.wants_write();
    if (want == it->second.writing) return;
    if (it->second.reg->modify(want ? jaal::interest::read_write : jaal::interest::read)) it->second.writing = want;
}

void Host::handle(const fx::Send& e) {
    const auto bytes = link::encode(e.message);
    std::vector<HeadId> dead;
    for (auto id : e.to)
        if (auto it = heads_.find(id); it != heads_.end() && !it->second.channel.send(bytes)) dead.push_back(id);
    for (auto id : dead) drop_head(id);
}

void Host::handle(const fx::Drop& e) {
    if (auto it = heads_.find(e.head); it != heads_.end()) {
        (void)it->second.channel.flush();  // let the refusal out first
        heads_.erase(it);
    }
}

void Host::handle(const fx::Share& e) {
    auto* s = surface(e.window);
    if (!s || !s->toplevel || !s->content) return;
    auto b = buffers_.find({s->client, *s->content});
    if (b == buffers_.end()) return;

    // Zero copy either way: the client's own memory goes to the head (shm:
    // the pool; dmabuf: each plane), with where in it this frame is.
    // Nothing here maps or reads it, and it is finished (explicit sync's
    // acquire point has signalled before a buffer becomes content).
    const auto dup = [](int fd) { return transport::Fd{::fcntl(fd, F_DUPFD_CLOEXEC, 0)}; };
    link::down::Content c{e.window, s->toplevel->generation, {}, link::down::ShmLayout{}};
    std::vector<int> fds;
    const bool ok = match(
        b->second,
        [&](const ShmBuffer& buf) {
            using F = wl::proto::wl_shm::format;
            if (buf.format != F::argb8888 && buf.format != F::xrgb8888) return false;
            c.size = {buf.width, buf.height};
            c.layout = link::down::ShmLayout{buf.offset, buf.stride, buf.format == F::argb8888};
            fds.push_back(buf.pool->fd.get());
            return true;
        },
        [&](const DmabufBuffer& buf) {
            c.size = {buf.width, buf.height};
            link::down::DmabufLayout d{buf.id, buf.fourcc, buf.modifier, {}};
            for (const auto& p : buf.planes) {
                d.planes.push_back({p.offset, p.stride});
                fds.push_back(p.fd->get());
            }
            c.layout = std::move(d);
            return true;
        });
    if (!ok) return;

    const auto bytes = link::encode(link::Down{c});
    for (auto id : e.to) {
        auto it = heads_.find(id);
        if (it == heads_.end()) continue;
        if (it->second.channel.remote()) {
            share_remote(id, it->second, e.window);
            continue;
        }
        std::vector<transport::Fd> owned;
        for (int fd : fds) owned.push_back(dup(fd));
        if (std::ranges::any_of(owned, [](const auto& f) { return !f.valid(); })) continue;
        (void)it->second.channel.send(bytes, std::move(owned));
    }
}

// -- heads across a network -------------------------------------------------------
//
// No fd reaches another machine, so here (and only here) i4d reads a frame:
// the client's finished buffer is mapped, diffed tile by tile against what
// this head already has, and the changed tiles go out compressed. Then the
// buffer is the client's again at once (the copy is ours), and its frame
// callback fires as soon as the bytes are on their way: the client draws at
// the rate the NETWORK takes frames. While the socket is backed up, frames
// are skipped, not queued, so a keystroke's frame never waits behind stale
// ones; the newest goes out when it drains (flush()).

namespace {

/// A mapping of `size` bytes of `fd`, unmapped on scope exit.
struct Mapped {
    void* p = MAP_FAILED;
    std::size_t size = 0;
    Mapped(int fd, std::size_t n) : p(::mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0)), size(n) {}
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
    ~Mapped() {
        if (p != MAP_FAILED) ::munmap(p, size);
    }
    [[nodiscard]] std::span<const std::byte> bytes() const {
        return p == MAP_FAILED ? std::span<const std::byte>{} : std::span{static_cast<const std::byte*>(p), size};
    }
};

std::size_t file_size(int fd) {
    struct stat st{};
    return ::fstat(fd, &st) == 0 && st.st_size > 0 ? static_cast<std::size_t>(st.st_size) : 0;
}

}  // namespace

void Host::share_remote(HeadId id, HeadConn& h, wm::WindowId window) {
    auto* s = surface(window);
    if (!s || !s->toplevel || !s->content) return;
    if (h.channel.unsent() >= kBacklog) {
        h.behind.insert(window);
        return;
    }
    auto b = buffers_.find({s->client, *s->content});
    if (b == buffers_.end()) return;
    const auto gen = s->toplevel->generation;
    const auto t0 = std::chrono::steady_clock::now();
    std::optional<link::down::Encoded> enc;
    std::optional<link::down::Video> video;
    link::down::Content c{window, gen, {}, link::down::ShmLayout{}};
    match(
        b->second,
        [&](const ShmBuffer& buf) {
            using F = wl::proto::wl_shm::format;
            if (buf.format != F::argb8888 && buf.format != F::xrgb8888) return;
            const Mapped m{buf.pool->fd.get(), file_size(buf.pool->fd.get())};
            c.size = {buf.width, buf.height};
            enc = remote::encode(h.shadow[window],
                                 {m.bytes(), buf.width, buf.height, buf.offset, buf.stride, buf.format == F::argb8888});
        },
        [&](const DmabufBuffer& buf) {
            if (h.video && encoder_.available()) {
                // On the GPU, start to finish: no CPU ever reads it.
                DmabufFrame f{buf.id, buf.width, buf.height, buf.fourcc, buf.modifier, {}};
                for (const auto& p : buf.planes) f.planes.push_back({p.fd->get(), p.offset, p.stride});
                auto& stream = h.streams[window];
                if (!stream) stream = encoder_.stream();
                c.size = {buf.width, buf.height};
                video = encoder_.encode(*stream, f);
                return;
            }
            // A LINEAR single-plane 8888 buffer reads straight from the CPU:
            // a remote head asks clients for exactly that, so they keep
            // rendering on the GPU. The read is bracketed by DMA_BUF_IOCTL_SYNC
            // (cache coherence; and it waits out an implicit-sync client's
            // rendering, which is already done for an explicit-sync one).
            constexpr std::uint32_t ar24 = 0x34325241, xr24 = 0x34325258;
            if (buf.modifier != 0 || buf.planes.size() != 1 || (buf.fourcc != ar24 && buf.fourcc != xr24)) return;
            const auto& p = buf.planes.front();
            const int fd = p.fd->get();
            const auto size = static_cast<std::size_t>(::lseek(fd, 0, SEEK_END));
            const Mapped m{fd, size};
            if (m.bytes().empty()) return;
            dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
            c.size = {buf.width, buf.height};
            enc = remote::encode(h.shadow[window], {m.bytes(), buf.width, buf.height, static_cast<std::int32_t>(p.offset),
                                                    static_cast<std::int32_t>(p.stride), buf.fourcc == ar24});
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            (void)::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
        });
    if (video) {
        if (static const bool trace = std::getenv("I4D_TRACE") != nullptr; trace)
            std::fprintf(stderr, "i4d: video encode %dx%d  %s %zu bytes  %.3f ms\n", c.size.width, c.size.height,
                         video->keyframe ? "key" : "delta", video->data.size(),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        c.layout = std::move(*video);
        if (!h.channel.send(link::encode(link::Down{c}))) {
            drop_head(id);
            return;
        }
        emit(ev::HeadSaid{id, link::up::Released{window, gen}});
        emit(ev::HeadSaid{id, link::up::Shown{window, gen}});
        return;
    }
    if (!enc) {
        // Could not be read or encoded: the head keeps its last frame, but
        // the client must never wait on a frame that will not be shown.
        emit(ev::HeadSaid{id, link::up::Released{window, gen}});
        emit(ev::HeadSaid{id, link::up::Shown{window, gen}});
        return;
    }
    if (static const bool trace = std::getenv("I4D_TRACE") != nullptr; trace)
        std::fprintf(stderr, "i4d: remote encode %dx%d  %zu tiles  %zu bytes  %.3f ms\n", c.size.width, c.size.height,
                     enc->tiles.size(), enc->data.size(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    const bool nothing = enc->tiles.empty();
    if (!nothing) {
        c.layout = std::move(*enc);
        if (!h.channel.send(link::encode(link::Down{c}))) {
            drop_head(id);
            return;
        }
    }
    // The pixels are ours now and on their way: the buffer goes back to the
    // client and its frame callback fires, without waiting a round trip.
    emit(ev::HeadSaid{id, link::up::Released{window, gen}});
    emit(ev::HeadSaid{id, link::up::Shown{window, gen}});
}

Event Host::handle(const fx::Spawn& e) {
    // `exec` is a shell command (as in i3), so it runs under sh -c; argv is
    // explicit, never re-parsed. The environment says THIS session is the
    // display, and nothing else is:
    //   * WAYLAND_DISPLAY is ours
    //   * DISPLAY is removed: i4 has no Xwayland yet, and a toolkit that fell
    //     back to X11 would open on whatever X server the parent knew about,
    //     on another screen, silently
    //   * XDG_SESSION_TYPE=wayland, so Chromium/Electron's ozone "auto"
    //     picks Wayland
    namespace pf = jaal::platform;
    pf::process_spec spec;
    spec.env_is_complete = true;
    for (char** env = environ; *env != nullptr; ++env) {
        const std::string_view kv = *env;
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = kv.substr(0, eq);
        if (key == "DISPLAY" || key == "WAYLAND_DISPLAY" || key == "WAYLAND_SOCKET" || key == "XDG_SESSION_TYPE")
            continue;
        spec.env.emplace_back(std::string(key), std::string(kv.substr(eq + 1)));
    }
    spec.env.emplace_back("WAYLAND_DISPLAY", options_.wayland_display);
    spec.env.emplace_back("XDG_SESSION_TYPE", "wayland");
    spec.stdin_from = pf::stream_to::null;
    spec.stdout_to = pf::stream_to::inherit;
    spec.stderr_to = pf::stream_to::inherit;

    const auto id = next_launch_;
    next_launch_ = next_launch_.next();
    // An app's own chatter (Chrome's sandbox notices, a terminal's parser
    // complaints) is the app's, not the session's: it goes to a log, one
    // per session, rather than into i4d's terminal.
    spec.argv = {"sh", "-c", "exec >>\"$I4_APP_LOG\" 2>&1; exec " + e.command};
    spec.env.emplace_back("I4_APP_LOG", options_.app_log);
    auto p = pf::native_process::spawn(spec);
    if (!p) {
        std::fprintf(stderr, "i4d: cannot run: %s\n", e.command.c_str());
        return ev::Launched{id, e.command, false};
    }
    Running r{std::move(*p), std::nullopt};
    // Its exit is one more readiness on the same reactor, reaped there:
    // nothing is left a zombie, and the session hears how it ended.
    if (auto reg = watch_(r.process.exit_handle().get(), jaal::interest::read, kLaunchBase + id.value))
        r.exit.emplace(std::move(*reg));
    launches_.emplace(id, std::move(r));
    return ev::Launched{id, e.command, true};
}

void Host::reap(LaunchId id) {
    auto it = launches_.find(id);
    if (it == launches_.end()) return;
    auto status = it->second.process.reap();
    if (!status) return;  // not ended yet (a spurious wake): keep watching
    launches_.erase(it);
    emit(ev::Exited{id, status->how == jaal::platform::exit_status::kind::signalled, status->code});
}

}  // namespace i4::server
