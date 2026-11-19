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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
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
            Heard h{std::move(*d), std::nullopt, false};
            if (const auto* c = std::get_if<link::down::Content>(&h.message); c && !p.fds.empty()) {
                const auto t0 = std::chrono::steady_clock::now();
                if (const auto* dl = std::get_if<link::down::DmabufLayout>(&c->layout))
                    h.imported = import(*c, *dl, p.fds);
                else
                    h.pixels = frame(*c, std::get<link::down::ShmLayout>(c->layout), p.fds.front());
                if (trace_) {
                    const auto t1 = std::chrono::steady_clock::now();
                    auto ms = [](auto dt) { return std::chrono::duration<double, std::milli>(dt).count(); };
                    std::fprintf(stderr, "i4-head: frame %dx%d  %s %.3f ms  key->frame %.1f ms\n", c->size.width,
                                 c->size.height, h.imported ? "import" : "convert", ms(t1 - t0),
                                 last_key_ ? ms(t0 - *last_key_) : -1.0);
                    last_key_.reset();
                }
            }
            emit_(std::move(h));
        }
        flush_gpu_done();
    }

    template <class K>
    void present(K& k) {
        window_.present(k);
        // Buffers the GPU has finished with go back to their clients now,
        // not at the next frame (which, on an idle screen, never comes).
        gpu_busy_ = window_.window().poll_gpu_retired();
        flush_gpu_done();
        // mayag adopts its GPU some frames in; the first present that finds
        // it importing tells the program what clients may render into.
        if (!gpu_announced_ && window_.window().can_import_images()) {
            gpu_announced_ = true;
            emit_(GpuReady{importable()});
        }
        const bool want = link_.wants_write();
        if (reg_ && want != writing_ && reg_->modify(want ? jaal::interest::read_write : jaal::interest::read))
            writing_ = want;
    }

    /// While a client buffer is retired but not yet free, ask to be woken
    /// again shortly: a fence check is all it costs.
    [[nodiscard]] auto wait_hint() const noexcept {
        auto hint = window_.wait_hint();
        if (gpu_busy_) {
            constexpr std::chrono::nanoseconds soon{1'000'000};
            hint = hint ? std::min(*hint, soon) : soon;
        }
        return hint;
    }
    [[nodiscard]] bool owes_frame() const noexcept { return window_.owes_frame() || gpu_busy_; }

    void release() {
        reg_.reset();
        window_.release();
    }


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
    void handle(const Say& s) {
        if (trace_ && std::holds_alternative<link::up::Key>(s.message) && !last_key_)
            last_key_ = std::chrono::steady_clock::now();
        (void)link_.send(link::encode(s.message));
    }

    [[nodiscard]] Window& window() { return window_; }

    /// What this head's GPU can sample in place, for up::Caps.
    [[nodiscard]] std::vector<link::up::Format> importable() {
        std::vector<link::up::Format> out;
        for (const auto& f : window_.window().importable_formats())
            out.push_back({static_cast<std::uint32_t>(f.format), static_cast<std::uint64_t>(f.modifier)});
        return out;
    }

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
    /// when the packet does: the head never holds a client's shm.
    static std::optional<jaal::shared<mayag::ImageData>> frame(const link::down::Content& c,
                                                               const link::down::ShmLayout& l,
                                                               const transport::Fd& fd) {
        struct stat st{};
        if (::fstat(fd.get(), &st) != 0 || st.st_size <= 0) return std::nullopt;
        const auto size = static_cast<std::size_t>(st.st_size);
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd.get(), 0);
        if (p == MAP_FAILED) return std::nullopt;
        auto img = convert(std::span<const std::byte>(static_cast<const std::byte*>(p), size),
                           Layout{c.size.width, c.size.height, l.offset, l.stride, l.alpha});
        ::munmap(p, size);
        if (!img) return std::nullopt;
        return jaal::shared<mayag::ImageData>::make(std::move(*img));
    }

    /// A GPU buffer, imported as the window's texture where it is: zero
    /// copies, nothing mapped. Vulkan dups the fd, so the packet's copy
    /// closes as usual. The buffer this replaces is reported done (GpuDone)
    /// only when the GPU has finished sampling it.
    bool import(const link::down::Content& c, const link::down::DmabufLayout& l, const std::vector<transport::Fd>& fds) {
        if (l.planes.empty() || l.planes.size() > 4 || fds.size() < l.planes.size()) return false;
        mayag::Dmabuf d;
        d.n_planes = static_cast<int>(l.planes.size());
        d.format = static_cast<mayag::Fourcc>(l.fourcc);
        d.modifier = static_cast<mayag::Modifier>(l.modifier);
        for (std::size_t i = 0; i < l.planes.size(); ++i)
            d.planes[i] = {fds[i].get(), l.planes[i].offset, l.planes[i].stride};
        // The previous frame's generation is what becomes free when this
        // one replaces it on the GPU.
        auto& last = imported_[c.window];
        const auto prev = last;
        last = c.generation;
        auto r = window_.window().import_image(texture_of(c.window), d, c.size.width, c.size.height,
                                               [this, w = c.window, prev] {
                                                   if (prev != 0) gpu_done_.push_back({w, prev});
                                               });
        if (!r) {
            last = prev;
            if (trace_) std::fprintf(stderr, "i4-head: dmabuf import failed (%d)\n", static_cast<int>(r.error()));
        } else if (trace_ && prev == 0) {
            std::fprintf(stderr, "i4-head: window %u imports %.4s modifier 0x%016llx, %zu plane(s), stride %u\n",
                         c.window.value, reinterpret_cast<const char*>(&l.fourcc),
                         static_cast<unsigned long long>(l.modifier), l.planes.size(), l.planes[0].stride);
        }
        return r.has_value();
    }

    /// Reclaimer callbacks only record; the events go out here, on the loop.
    void flush_gpu_done() {
        for (auto& d : std::exchange(gpu_done_, {})) emit_(std::move(d));
    }

    Window window_;
    transport::Channel link_;
    std::function<std::size_t(const mayag::window_events&)> widen_;
    std::function<void(int)> stop_;
    std::function<jaal::result<jaal::platform::native_reactor::registration>(int, jaal::interest, std::uint64_t)> watch_;
    std::function<void(Events)> emit_;
    Narrow narrow_{this};
    bool trace_ = std::getenv("I4_HEAD_TRACE") != nullptr;
    std::optional<std::chrono::steady_clock::time_point> last_key_;
    std::map<wm::WindowId, std::uint64_t> imported_;  // newest generation imported, per window
    std::vector<GpuDone> gpu_done_;
    bool gpu_busy_ = false;
    bool gpu_announced_ = false;
    std::optional<jaal::platform::native_reactor::registration> reg_;
    bool writing_ = false;
};

}  // namespace i4::head
