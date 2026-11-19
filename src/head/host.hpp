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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>

#include "../transport/channel.hpp"
#include "../remote/tiles.hpp"
#include "../remote/video.hpp"
#include "head.hpp"
#include "pixels.hpp"

namespace i4::head {

template <class P, class W>
class Host {
public:
    using event_type = Events;
    using Window = mayag::window_host<P, W>;

    Host(W& window, mayag::AppConfig cfg, transport::Channel link)
        : window_(window, std::move(cfg)), link_(std::move(link)), native_(window) {}
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    ~Host() {
        if (!trace_ || photon_.empty()) return;
        // Key down (kernel timestamp) to the first presented frame showing
        // a client's answer (wp_presentation): the whole path, including
        // the client's own time to draw.
        auto v = photon_;
        std::ranges::sort(v);
        auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
        std::fprintf(stderr, "i4-head: key->photon  n=%zu  p50 %.2f ms  p90 %.2f ms  p99 %.2f ms  min %.2f ms\n",
                     v.size(), at(0.5), at(0.9), at(0.99), v.front());
    }

    // -- the window's half, forwarded; the link's half, added ---------------

    template <class Cx>
    void attach(Cx& cx) {
        widen_ = [&cx](const mayag::window_events& e) -> std::size_t {
            return std::visit([&](const auto& x) { return cx.emit(Events{x}); }, e);
        };
        stop_ = [&cx](int code) { cx.stop(code); };
        watch_ = [&cx](int fd, jaal::interest what, std::uint64_t token) { return cx.watch(fd, what, token); };
        emit_ = [&cx](Events e) { (void)cx.emit(e); };
        tap();
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
            Heard h{std::move(*d), std::nullopt, 0};
            const auto* c = std::get_if<link::down::Content>(&h.message);
            if (c && std::holds_alternative<link::down::Video>(c->layout)) {
                // Across a network, encoded on the session's GPU: decoded here
                // by this platform's hardware decoder.
                const auto t0 = std::chrono::steady_clock::now();
                auto& dec = decoders_[c->window];
                if (!dec) dec = std::make_unique<remote::Decoder>();
                const auto& v = std::get<link::down::Video>(c->layout);
                if (auto pic = dec->decode(v, c->size.width, c->size.height)) {
                    mayag::ImageData img;
                    img.width = pic->width;
                    img.height = pic->height;
                    img.rgba = std::move(pic->rgba);
                    h.pixels = jaal::shared<mayag::ImageData>::make(std::move(img));
                }
                if (trace_)
                    std::fprintf(stderr, "i4-head: video frame %dx%d  %s %zu bytes  %s  %.3f ms\n", c->size.width,
                                 c->size.height, v.keyframe ? "key" : "delta", v.data.size(), dec->name().c_str(),
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            } else if (c && std::holds_alternative<link::down::Encoded>(c->layout)) {
                // Across a network: changed tiles onto this window's canvas,
                // converted to RGBA as they land; the canvas is the image.
                const auto t0 = std::chrono::steady_clock::now();
                auto& canvas = canvases_[c->window];
                const auto& e = std::get<link::down::Encoded>(c->layout);
                if (remote::apply(canvas, c->size.width, c->size.height, e, true)) {
                    mayag::ImageData img;
                    img.width = canvas.width;
                    img.height = canvas.height;
                    img.rgba.assign(reinterpret_cast<const std::uint8_t*>(canvas.pixels.data()),
                                    reinterpret_cast<const std::uint8_t*>(canvas.pixels.data()) + canvas.pixels.size());
                    h.pixels = jaal::shared<mayag::ImageData>::make(std::move(img));
                }
                if (trace_)
                    std::fprintf(stderr, "i4-head: remote frame %dx%d  %zu tiles  %zu bytes  %.3f ms\n", c->size.width,
                                 c->size.height, e.tiles.size(), e.data.size(),
                                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            } else if (c && !p.fds.empty()) {
                const auto t0 = std::chrono::steady_clock::now();
                if (const auto* dl = std::get_if<link::down::DmabufLayout>(&c->layout))
                    h.texture = import(*c, *dl, p.fds);
                else
                    h.pixels = frame(*c, std::get<link::down::ShmLayout>(c->layout), p.fds.front());
                if (trace_) {
                    const auto t1 = std::chrono::steady_clock::now();
                    auto ms = [](auto dt) { return std::chrono::duration<double, std::milli>(dt).count(); };
                    std::fprintf(stderr, "i4-head: frame %dx%d  %s %.3f ms  key->frame %.1f ms\n", c->size.width,
                                 c->size.height, h.texture != 0 ? "import" : "convert", ms(t1 - t0),
                                 last_key_ ? ms(t0 - *last_key_) : -1.0);
                    last_key_.reset();
                }
            }
            // The client answered: the next frame drawn shows it, and its
            // presentation closes the key's input-to-photon measurement.
            if (answer_due_ && std::holds_alternative<link::down::Content>(h.message)) {
                if constexpr (requires { native_.mark_input(0.0); }) native_.mark_input(*answer_due_);
                answer_due_.reset();
            }
            emit_(std::move(h));
        }
        flush_gpu_done();
    }

    template <class K>
    void present(K& k) {
        window_.present(k);
        if constexpr (local_gpu) {
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
        } else if (!gpu_announced_) {
            // A head that cannot import client buffers (macOS, Windows): it
            // is a remote head, and takes video.
            gpu_announced_ = true;
            emit_(GpuReady{});
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
    void handle(const Forget& f) { forget(f.window); }
    void handle(const ShowCursor& c) {
        if constexpr (requires { native_.set_cursor_shape_raw(1u); }) {
            std::visit(
                [&]<class T>(const T& look) {
                    if constexpr (std::is_same_v<T, link::down::CursorShape>)
                        native_.set_cursor_shape_raw(look.shape);
                    else if constexpr (std::is_same_v<T, link::down::CursorImage>)
                        native_.set_cursor_image(reinterpret_cast<const std::uint8_t*>(look.argb.data()), look.width,
                                                 look.height, look.width * 4, look.hot_x, look.hot_y);
                    else
                        native_.hide_cursor();
                },
                c.cursor.look);
        }
    }
    void handle(const Say& s) {
        if (const auto* k = std::get_if<link::up::Key>(&s.message); k && k->pressed) {
            if (trace_ && !last_key_) last_key_ = std::chrono::steady_clock::now();
            // This press went to a client (a binding's never does): time its
            // answer from when the key went down, by the kernel's clock.
            if (key_down_at_ && (!answer_due_ || *key_down_at_ < *answer_due_)) answer_due_ = key_down_at_;
        }
        (void)link_.send(link::encode(s.message));
    }

    [[nodiscard]] Window& window() { return window_; }
    /// For tests: does this platform take the local zero-copy path?
    static constexpr bool gpu_local_for_test() { return local_gpu; }

    /// What this head's GPU can sample in place, for up::Caps.
    [[nodiscard]] std::vector<link::up::Format> importable() {
        std::vector<link::up::Format> out;
        if constexpr (local_gpu)
            for (const auto& f : window_.window().importable_formats())
                out.push_back({static_cast<std::uint32_t>(f.format), static_cast<std::uint64_t>(f.modifier)});
        return out;
    }

private:
    static constexpr std::uint64_t kLink = 1u << 20;
    /// The platform imports client GPU buffers (Linux dmabuf): the local,
    /// zero-copy path. Elsewhere only remote frames (tiles, video) arrive.
    static constexpr bool local_gpu = requires(Window& w, mayag::Dmabuf d) {
        w.window().import_image(1u, d, 1, 1, {});
        w.window().poll_gpu_retired();
        w.window().after_gpu([] {});
    };

    /// The platform's raw seat (Wayland): real key-up, xkb state, the axis
    /// stream. Wired only where the platform has it; elsewhere the program
    /// falls back to mayag's events.
    void tap() {
#if (defined(__linux__) || defined(__FreeBSD__)) && defined(MAYAG_WITH_WAYLAND)
        if constexpr (requires { native_.set_raw_input([](const mayag::platform::RawInput&) {}); }) {
            // The head's own events do not start the photon clock: what
            // counts is when a CLIENT's answer to a key reaches the glass.
            native_.set_photon_manual(true);
            native_.set_photon_sink([this](double ms) {
                photon_.push_back(ms);
                if (trace_) std::fprintf(stderr, "i4-head: key->photon %.2f ms\n", ms);
            });
            native_.set_raw_input([this](const mayag::platform::RawInput& in) {
                namespace r = mayag::platform::raw;
                if (!emit_) return;
                std::visit(
                    [&]<class T>(const T& e) {
                        if constexpr (std::is_same_v<T, r::Key>) {
                            if (e.pressed && trace_) last_key_ = std::chrono::steady_clock::now();
                            // From when the head got the key: event stamps
                            // from synthetic input (hyprctl, wtype) can be
                            // stale, which would inflate every sample.
                            if (e.pressed)
                                key_down_at_ = std::chrono::duration<double>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count();
                            emit_(KeyInput{e.code, e.pressed, mayag::KeyEvent{e.key, e.mods, false}});
                        } else if constexpr (std::is_same_v<T, r::Keymap>) {
                            emit_(KeymapInput{e.text});
                        } else if constexpr (std::is_same_v<T, r::Repeat>) {
                            emit_(RepeatInput{e.rate, e.delay});
                        } else if constexpr (std::is_same_v<T, r::Modifiers>) {
                            emit_(ModsInput{{e.depressed, e.latched, e.locked, e.group}});
                        } else if constexpr (std::is_same_v<T, r::KeyboardFocus>) {
                            if (!e.in) emit_(KeyboardLeft{});
                        } else if constexpr (std::is_same_v<T, r::Axis>) {
                            (e.axis == 0 ? scroll_.dy : scroll_.dx) += e.value;
                            scrolled_ = true;
                        } else if constexpr (std::is_same_v<T, r::AxisValue120>) {
                            (e.axis == 0 ? scroll_.dy120 : scroll_.dx120) += e.value120;
                            scrolled_ = true;
                        } else if constexpr (std::is_same_v<T, r::AxisSource>) {
                            scroll_.source = e.source;
                        } else if constexpr (std::is_same_v<T, r::AxisStop>) {
                            (e.axis == 0 ? scroll_.stop_y : scroll_.stop_x) = true;
                            scrolled_ = true;
                        } else if constexpr (std::is_same_v<T, r::Frame>) {
                            if (scrolled_) emit_(ScrollInput{scroll_});
                            scroll_ = {};
                            scrolled_ = false;
                        }
                    },
                    in);
            });
        }
#endif
    }

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
#if defined(__unix__) || defined(__APPLE__)
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
#else
        (void)c, (void)l, (void)fd;
        return std::nullopt;
#endif
    }

    /// A GPU buffer, shown where it is: zero copies, nothing mapped.
    ///
    /// Each client buffer is imported ONCE (vkCreateImage + memory import)
    /// and kept: clients cycle two or three, so after the first frames a new
    /// frame costs a texture rebind and nothing else. Returns the texture
    /// that shows it, or 0.
    ///
    /// The buffer it replaces on screen is free for the client to write
    /// again once the frames already submitted (which may sample it) have
    /// finished on the GPU: after_gpu -> GpuDone -> up::Released.
    std::uint32_t import(const link::down::Content& c, const link::down::DmabufLayout& l,
                         const std::vector<transport::Fd>& fds) {
        if constexpr (!local_gpu) {
            (void)c, (void)l, (void)fds;
            return 0;
        } else {
        auto& win = windows_[c.window];
        auto cached = std::ranges::find(win.cache, l.buffer, &Imported::buffer);
        if (cached == win.cache.end()) {
            if (l.planes.empty() || l.planes.size() > 4 || fds.size() < l.planes.size()) return 0;
            mayag::Dmabuf d;
            d.n_planes = static_cast<int>(l.planes.size());
            d.format = static_cast<mayag::Fourcc>(l.fourcc);
            d.modifier = static_cast<mayag::Modifier>(l.modifier);
            for (std::size_t i = 0; i < l.planes.size(); ++i)
                d.planes[i] = {fds[i].get(), l.planes[i].offset, l.planes[i].stride};
            const auto tex = next_texture_++;
            if (auto r = window_.window().import_image(tex, d, c.size.width, c.size.height, {}); !r) {
                if (trace_) std::fprintf(stderr, "i4-head: dmabuf import failed (%d)\n", static_cast<int>(r.error()));
                return 0;
            }
            if (trace_)
                std::fprintf(stderr, "i4-head: window %u buffer %llu imported: %.4s modifier 0x%016llx, stride %u\n",
                             c.window.value, static_cast<unsigned long long>(l.buffer),
                             reinterpret_cast<const char*>(&l.fourcc), static_cast<unsigned long long>(l.modifier),
                             l.planes[0].stride);
            // Bounded: a client that reallocates (a resize) leaves its old
            // buffers behind; the least recently shown go.
            constexpr std::size_t keep = 4;
            if (win.cache.size() >= keep) {
                window_.window().release_texture(win.cache.front().texture);
                win.cache.erase(win.cache.begin());
            }
            win.cache.push_back({l.buffer, tex});
            cached = std::prev(win.cache.end());
        } else {
            // Most recently used last.
            std::rotate(cached, std::next(cached), win.cache.end());
            cached = std::prev(win.cache.end());
        }
        // The frame on screen until now: free once the GPU is done with it
        // (the session then gives the buffer back, or signals the client's
        // release point under explicit sync).
        if (win.shown != 0)
            window_.window().after_gpu([this, w = c.window, g = win.shown] { gpu_done_.push_back({w, g}); });
        win.shown = c.generation;
        return cached->texture;
        }
    }

    /// A window is gone: its imported buffers go with it.
    void forget(wm::WindowId w) {
        canvases_.erase(w);
        decoders_.erase(w);
        auto it = windows_.find(w);
        if (it == windows_.end()) return;
        if constexpr (local_gpu)
            for (const auto& i : it->second.cache) window_.window().release_texture(i.texture);
        windows_.erase(it);
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
    W& native_;
    link::up::Scroll scroll_;  // the pointer frame being gathered
    bool scrolled_ = false;
    bool trace_ = std::getenv("I4_HEAD_TRACE") != nullptr;
    std::optional<std::chrono::steady_clock::time_point> last_key_;
    std::optional<double> key_down_at_;  // the newest key press, kernel time (s)
    std::optional<double> answer_due_;   // the oldest press sent to a client and not yet answered
    std::vector<double> photon_;          // key -> photon samples (ms)
    struct Imported {
        std::uint64_t buffer;   // the client's buffer (DmabufLayout::buffer)
        std::uint32_t texture;  // where it is on the GPU
    };
    struct WindowGpu {
        std::vector<Imported> cache;  // least recently shown first
        std::uint64_t shown = 0;      // generation on screen
    };
    std::map<wm::WindowId, WindowGpu> windows_;
    std::map<wm::WindowId, remote::Canvas> canvases_;  // remote frames, as they stand
    std::map<wm::WindowId, std::unique_ptr<remote::Decoder>> decoders_;  // remote video, per window
    std::uint32_t next_texture_ = 0x2000'0000u;
    std::vector<GpuDone> gpu_done_;
    bool gpu_busy_ = false;
    bool gpu_announced_ = false;
    std::optional<jaal::platform::native_reactor::registration> reg_;
    bool writing_ = false;
};

}  // namespace i4::head
