#pragma once
// src/head/host.hpp — i4-head's shell: mayag's window, plus the link.
//
// mayag's window_host is already a complete jaal host for a window (input,
// frame clock, drawing, image upload). This composes it with one more fd,
// the link to the session, rather than inventing a second loop: jaal sees
// one host, with one reactor, and each readiness goes to whichever part
// registered the token.
//
// Every hook jaal looks for is forwarded, so nothing mayag does is lost:
// attach / on_ready / present / release / wait_hint / owes_frame, the frame
// clock's start/stop_source, and every effect it handles. The link adds
// one effect of its own (do_say).

#include <mayag/mayag.hpp>

#include <optional>
#include <type_traits>
#include <utility>

#include <sys/mman.h>
#include <sys/stat.h>

#include "../transport/channel.hpp"
#include "head.hpp"
#include "pixels.hpp"

namespace i4::head {

template <class P, class W>
class Host {
public:
    using event_type = Events;
    using Window = mayag::window_host<P, W>;

    Host(W& window, mayag::AppConfig cfg, transport::Channel link)
        : window_(window, std::move(cfg)), link_(std::move(link)) {}

    // -- the window's half, forwarded; the link's half, added ---------------

    template <class Cx>
    void attach(Cx& cx) {
        widen_ = [&cx](const mayag::window_events& e) -> std::size_t {
            return std::visit([&](const auto& x) { return cx.emit(Events{x}); }, e);
        };
        stop_ = [&cx](int code) { cx.stop(code); };
        watch_ = [&cx](int fd, jaal::interest what, std::uint64_t token) { return cx.watch(fd, what, token); };
        emit_ = [&cx](Events e) { (void)cx.emit(e); };
        window_.attach(narrow_);
        if (auto r = cx.watch(link_.fd(), jaal::interest::read, kLink)) reg_.emplace(std::move(*r));
    }

    template <class Cx>
    void on_ready(Cx&, const jaal::readiness& r) {
        if (r.token != kLink) {
            window_.on_ready(narrow_, r);
            return;
        }
        if (r.writable) (void)link_.flush();
        if (!(r.readable || r.hangup || r.error)) return;
        auto packets = link_.receive();
        if (!packets) {
            emit_(Lost{});
            reg_.reset();
            return;
        }
        for (auto& p : *packets) {
            auto d = link::decode<link::Down>(p.bytes);
            if (!d) continue;
            Heard h{std::move(*d), std::nullopt};
            if (const auto* c = std::get_if<link::down::Content>(&h.message); c && !p.fds.empty())
                h.pixels = frame(*c, p.fds.front());
            emit_(std::move(h));
        }
    }

    template <class K>
    void present(K& k) {
        window_.present(k);
        const bool want = link_.wants_write();
        if (reg_ && want != writing_ && reg_->modify(want ? jaal::interest::read_write : jaal::interest::read))
            writing_ = want;
    }

    void release() {
        reg_.reset();
        window_.release();
    }

    [[nodiscard]] auto wait_hint() const noexcept { return window_.wait_hint(); }
    [[nodiscard]] bool owes_frame() const noexcept { return window_.owes_frame(); }

    template <class... A>
        requires requires(Window& w, A&&... a) { w.start_source(std::forward<A>(a)...); }
    void start_source(A&&... a) {
        window_.start_source(std::forward<A>(a)...);
    }
    template <class... A>
        requires requires(Window& w, A&&... a) { w.stop_source(std::forward<A>(a)...); }
    void stop_source(A&&... a) {
        window_.stop_source(std::forward<A>(a)...);
    }

    /// Every window effect (title, images, ...), and the link's own.
    template <class E>
        requires requires(Window& w, const E& e) { w.handle(e); }
    decltype(auto) handle(const E& e) {
        return window_.handle(e);
    }
    void handle(const Say& s) { (void)link_.send(link::encode(s.message)); }

    [[nodiscard]] Window& window() { return window_; }

private:
    static constexpr std::uint64_t kLink = 1u << 20;

    /// What the window sees as its context: its own event type, routed into
    /// ours. A member, because the window keeps a reference to its context
    /// (it captures it in lambdas it calls later) for as long as it runs.
    struct Narrow {
        Host* h;
        std::size_t emit(const mayag::window_events& e) { return h->widen_(e); }
        void stop(int code) { h->stop_(code); }
        jaal::result<jaal::platform::native_reactor::registration> watch(int fd, jaal::interest what,
                                                                         std::uint64_t token) {
            return h->watch_(fd, what, token);
        }
    };

    /// Map the client's memory, convert this frame, unmap. The fd closes
    /// when the packet does: the head never holds a client's memory.
    static std::optional<jaal::shared<mayag::ImageData>> frame(const link::down::Content& c, const transport::Fd& fd) {
        struct stat st{};
        if (::fstat(fd.get(), &st) != 0 || st.st_size <= 0) return std::nullopt;
        const auto size = static_cast<std::size_t>(st.st_size);
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED) return std::nullopt;
        auto img = convert(std::span<const std::byte>(static_cast<const std::byte*>(p), size),
                           Layout{c.size.width, c.size.height, c.offset, c.stride, c.alpha});
        ::munmap(p, size);
        if (!img) return std::nullopt;
        return jaal::shared<mayag::ImageData>::make(std::move(*img));
    }

    Window window_;
    transport::Channel link_;
    std::function<std::size_t(const mayag::window_events&)> widen_;
    std::function<void(int)> stop_;
    std::function<jaal::result<jaal::platform::native_reactor::registration>(int, jaal::interest, std::uint64_t)> watch_;
    std::function<void(Events)> emit_;
    Narrow narrow_{this};
    std::optional<jaal::platform::native_reactor::registration> reg_;
    bool writing_ = false;
};

}  // namespace i4::head
