// src/server/host.cpp — the fds: Wayland, the link listener, each head.
#include "host.hpp"

#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

#include "i4/base/match.hpp"

namespace i4::server {

namespace {

constexpr std::uint64_t kListener = 1;
constexpr std::uint64_t kHeadBase = 1u << 16;     // + head id
constexpr std::uint64_t kWaylandBase = 1u << 24;  // + i4::wl's key

}  // namespace

Host::Host(Options options, transport::Fd link_listener)
    : options_(std::move(options)), listener_(std::move(link_listener)) {
    if (auto k = Keymap::compile(options_.keyboard_layout))
        keymap_.emplace(std::move(*k));
    else
        std::fprintf(stderr, "i4d: %s (keyboards get no keymap)\n", k.error().c_str());
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
    // The output is the first thing the session hears, like any other event.
    emit(ev::OutputReady{options_.output});
}

void Host::release() {
    heads_.clear();
    wayland_.clear();
    listener_reg_.reset();
    emit_ = nullptr;
    watch_ = nullptr;
}

void Host::ready(const jaal::readiness& r) {
    if (r.token >= kWaylandBase) {
        auto what = srv::Ready::readable;
        if (r.hangup) what = what | srv::Ready::hangup;
        if (r.error) what = what | srv::Ready::error;
        display_.ready(r.token - kWaylandBase, what);
    } else if (r.token >= kHeadBase) {
        const HeadId id{static_cast<std::uint32_t>(r.token - kHeadBase)};
        auto it = heads_.find(id);
        if (it == heads_.end()) return;
        if (r.writable && !it->second.channel.flush()) {
            drop_head(id);
            return;
        }
        if (r.readable || r.hangup || r.error) read_head(id);
    } else if (r.token == kListener) {
        accept_head();
    }
}

void Host::flush() {
    display_.flush_clients();
    std::vector<HeadId> ids;
    for (const auto& [id, h] : heads_) ids.push_back(id);
    for (auto id : ids) rewatch(id);
}

// -- heads ---------------------------------------------------------------------

void Host::accept_head() {
    auto ch = transport::Channel::accept(listener_);
    if (!ch) return;
    const auto id = next_head_;
    next_head_ = next_head_.next();
    HeadConn h{std::move(*ch), std::nullopt, false};
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
    if (!s || !s->toplevel || !s->current.buffer) return;
    auto b = buffers_.find({s->client, *s->current.buffer});
    if (b == buffers_.end()) return;
    const auto& buf = b->second;
    using F = wl::proto::wl_shm::format;
    if (buf.format != F::argb8888 && buf.format != F::xrgb8888) return;

    // Zero copy: the client's own pool goes to the head, with where in it
    // this frame is. Nothing here maps it.
    const link::down::Content c{e.window, s->toplevel->generation, Size<space::Buffer>{buf.width, buf.height},
                                buf.offset, buf.stride};
    const auto bytes = link::encode(link::Down{c});
    for (auto id : e.to) {
        auto it = heads_.find(id);
        if (it == heads_.end()) continue;
        const int dup = ::fcntl(buf.pool->fd.get(), F_DUPFD_CLOEXEC, 0);
        if (dup < 0) continue;
        std::vector<transport::Fd> fds;
        fds.emplace_back(dup);
        (void)it->second.channel.send(bytes, std::move(fds));
    }
}

void Host::handle(const fx::Spawn& e) {
    if (options_.spawn) options_.spawn(e.command);
}

}  // namespace i4::server
