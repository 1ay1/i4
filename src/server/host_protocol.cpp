// src/server/host_protocol.cpp — the Wayland objects, on i4::wl.
//
// Handlers only record what clients say, or emit an Event. The effect
// handlers at the bottom are the reverse: the Session decided, this says it
// to a client.
#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <i4/wl/proto/linux_dmabuf_v1.hpp>
#include <i4/wl/proto/cursor_shape_v1.hpp>

#include "host.hpp"
#include "i4/base/match.hpp"

namespace i4::server {

using namespace i4::wl;
using namespace i4::wl::proto;
namespace wire = i4::wl::wire;

namespace {

// wl_display and wl_registry belong to the core: i4::wl installs none.
constexpr std::uint32_t display_sync = 0;
constexpr std::uint32_t display_get_registry = 1;
constexpr std::uint32_t registry_bind = 0;

constexpr std::uint32_t compositor_version = 6;
constexpr std::uint32_t xdg_version = 6;
constexpr std::uint32_t seat_version = 9;

std::uint32_t now_ms() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

template <class I>
NonNull<I> nonnull(id_t id) {
    return NonNull<I>::unchecked(Ref<I>{id});
}

}  // namespace

std::vector<id_t> commit(Current& current, Pending& pending) {
    if (pending.attach) current.buffer = *pending.attach;
    // Sync points belong to the buffer committed with them, and only to it.
    current.acquire = pending.attach ? pending.acquire : std::nullopt;
    current.release = pending.attach ? pending.release : std::nullopt;
    auto frames = std::move(pending.frames);
    pending = Pending{};
    return frames;
}

Surface* Host::surface(srv::Client* c, id_t id) {
    auto it = surfaces_.find({c, id});
    return it == surfaces_.end() ? nullptr : it->second.get();
}

Surface* Host::surface(wm::WindowId w) {
    auto it = windows_.find(w);
    return it == windows_.end() ? nullptr : surface(it->second.first, it->second.second);
}

void Host::answer_frames(srv::Client& c, const std::vector<id_t>& frames, std::uint32_t time) {
    for (auto cb : frames) {
        {
            auto ev = c.event(cb, 0);  // wl_callback.done
            ev.encoder().put_u32(time);
        }
        c.send_delete_id(cb);
    }
}

// -- every client starts here --------------------------------------------------------

void Host::greet(srv::Client& client) {
    client.on_destroy([this](srv::Client& c) { forget(c); });
    client.add_object(
        1,
        [this, &client](std::uint32_t opcode, wire::Decoder& d) {
            if (opcode == display_sync) {
                const id_t cb = d.get_u32();
                if (!d.bad) answer_frames(client, {cb}, display_.next_serial());
            } else if (opcode == display_get_registry) {
                const id_t reg = d.get_u32();
                if (d.bad) return;
                client.add_object(
                    reg,
                    [this, &client](std::uint32_t op, wire::Decoder& rd) {
                        if (op != registry_bind) return;
                        const std::uint32_t name = rd.get_u32();
                        const std::string_view iface = rd.get_string();
                        const std::uint32_t ver = rd.get_u32();
                        const id_t nid = rd.get_u32();
                        if (!rd.bad) display_.handle_bind(client, name, iface, ver, nid);
                    },
                    {});
                display_.send_globals_to(client, reg);
            }
        },
        {});
}

void Host::publish_globals() {
    auto global = [this]<class I>(std::type_identity<I>, std::uint32_t v, void (Host::*bind)(srv::Client&, std::uint32_t, id_t)) {
        display_.add_global<I>(v, [this, bind](srv::Client& c, std::uint32_t ver, id_t id) { (this->*bind)(c, ver, id); });
    };
    global(std::type_identity<wl_compositor>{}, compositor_version, &Host::bind_compositor);
    global(std::type_identity<wl_shm>{}, 2, &Host::bind_shm);
    global(std::type_identity<xdg_wm_base>{}, xdg_version, &Host::bind_xdg);
    global(std::type_identity<wl_seat>{}, seat_version, &Host::bind_seat);
    global(std::type_identity<wl_output>{}, 4, &Host::bind_output);
    global(std::type_identity<wl_subcompositor>{}, 1, &Host::bind_subcompositor);
    global(std::type_identity<zxdg_decoration_manager_v1>{}, 1, &Host::bind_decoration);
    global(std::type_identity<wl_data_device_manager>{}, 3, &Host::bind_data_device_manager);
    global(std::type_identity<wp_cursor_shape_manager_v1>{}, 2, &Host::bind_cursor_shape);
}

void Host::destroyed(Surface& s) {
    if (keyboard_focus_ == &s) keyboard_focus_ = nullptr;
    if (pointer_focus_ == &s) pointer_focus_ = nullptr;
    if (cursor_surface_ == &s) cursor_surface_ = nullptr;
    // Its acquire waits end with it; release points it holds are signalled
    // (no head will read a destroyed surface's buffers again).
    for (auto& [n, st] : s.staged) {
        waits_.erase(st.wait);
        if (st.release) syncobjs_.signal(st.release->timeline->handle, st.release->value);
    }
    s.staged.clear();
    for (auto& [b, h] : s.held)
        for (const auto& p : h.release) syncobjs_.signal(p.timeline->handle, p.value);
    s.held.clear();
    if (s.toplevel)
        if (const auto* m = std::get_if<life::Mapped>(&s.toplevel->life)) {
            windows_.erase(m->window);
            emit(ev::Unmapped{m->window});
        }
}

void Host::forget(srv::Client& c) {
    for (auto it = surfaces_.begin(); it != surfaces_.end();) {
        if (it->first.first != &c) {
            ++it;
            continue;
        }
        destroyed(*it->second);
        it = surfaces_.erase(it);
    }
    std::erase_if(buffers_, [&](const auto& kv) { return kv.first.first == &c; });
    std::erase_if(params_, [&](const auto& kv) { return kv.first.first == &c; });
    std::erase_if(feedbacks_, [&](const auto& f) { return f.first == &c; });
    std::erase_if(timelines_, [&](const auto& kv) { return kv.first.first == &c; });
    std::erase_if(offers_, [&](const auto& kv) { return kv.first.first == &c || kv.second.first == &c; });
    std::erase_if(sources_, [&](const auto& kv) { return kv.first.first == &c; });
    if (selection_ && selection_->first == &c) set_selection(std::nullopt);
    seats_.erase(&c);
}

// -- wl_compositor, wl_surface, wl_region -----------------------------------------------

void Host::bind_compositor(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wl_compositor>>();

    l->on<wl_compositor::requests::create_surface>([this, &client](srv::NewObject<wl_surface> s) {
        auto& surf = surfaces_[{&client, s.id()}];
        surf = std::make_unique<Surface>();
        surf->client = &client;
        surf->id = s.id();

        auto sl = std::make_shared<srv::RequestListener<wl_surface>>();
        const auto sid = s.id();
        sl->on<wl_surface::requests::attach>([this, &client, sid](Ref<wl_buffer> b, std::int32_t, std::int32_t) {
            if (auto* x = surface(&client, sid)) x->pending.attach = b ? std::optional<id_t>{b.id} : std::nullopt;
        });
        sl->on<wl_surface::requests::frame>([this, &client, sid](srv::NewObject<wl_callback> cb) {
            if (auto* x = surface(&client, sid)) x->pending.frames.push_back(cb.id());
        });
        sl->on<wl_surface::requests::commit>([this, &client, sid] {
            if (auto* x = surface(&client, sid)) committed(*x);
        });
        sl->on<wl_surface::requests::destroy>([this, &client, sid] {
            auto it = surfaces_.find({&client, sid});
            if (it == surfaces_.end()) return;
            destroyed(*it->second);
            surfaces_.erase(it);
        });
        // Accepted; i4 composites whole buffers at scale 1 for now.
        sl->on<wl_surface::requests::damage>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        sl->on<wl_surface::requests::damage_buffer>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        sl->on<wl_surface::requests::set_opaque_region>([](Ref<wl_region>) {});
        sl->on<wl_surface::requests::set_input_region>([](Ref<wl_region>) {});
        sl->on<wl_surface::requests::set_buffer_scale>([](std::int32_t) {});
        sl->on<wl_surface::requests::set_buffer_transform>([](wl_output_transform) {});
        sl->on<wl_surface::requests::offset>([this, &client, sid](std::int32_t dx, std::int32_t dy) {
            if (auto* x = surface(&client, sid)) {
                x->offset_x = dx;
                x->offset_y = dy;
            }
        });
        s.bind<compositor_version>(sl);
    });

    l->on<wl_compositor::requests::create_region>([](srv::NewObject<wl_region> r) {
        auto rl = std::make_shared<srv::RequestListener<wl_region>>();
        rl->on<wl_region::requests::add>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        rl->on<wl_region::requests::subtract>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        rl->on<wl_region::requests::destroy>([] {});
        r.bind<1>(rl);
    });

    client.bind_resource<wl_compositor, compositor_version>(id, l, version);
}

void Host::committed(Surface& s) {
    // linux-drm-syncobj's commit rules, checked before anything is applied.
    if (s.explicit_sync && s.pending.attach && *s.pending.attach) {
        using E = wp_linux_drm_syncobj_surface_v1::error;
        const auto bad = [&](E e, const char* why) {
            s.client->post_error(s.id, static_cast<std::uint32_t>(e), why);
        };
        if (!s.pending.acquire) return bad(E::no_acquire_point, "a buffer was committed without an acquire point");
        if (!s.pending.release) return bad(E::no_release_point, "a buffer was committed without a release point");
        if (s.pending.acquire->timeline == s.pending.release->timeline &&
            s.pending.acquire->value >= s.pending.release->value)
            return bad(E::conflicting_points, "the release point must come after the acquire point");
        if (!std::holds_alternative<DmabufBuffer>(buffers_[{s.client, **s.pending.attach}]))
            return bad(E::unsupported_buffer, "explicit sync needs a dmabuf");
    }
    const auto acquire = s.pending.attach && *s.pending.attach ? s.pending.acquire : std::nullopt;
    const auto release = s.pending.attach && *s.pending.attach ? s.pending.release : std::nullopt;
    const bool attached = s.pending.attach.has_value();
    const auto frames = commit(s.current, s.pending);
    const auto n = ++s.commits;

    if (!s.toplevel) {
        // Not a window (subsurface, popup, cursor): nothing shows it yet, so
        // its frames are answered now and the client keeps going; a release
        // point is signalled at once (the buffer is never read).
        if (s.cursor) cursor_committed(s);
        if (release) syncobjs_.signal(release->timeline->handle, release->value);
        answer_frames(*s.client, frames, now_ms());
        return;
    }
    auto& t = *s.toplevel;
    // A toplevel's first commit asks to be configured: to i4 that is the
    // window appearing. Its size is the window manager's to give.
    if (is<life::Unmapped>(t.life)) {
        const wm::WindowId w{next_window_++};
        t.life = life::Mapped{w};
        windows_[w] = {s.client, s.id};
        emit(ev::Mapped{w, t.info});
    }

    if (acquire) {
        // Explicit sync: the GPU may still be drawing it. It becomes the
        // content when the kernel says the acquire point has signalled (an
        // eventfd on the loop: no thread, no CPU wait, no head involved).
        Surface::Staged st{s.current.buffer, release, frames, 0};
        if (auto ev = syncobjs_.when(acquire->timeline->handle, acquire->value)) {
            auto token = kSyncBase + (next_wait_++ % kSyncSpan);
            while (waits_.contains(token)) token = kSyncBase + (next_wait_++ % kSyncSpan);
            if (auto reg = watch_(ev->get(), jaal::interest::read, token)) {
                st.wait = token;
                waits_.emplace(token, Waiting{&s, n, std::move(*ev), std::move(*reg)});
                s.staged.emplace(n, std::move(st));
                return;
            }
        }
        // The kernel would not wait for us: show it now rather than never.
        s.staged.emplace(n, std::move(st));
        acquired(s, n);
        return;
    }
    if (!attached && !s.staged.empty()) {
        // Only frame callbacks (or damage): they belong to the newest frame,
        // which is still on its way.
        auto& newest = s.staged.rbegin()->second.frames;
        newest.insert(newest.end(), frames.begin(), frames.end());
        return;
    }
    // Implicit sync (or a detach): ready now, and newer than anything staged.
    supersede(s, n);
    show(s, s.current.buffer, std::nullopt, frames);
}

void Host::acquired(Surface& s, std::uint64_t n) {
    auto it = s.staged.find(n);
    if (it == s.staged.end()) return;
    auto st = std::move(it->second);
    s.staged.erase(it);
    // Older commits still waiting are superseded by this one: never shown.
    // Their frame callbacks ride on this frame.
    for (auto old = s.staged.begin(); old != s.staged.end() && old->first < n;) {
        st.frames.insert(st.frames.begin(), old->second.frames.begin(), old->second.frames.end());
        if (old->second.release) syncobjs_.signal(old->second.release->timeline->handle, old->second.release->value);
        waits_.erase(old->second.wait);
        old = s.staged.erase(old);
    }
    if (n < s.content_commit) {
        // A newer commit is on screen already: this one never will be.
        if (st.release) syncobjs_.signal(st.release->timeline->handle, st.release->value);
        answer_frames(*s.client, st.frames, now_ms());
        return;
    }
    s.content_commit = n;
    show(s, st.buffer, std::move(st.release), st.frames);
}

void Host::supersede(Surface& s, std::uint64_t n) {
    std::vector<id_t> frames;
    for (auto& [k, st] : s.staged) {
        frames.insert(frames.end(), st.frames.begin(), st.frames.end());
        if (st.release) syncobjs_.signal(st.release->timeline->handle, st.release->value);
        waits_.erase(st.wait);
    }
    s.staged.clear();
    s.content_commit = n;
    answer_frames(*s.client, frames, now_ms());
}

void Host::show(Surface& s, std::optional<id_t> buffer, std::optional<SyncPoint> release,
                const std::vector<id_t>& frames) {
    auto& t = *s.toplevel;
    const auto w = std::get<life::Mapped>(t.life).window;
    if (buffer && buffers_.contains({s.client, *buffer})) {
        s.content = buffer;
        ++t.generation;
        for (auto cb : frames) s.owed.emplace(t.generation, cb);
        // A head reads this buffer's memory; it goes back to the client
        // only when that frame has been shown and released by every head.
        auto& h = s.held[*buffer];
        h.generation = t.generation;
        if (release) h.release.push_back(std::move(*release));
        emit(ev::Committed{w, t.generation});
        return;
    }
    // Detached: nothing is on screen from this surface any more.
    s.content.reset();
    if (release) syncobjs_.signal(release->timeline->handle, release->value);
    release_held(s, std::numeric_limits<std::uint64_t>::max());
    answer_frames(*s.client, frames, now_ms());
}

// -- wl_shm --------------------------------------------------------------------------

void Host::bind_shm(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wl_shm>>();

    l->on<wl_shm::requests::create_pool>([this, &client](srv::NewObject<wl_shm_pool> p, BorrowedFd fd, std::int32_t) {
        // It came by SCM_RIGHTS and is ours to close: own it at once.
        auto pool = std::make_shared<Pool>(Pool{OwnedFd{fd.get()}});
        auto pl = std::make_shared<srv::RequestListener<wl_shm_pool>>();
        pl->on<wl_shm_pool::requests::create_buffer>(
            [this, &client, pool](srv::NewObject<wl_buffer> b, std::int32_t offset, std::int32_t w, std::int32_t h,
                                  std::int32_t stride, wl_shm_format format) {
                buffers_[{&client, b.id()}] = ShmBuffer{pool, offset, w, h, stride, format};
                auto bl = std::make_shared<srv::RequestListener<wl_buffer>>();
                bl->on<wl_buffer::requests::destroy>([this, &client, bid = b.id()] { buffers_.erase({&client, bid}); });
                b.bind<1>(bl);
            });
        pl->on<wl_shm_pool::requests::resize>([](std::int32_t) {});  // the fd is the same; heads map its size
        pl->on<wl_shm_pool::requests::destroy>([] {});                 // buffers keep the pool alive
        p.bind<1>(pl);
    });

    auto shm = client.bind_resource<wl_shm, 2>(id, l, version);
    shm.send<wl_shm::events::format>(wl_shm::format::argb8888);
    shm.send<wl_shm::events::format>(wl_shm::format::xrgb8888);
}

// -- xdg-shell -------------------------------------------------------------------------

void Host::bind_xdg(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<xdg_wm_base>>();

    l->on<xdg_wm_base::requests::create_positioner>([](srv::NewObject<xdg_positioner> p) {
        auto pl = std::make_shared<srv::RequestListener<xdg_positioner>>();
        pl->on<xdg_positioner::requests::destroy>([] {});
        pl->on<xdg_positioner::requests::set_size>([](std::int32_t, std::int32_t) {});
        pl->on<xdg_positioner::requests::set_anchor_rect>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        pl->on<xdg_positioner::requests::set_anchor>([](xdg_positioner_anchor) {});
        pl->on<xdg_positioner::requests::set_gravity>([](xdg_positioner_gravity) {});
        pl->on<xdg_positioner::requests::set_constraint_adjustment>([](xdg_positioner_constraint_adjustment) {});
        pl->on<xdg_positioner::requests::set_offset>([](std::int32_t, std::int32_t) {});
        pl->on<xdg_positioner::requests::set_reactive>([] {});
        pl->on<xdg_positioner::requests::set_parent_size>([](std::int32_t, std::int32_t) {});
        pl->on<xdg_positioner::requests::set_parent_configure>([](std::uint32_t) {});
        p.bind<xdg_version>(pl);
    });

    l->on<xdg_wm_base::requests::get_xdg_surface>([this, &client, version](srv::NewObject<xdg_surface> xs, Ref<wl_surface> ws) {
        const id_t sid = ws.id;
        const id_t xsid = xs.id();
        auto xl = std::make_shared<srv::RequestListener<xdg_surface>>();

        xl->on<xdg_surface::requests::get_toplevel>([this, &client, sid, xsid, version](srv::NewObject<xdg_toplevel> t) {
            auto* s = surface(&client, sid);
            if (!s) return;
            s->toplevel.emplace();
            s->toplevel->xdg_surface = xsid;
            s->toplevel->xdg_toplevel = t.id();
            s->toplevel->version = version;

            // A change to what the window manager knows about the window.
            auto changed = [this, &client, sid](auto&& edit) {
                auto* x = surface(&client, sid);
                if (!x || !x->toplevel) return;
                edit(x->toplevel->info);
                if (const auto* m = std::get_if<life::Mapped>(&x->toplevel->life))
                    emit(ev::Retitled{m->window, x->toplevel->info});
            };
            auto tl = std::make_shared<srv::RequestListener<xdg_toplevel>>();
            tl->on<xdg_toplevel::requests::set_title>(
                [changed](std::string_view v) { changed([&](wm::WindowInfo& i) { i.title = std::string(v); }); });
            tl->on<xdg_toplevel::requests::set_app_id>(
                [changed](std::string_view v) { changed([&](wm::WindowInfo& i) { i.app_id = std::string(v); }); });
            // A transient (a dialog) floats, as in i3.
            tl->on<xdg_toplevel::requests::set_parent>(
                [changed](Ref<xdg_toplevel> p) { changed([&](wm::WindowInfo& i) { i.floating = bool(p); }); });
            tl->on<xdg_toplevel::requests::set_min_size>([](std::int32_t, std::int32_t) {});
            tl->on<xdg_toplevel::requests::set_max_size>([](std::int32_t, std::int32_t) {});
            tl->on<xdg_toplevel::requests::set_maximized>([] {});
            tl->on<xdg_toplevel::requests::unset_maximized>([] {});
            tl->on<xdg_toplevel::requests::set_minimized>([] {});
            tl->on<xdg_toplevel::requests::set_fullscreen>([](Ref<wl_output>) {});
            tl->on<xdg_toplevel::requests::unset_fullscreen>([] {});
            tl->on<xdg_toplevel::requests::move>([](Ref<wl_seat>, std::uint32_t) {});
            tl->on<xdg_toplevel::requests::resize>([](Ref<wl_seat>, std::uint32_t, xdg_toplevel_resize_edge) {});
            tl->on<xdg_toplevel::requests::show_window_menu>([](Ref<wl_seat>, std::uint32_t, std::int32_t, std::int32_t) {});
            tl->on<xdg_toplevel::requests::destroy>([this, &client, sid] {
                auto* x = surface(&client, sid);
                if (!x) return;
                destroyed(*x);
                x->toplevel.reset();
            });
            t.bind<xdg_version>(tl);
        });

        xl->on<xdg_surface::requests::get_popup>([&client, xsid](srv::NewObject<xdg_popup> p, Ref<xdg_surface>, Ref<xdg_positioner>) {
            // Configured so the client never waits; popup content is not
            // composited yet.
            auto pl = std::make_shared<srv::RequestListener<xdg_popup>>();
            pl->on<xdg_popup::requests::grab>([](Ref<wl_seat>, std::uint32_t) {});
            pl->on<xdg_popup::requests::reposition>([](Ref<xdg_positioner>, std::uint32_t) {});
            pl->on<xdg_popup::requests::destroy>([] {});
            auto popup = p.bind<xdg_version>(pl);
            popup.send<xdg_popup::events::configure>(0, 0, 1, 1);
            srv::Resource<xdg_surface, xdg_version>{&client, xsid}.send<xdg_surface::events::configure>(
                client.display()->next_serial());
        });
        xl->on<xdg_surface::requests::set_window_geometry>([](std::int32_t, std::int32_t, std::int32_t, std::int32_t) {});
        xl->on<xdg_surface::requests::ack_configure>([](std::uint32_t) {});
        xl->on<xdg_surface::requests::destroy>([] {});
        xs.bind<xdg_version>(xl);
    });

    l->on<xdg_wm_base::requests::pong>([](std::uint32_t) {});
    l->on<xdg_wm_base::requests::destroy>([] {});
    client.bind_resource<xdg_wm_base, xdg_version>(id, l, version);
}

// -- seat ------------------------------------------------------------------------------

void Host::bind_seat(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wl_seat>>();
    const std::uint32_t v = std::min(version, seat_version);

    l->on<wl_seat::requests::get_keyboard>([this, &client, v](srv::NewObject<wl_keyboard> k) {
        auto kl = std::make_shared<srv::RequestListener<wl_keyboard>>();
        kl->on<wl_keyboard::requests::release>([this, &client, kid = k.id()] {
            std::erase_if(seats_[&client].keyboards, [&](const auto& e) { return e.first == kid; });
        });
        auto kb = k.bind<seat_version>(kl);
        seats_[&client].keyboards.emplace_back(k.id(), v);
        // The keymap first: a key before its layout means nothing.
        send_keymap(client, k.id(), v);
        if (keyboard_focus_ && keyboard_focus_->client == &client) {
            kb.send<wl_keyboard::events::enter>(
                display_.next_serial(), nonnull<wl_surface>(keyboard_focus_->id),
                std::span<const std::byte>{reinterpret_cast<const std::byte*>(pressed_.data()),
                                           pressed_.size() * sizeof(std::uint32_t)});
            send_modifiers(client, k.id());
        }
    });
    l->on<wl_seat::requests::get_pointer>([this, &client, v](srv::NewObject<wl_pointer> p) {
        auto pl = std::make_shared<srv::RequestListener<wl_pointer>>();
        pl->on<wl_pointer::requests::set_cursor>(
            [this, &client](std::uint32_t, Ref<wl_surface> ws, std::int32_t hx, std::int32_t hy) {
                // Only the client the pointer is over decides how it looks.
                if (!pointer_focus_ || pointer_focus_->client != &client) return;
                if (!ws) {
                    cursor_surface_ = nullptr;
                    publish_cursor({link::down::CursorHidden{}});
                    return;
                }
                auto* s = surface(&client, ws.id);
                if (!s || s->toplevel) return;  // a window cannot also be a cursor
                s->cursor = true;
                s->hot_x = hx;
                s->hot_y = hy;
                cursor_surface_ = s;
                // Usually the surface was committed before this call: show it now.
                if (s->current.buffer) cursor_committed(*s);
            });
        pl->on<wl_pointer::requests::release>([this, &client, pid = p.id()] {
            std::erase_if(seats_[&client].pointers, [&](const auto& e) { return e.first == pid; });
        });
        p.bind<seat_version>(pl);
        seats_[&client].pointers.emplace_back(p.id(), v);
    });
    l->on<wl_seat::requests::get_touch>([](srv::NewObject<wl_touch> t) {
        auto tl = std::make_shared<srv::RequestListener<wl_touch>>();
        tl->on<wl_touch::requests::release>([] {});
        t.bind<1>(tl);
    });
    l->on<wl_seat::requests::release>([] {});

    auto seat = client.bind_resource<wl_seat, seat_version>(id, l, version);
    seat.send<wl_seat::events::capabilities>(static_cast<wl_seat::capability>(
        static_cast<std::uint32_t>(wl_seat::capability::pointer) | static_cast<std::uint32_t>(wl_seat::capability::keyboard)));
    if (v >= 2) seat.send<wl_seat::events::name>("seat0");
}

void Host::bind_output(srv::Client& client, std::uint32_t version, id_t id) {
    // One fixed output advertised for now; heads report real ones next.
    auto l = std::make_shared<srv::RequestListener<wl_output>>();
    l->on<wl_output::requests::release>([] {});
    auto out = client.bind_resource<wl_output, 4>(id, l, version);
    out.send<wl_output::events::geometry>(0, 0, 0, 0, wl_output::subpixel::unknown, "i4", "i4", wl_output::transform::normal);
    out.send<wl_output::events::mode>(wl_output::mode::current, 1920, 1080, 60000);
    if (version >= 2) out.send<wl_output::events::scale>(1);
    if (version >= 4) out.send<wl_output::events::name>("i4-0");
    if (version >= 2) out.send<wl_output::events::done>();
}

void Host::bind_subcompositor(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wl_subcompositor>>();
    l->on<wl_subcompositor::requests::get_subsurface>([](srv::NewObject<wl_subsurface> s, Ref<wl_surface>, Ref<wl_surface>) {
        auto sl = std::make_shared<srv::RequestListener<wl_subsurface>>();
        sl->on<wl_subsurface::requests::set_position>([](std::int32_t, std::int32_t) {});
        sl->on<wl_subsurface::requests::place_above>([](Ref<wl_surface>) {});
        sl->on<wl_subsurface::requests::place_below>([](Ref<wl_surface>) {});
        sl->on<wl_subsurface::requests::set_sync>([] {});
        sl->on<wl_subsurface::requests::set_desync>([] {});
        sl->on<wl_subsurface::requests::destroy>([] {});
        s.bind<1>(sl);
    });
    l->on<wl_subcompositor::requests::destroy>([] {});
    client.bind_resource<wl_subcompositor, 1>(id, l, version);
}

/// i4 draws borders and i3 title bars, so every window is server-side.
void Host::bind_decoration(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<zxdg_decoration_manager_v1>>();
    l->on<zxdg_decoration_manager_v1::requests::get_toplevel_decoration>(
        [](srv::NewObject<zxdg_toplevel_decoration_v1> d, Ref<xdg_toplevel>) {
            auto dl = std::make_shared<srv::RequestListener<zxdg_toplevel_decoration_v1>>();
            dl->on<zxdg_toplevel_decoration_v1::requests::set_mode>([](zxdg_toplevel_decoration_v1_mode) {});
            dl->on<zxdg_toplevel_decoration_v1::requests::unset_mode>([] {});
            dl->on<zxdg_toplevel_decoration_v1::requests::destroy>([] {});
            d.bind<1>(dl).send<zxdg_toplevel_decoration_v1::events::configure>(zxdg_toplevel_decoration_v1::mode::server_side);
        });
    l->on<zxdg_decoration_manager_v1::requests::destroy>([] {});
    client.bind_resource<zxdg_decoration_manager_v1, 1>(id, l, version);
}

// -- effects: the Session decided, a client is told ------------------------------------------

void Host::handle(const fx::Configure& e) {
    auto* s = surface(e.window);
    if (!s || !s->toplevel) return;
    const auto& t = *s->toplevel;
    std::vector<std::uint32_t> states;
    if (e.activated) states.push_back(static_cast<std::uint32_t>(xdg_toplevel::state::activated));
    // Tiled on every edge (v2+): no client shadows, corners or resize handles.
    if (t.version >= 2)
        for (auto st : {xdg_toplevel::state::tiled_left, xdg_toplevel::state::tiled_right,
                        xdg_toplevel::state::tiled_top, xdg_toplevel::state::tiled_bottom})
            states.push_back(static_cast<std::uint32_t>(st));
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(states.data()), states.size() * 4};
    srv::Resource<xdg_toplevel, xdg_version>{s->client, t.xdg_toplevel}.send<xdg_toplevel::events::configure>(
        std::max(1, e.size.width), std::max(1, e.size.height), bytes);
    srv::Resource<xdg_surface, xdg_version>{s->client, t.xdg_surface}.send<xdg_surface::events::configure>(display_.next_serial());
}

void Host::handle(const fx::Close& e) {
    if (auto* s = surface(e.window); s && s->toplevel)
        srv::Resource<xdg_toplevel, xdg_version>{s->client, s->toplevel->xdg_toplevel}.send<xdg_toplevel::events::close>();
}

void Host::handle(const fx::KeyboardFocus& e) {
    Surface* to = e.window ? surface(*e.window) : nullptr;
    if (to == keyboard_focus_) return;
    if (keyboard_focus_)
        for (auto [k, v] : seats_[keyboard_focus_->client].keyboards)
            srv::Resource<wl_keyboard, seat_version>{keyboard_focus_->client, k}.send<wl_keyboard::events::leave>(
                display_.next_serial(), nonnull<wl_surface>(keyboard_focus_->id));
    keyboard_focus_ = to;
    if (to) {
        // Keys already held enter with the focus (wl_keyboard.enter's
        // array), so a client never sees the release of a key it never saw
        // go down, nor misses that one is down.
        const std::span<const std::byte> held{reinterpret_cast<const std::byte*>(pressed_.data()),
                                              pressed_.size() * sizeof(std::uint32_t)};
        for (auto [k, v] : seats_[to->client].keyboards) {
            srv::Resource<wl_keyboard, seat_version> kb{to->client, k};
            kb.send<wl_keyboard::events::enter>(display_.next_serial(), nonnull<wl_surface>(to->id), held);
            send_modifiers(*to->client, k);
        }
        // The protocol's rule: a client sees the clipboard when it gains
        // keyboard focus, just before its first key.
        offer_selection(*to->client);
    }
}

void Host::send_modifiers(srv::Client& c, id_t keyboard) {
    srv::Resource<wl_keyboard, seat_version> kb{&c, keyboard};
    if (xkb_mods_)
        kb.send<wl_keyboard::events::modifiers>(display_.next_serial(), xkb_mods_->depressed, xkb_mods_->latched,
                                                xkb_mods_->locked, xkb_mods_->group);
    else if (keymap_)
        kb.send<wl_keyboard::events::modifiers>(display_.next_serial(), keymap_->mask(mods_), 0, 0, 0);
}

void Host::send_keymap(srv::Client& c, id_t keyboard, std::uint32_t version) {
    srv::Resource<wl_keyboard, seat_version> kb{&c, keyboard};
    if (keymap_) kb.send<wl_keyboard::events::keymap>(wl_keyboard::keymap_format::xkb_v1, keymap_->fd(), keymap_->size());
    if (version >= 4) kb.send<wl_keyboard::events::repeat_info>(repeat_rate_, repeat_delay_);
}

void Host::handle(const fx::Key& e) {
    // A head without exact xkb state: its coarse Mods become masks in the
    // session's keymap, sent when they change.
    const bool mods_changed = !xkb_mods_ && e.mods != mods_;
    if (!xkb_mods_) mods_ = e.mods;
    if (e.pressed) {
        if (std::ranges::find(pressed_, e.code) != pressed_.end()) return;  // already down
        pressed_.push_back(e.code);
    } else if (std::erase(pressed_, e.code) == 0) {
        return;  // was never down here
    }
    if (!keyboard_focus_) return;
    for (auto [k, v] : seats_[keyboard_focus_->client].keyboards) {
        srv::Resource<wl_keyboard, seat_version> kb{keyboard_focus_->client, k};
        if (mods_changed) send_modifiers(*keyboard_focus_->client, k);
        kb.send<wl_keyboard::events::key>(display_.next_serial(), now_ms(), e.code,
                                          e.pressed ? wl_keyboard::key_state::pressed : wl_keyboard::key_state::released);
    }
}

void Host::handle(const fx::Keyboard& e) {
    auto k = Keymap::from_text(e.keyboard.keymap);
    if (!k) {
        std::fprintf(stderr, "i4d: %s; keeping the current keymap\n", k.error().c_str());
        return;
    }
    keymap_.emplace(std::move(*k));
    repeat_rate_ = e.keyboard.rate;
    repeat_delay_ = e.keyboard.delay;
    // Every keyboard learns the new layout at once; the focused one gets
    // the modifier state again, which the protocol says follows a keymap.
    for (auto& [c, seat] : seats_)
        for (auto [kb, v] : seat.keyboards) {
            send_keymap(*c, kb, v);
            if (keyboard_focus_ && keyboard_focus_->client == c) send_modifiers(*c, kb);
        }
}

void Host::handle(const fx::Modifiers& e) {
    if (xkb_mods_ == e.mods) return;
    xkb_mods_ = e.mods;
    if (!keyboard_focus_) return;
    for (auto [k, v] : seats_[keyboard_focus_->client].keyboards) send_modifiers(*keyboard_focus_->client, k);
}

void Host::handle(const fx::Scroll& e) {
    if (!pointer_focus_) return;
    const auto& s = e.scroll;
    const auto t = now_ms();
    for (auto [p, v] : seats_[pointer_focus_->client].pointers) {
        srv::Resource<wl_pointer, seat_version> r{pointer_focus_->client, p};
        // axis_source/axis_stop/axis_discrete are v5+, axis_value120 v8+;
        // older pointers get the smooth values alone.
        if (v >= 5 && s.source <= 3) r.send<wl_pointer::events::axis_source>(static_cast<wl_pointer::axis_source>(s.source));
        auto axis = [&](wl_pointer::axis a, std::int32_t value, std::int32_t v120, bool stop) {
            if (v120 != 0) {
                if (v >= 8)
                    r.send<wl_pointer::events::axis_value120>(a, v120);
                else if (v >= 5 && v120 % 120 == 0)
                    r.send<wl_pointer::events::axis_discrete>(a, v120 / 120);
            }
            if (value != 0) r.send<wl_pointer::events::axis>(t, a, fixed::from_raw(value));
            if (stop && v >= 5) r.send<wl_pointer::events::axis_stop>(t, a);
        };
        axis(wl_pointer::axis::vertical_scroll, s.dy, s.dy120, s.stop_y);
        axis(wl_pointer::axis::horizontal_scroll, s.dx, s.dx120, s.stop_x);
        if (v >= 5) r.send<wl_pointer::events::frame>();
    }
}

void Host::handle(const fx::PointerAt& e) {
    Surface* over = e.window ? surface(*e.window) : nullptr;
    const auto x = fixed::from_int(e.at.x), y = fixed::from_int(e.at.y);
    auto each = [&](Surface* s, auto&& f) {
        if (!s) return;
        for (auto [p, v] : seats_[s->client].pointers) {
            srv::Resource<wl_pointer, seat_version> r{s->client, p};
            f(r);
            if (v >= 5) r.send<wl_pointer::events::frame>();
        }
    };
    if (over != pointer_focus_) {
        each(pointer_focus_, [&](auto& r) {
            r.template send<wl_pointer::events::leave>(display_.next_serial(), nonnull<wl_surface>(pointer_focus_->id));
        });
        pointer_focus_ = over;
        // The new client sets its own cursor on enter; over nothing (the
        // gaps, the bar) the pointer is the default arrow.
        cursor_surface_ = nullptr;
        if (!over) publish_cursor({link::down::CursorShape{1}});
        each(over, [&](auto& r) {
            r.template send<wl_pointer::events::enter>(display_.next_serial(), nonnull<wl_surface>(over->id), x, y);
        });
        return;
    }
    each(over, [&](auto& r) { r.template send<wl_pointer::events::motion>(now_ms(), x, y); });
}

void Host::handle(const fx::Button& e) {
    if (!pointer_focus_) return;
    for (auto [p, v] : seats_[pointer_focus_->client].pointers) {
        srv::Resource<wl_pointer, seat_version> r{pointer_focus_->client, p};
        r.send<wl_pointer::events::button>(display_.next_serial(), now_ms(), e.button,
                                           e.pressed ? wl_pointer::button_state::pressed : wl_pointer::button_state::released);
        if (v >= 5) r.send<wl_pointer::events::frame>();
    }
}

void Host::handle(const fx::FramesDone& e) {
    auto* s = surface(e.window);
    if (!s) return;
    const auto end = s->owed.upper_bound(e.generation);
    std::vector<id_t> due;
    for (auto it = s->owed.begin(); it != end; ++it) due.push_back(it->second);
    s->owed.erase(s->owed.begin(), end);
    answer_frames(*s->client, due, now_ms());
}

void Host::handle(const fx::Release& e) {
    if (auto* s = surface(e.window)) release_held(*s, e.generation);
}

void Host::release_held(Surface& s, std::uint64_t shown) {
    for (auto it = s.held.begin(); it != s.held.end();) {
        const bool done = it->second.generation <= shown && it->first != s.content;
        if (done) {
            // Explicit sync: the client waits on its release points, not on
            // wl_buffer.release (whose delivery the protocol leaves undefined).
            for (const auto& p : it->second.release) syncobjs_.signal(p.timeline->handle, p.value);
            if (it->second.release.empty() && buffers_.contains({s.client, it->first}))
                srv::Resource<wl_buffer, 1>{s.client, it->first}.send<wl_buffer::events::release>();
        }
        it = done ? s.held.erase(it) : std::next(it);
    }
}

// -- linux-dmabuf: GPU buffers, zero copy end to end -------------------------------------
//
// v4 feedback: one tranche, on the render node, listing exactly what the
// heads said they can sample in place (fx::Formats). An app that follows it
// renders into memory the presenter reads as it is.

void Host::rebuild_format_table() {
    std::vector<std::byte> table(formats_.size() * 16);
    for (std::size_t i = 0; i < formats_.size(); ++i) {
        std::memcpy(table.data() + i * 16, &formats_[i].fourcc, 4);
        std::memcpy(table.data() + i * 16 + 8, &formats_[i].modifier, 8);
    }
    format_table_ = {};
    format_table_size_ = static_cast<std::uint32_t>(table.size());
    if (table.empty()) return;
    wl::OwnedFd fd{::memfd_create("i4-dmabuf-formats", MFD_CLOEXEC | MFD_ALLOW_SEALING)};
    if (!fd || ::ftruncate(fd.get(), static_cast<off_t>(table.size())) != 0 ||
        ::pwrite(fd.get(), table.data(), table.size(), 0) != static_cast<ssize_t>(table.size()))
        return;
    // Shared with every client read-only: sealed so no one can change it.
    ::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL);
    format_table_ = std::move(fd);
}

void Host::handle(const fx::Formats& e) {
    formats_ = e.formats;
    rebuild_format_table();
    // linux-dmabuf exists exactly while some head can show GPU buffers. With
    // none, an app that bound it would allocate a layout nobody can sample;
    // without it, every app falls back to shm, which always works.
    if (formats_.empty() && dmabuf_global_) {
        display_.remove_global(*dmabuf_global_);
        dmabuf_global_.reset();
    } else if (!formats_.empty() && !dmabuf_global_) {
        dmabuf_global_ = display_.add_global<zwp_linux_dmabuf_v1>(
            4, [this](srv::Client& c, std::uint32_t v, id_t id) { bind_dmabuf(c, v, id); });
    }
    // linux-drm-syncobj exists while clients may render on the GPU and the
    // kernel can wait on timelines for us: the session keeps the promises
    // itself, so it needs nothing from the heads.
    const bool sync = dmabuf_global_.has_value() && syncobjs_.available();
    if (!sync && syncobj_global_) {
        display_.remove_global(*syncobj_global_);
        syncobj_global_.reset();
    } else if (sync && !syncobj_global_) {
        syncobj_global_ = display_.add_global<wp_linux_drm_syncobj_manager_v1>(
            1, [this](srv::Client& c, std::uint32_t v, id_t id) { bind_syncobj(c, v, id); });
    }
    // Clients that already asked hear the new table now (v4: feedback is
    // live; a client reallocates on the next frame).
    std::erase_if(feedbacks_, [this](const auto& f) { return !f.first->alive(); });
    for (const auto& [c, id] : feedbacks_) send_feedback(*c, id);
}

void Host::send_feedback(srv::Client& c, id_t id) {
    srv::Resource<zwp_linux_dmabuf_feedback_v1, 4> fb{&c, id};
    const std::span<const std::byte> dev{reinterpret_cast<const std::byte*>(&main_device_), sizeof main_device_};
    if (format_table_) {
        fb.send<zwp_linux_dmabuf_feedback_v1::events::format_table>(BorrowedFd{format_table_.get()}, format_table_size_);
    }
    fb.send<zwp_linux_dmabuf_feedback_v1::events::main_device>(dev);
    if (!formats_.empty()) {
        std::vector<std::uint16_t> idx(formats_.size());
        for (std::size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<std::uint16_t>(i);
        fb.send<zwp_linux_dmabuf_feedback_v1::events::tranche_target_device>(dev);
        fb.send<zwp_linux_dmabuf_feedback_v1::events::tranche_flags>(
            static_cast<zwp_linux_dmabuf_feedback_v1::tranche_flags>(0));
        fb.send<zwp_linux_dmabuf_feedback_v1::events::tranche_formats>(
            std::span<const std::byte>{reinterpret_cast<const std::byte*>(idx.data()), idx.size() * 2});
        fb.send<zwp_linux_dmabuf_feedback_v1::events::tranche_done>();
    }
    fb.send<zwp_linux_dmabuf_feedback_v1::events::done>();
}

void Host::bind_dmabuf(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<zwp_linux_dmabuf_v1>>();

    auto feedback = [this, &client](srv::NewObject<zwp_linux_dmabuf_feedback_v1> f) {
        auto fl = std::make_shared<srv::RequestListener<zwp_linux_dmabuf_feedback_v1>>();
        fl->on<zwp_linux_dmabuf_feedback_v1::requests::destroy>([this, &client, fid = f.id()] {
            std::erase(feedbacks_, std::pair{&client, fid});
        });
        f.bind<4>(fl);
        feedbacks_.emplace_back(&client, f.id());
        send_feedback(client, f.id());
    };
    l->on<zwp_linux_dmabuf_v1::requests::get_default_feedback>(feedback);
    l->on<zwp_linux_dmabuf_v1::requests::get_surface_feedback>(
        [feedback](srv::NewObject<zwp_linux_dmabuf_feedback_v1> f, Ref<wl_surface>) { feedback(f); });

    l->on<zwp_linux_dmabuf_v1::requests::create_params>([this, &client](srv::NewObject<zwp_linux_buffer_params_v1> p) {
        const Key key{&client, p.id()};
        params_[key] = {};
        auto pl = std::make_shared<srv::RequestListener<zwp_linux_buffer_params_v1>>();
        pl->on<zwp_linux_buffer_params_v1::requests::add>(
            [this, key](BorrowedFd fd, std::uint32_t plane, std::uint32_t offset, std::uint32_t stride,
                        std::uint32_t mod_hi, std::uint32_t mod_lo) {
                // Arrived by SCM_RIGHTS: ours to close, so own it at once.
                auto owned = std::make_shared<wl::OwnedFd>(fd.get());
                auto it = params_.find(key);
                if (it == params_.end() || plane > 3 || it->second.planes.contains(plane)) return;
                it->second.planes[plane] = {std::move(owned), offset, stride};
                it->second.modifier = (std::uint64_t{mod_hi} << 32) | mod_lo;
                it->second.modifier_set = true;
            });
        // A buffer from params, or nullopt when they are not a complete,
        // offered layout. Checked HERE: a client may only hand the head
        // what the head said it can sample.
        auto build = [this, key](std::int32_t w, std::int32_t h, std::uint32_t fourcc) -> std::optional<DmabufBuffer> {
            auto it = params_.find(key);
            if (it == params_.end() || it->second.used || it->second.planes.empty() || w <= 0 || h <= 0)
                return std::nullopt;
            it->second.used = true;
            DmabufBuffer b;
            b.fourcc = fourcc;
            b.modifier = it->second.modifier;
            b.width = w;
            b.height = h;
            b.id = next_buffer_id_++;
            for (std::uint32_t i = 0; i < it->second.planes.size(); ++i) {
                auto plane = it->second.planes.find(i);
                if (plane == it->second.planes.end()) return std::nullopt;  // planes must be 0..n-1
                b.planes.push_back(plane->second);
            }
            if (std::ranges::find(formats_, link::up::Format{fourcc, b.modifier}) == formats_.end())
                return std::nullopt;
            return b;
        };
        auto adopt = [this, &client](srv::NewObject<wl_buffer> nb, DmabufBuffer b) {
            buffers_[{&client, nb.id()}] = std::move(b);
            auto bl = std::make_shared<srv::RequestListener<wl_buffer>>();
            bl->on<wl_buffer::requests::destroy>([this, &client, bid = nb.id()] { buffers_.erase({&client, bid}); });
            nb.bind<1>(bl);
        };
        pl->on<zwp_linux_buffer_params_v1::requests::create_immed>(
            [build, adopt, pid = p.id(), &client](srv::NewObject<wl_buffer> nb, std::int32_t w, std::int32_t h,
                                                  std::uint32_t fourcc, zwp_linux_buffer_params_v1_flags) {
                auto b = build(w, h, fourcc);
                if (!b) {
                    srv::Resource<zwp_linux_buffer_params_v1, 4>{&client, pid}.post_error(
                        zwp_linux_buffer_params_v1::error::invalid_wl_buffer, "i4: not a layout the presenter can sample");
                    return;
                }
                adopt(nb, std::move(*b));
            });
        pl->on<zwp_linux_buffer_params_v1::requests::create>(
            [build, adopt, pid = p.id(), &client](std::int32_t w, std::int32_t h, std::uint32_t fourcc,
                                                  zwp_linux_buffer_params_v1_flags) {
                srv::Resource<zwp_linux_buffer_params_v1, 4> params{&client, pid};
                auto b = build(w, h, fourcc);
                if (!b) {
                    params.send<zwp_linux_buffer_params_v1::events::failed>();
                    return;
                }
                // The server creates the wl_buffer: an id from the server's
                // range, announced by `created`.
                const id_t nid = params.send<zwp_linux_buffer_params_v1::events::created>();
                adopt(srv::NewObject<wl_buffer>{&client, nid, pid}, std::move(*b));
            });
        pl->on<zwp_linux_buffer_params_v1::requests::destroy>([this, key] { params_.erase(key); });
        p.bind<4>(pl);
    });
    l->on<zwp_linux_dmabuf_v1::requests::destroy>([] {});

    auto d = client.bind_resource<zwp_linux_dmabuf_v1, 4>(id, l, version);
    // v3 and older learn formats from these events instead of feedback.
    if (version < 4)
        for (const auto& f : formats_) {
            d.send<zwp_linux_dmabuf_v1::events::format>(f.fourcc);
            if (version >= 3)
                d.send<zwp_linux_dmabuf_v1::events::modifier>(f.fourcc, static_cast<std::uint32_t>(f.modifier >> 32),
                                                              static_cast<std::uint32_t>(f.modifier));
        }
}

// -- linux-drm-syncobj: explicit sync, end to end on the GPU -------------------------------
//
// A client's timelines are imported once and forwarded once to each head
// (down::Timeline, fd beside it); each frame then names its acquire and
// release points. The head's GPU waits for the acquire point and signals
// the release point. i4d never waits on either.

void Host::bind_syncobj(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wp_linux_drm_syncobj_manager_v1>>();
    using M = wp_linux_drm_syncobj_manager_v1;

    l->on<M::requests::import_timeline>([this, &client, mid = id](srv::NewObject<wp_linux_drm_syncobj_timeline_v1> t,
                                                                 BorrowedFd fd) {
        // It came by SCM_RIGHTS and is ours: the kernel's handle holds the
        // syncobj, so the fd closes as soon as it is imported.
        const wl::OwnedFd owned{fd.get()};
        const auto handle = syncobjs_.import(owned.get());
        if (handle == 0) {
            srv::Resource<M, 1>{&client, mid}.post_error(M::error::invalid_timeline, "not a DRM timeline syncobj");
            return;
        }
        timelines_[{&client, t.id()}] = std::make_shared<Timeline>(&syncobjs_, handle);
        auto tl = std::make_shared<srv::RequestListener<wp_linux_drm_syncobj_timeline_v1>>();
        tl->on<wp_linux_drm_syncobj_timeline_v1::requests::destroy>([this, &client, oid = t.id()] {
            // Points still pending on it keep it alive (shared).
            timelines_.erase({&client, oid});
        });
        t.bind<1>(tl);
    });

    l->on<M::requests::get_surface>([this, &client, mid = id](srv::NewObject<wp_linux_drm_syncobj_surface_v1> ss,
                                                             Ref<wl_surface> ws) {
        auto* surf = surface(&client, ws.id);
        if (!surf) return;
        if (surf->explicit_sync) {
            srv::Resource<M, 1>{&client, mid}.post_error(M::error::surface_exists, "the surface already has one");
            return;
        }
        surf->explicit_sync = true;
        auto sl = std::make_shared<srv::RequestListener<wp_linux_drm_syncobj_surface_v1>>();
        const auto point = [this, &client](Ref<wp_linux_drm_syncobj_timeline_v1> t, std::uint32_t hi,
                                           std::uint32_t lo) -> std::optional<SyncPoint> {
            auto it = timelines_.find({&client, t.id});
            if (it == timelines_.end()) return std::nullopt;
            return SyncPoint{it->second, (std::uint64_t{hi} << 32) | lo};
        };
        sl->on<wp_linux_drm_syncobj_surface_v1::requests::set_acquire_point>(
            [this, &client, sid = ws.id, point](Ref<wp_linux_drm_syncobj_timeline_v1> t, std::uint32_t hi, std::uint32_t lo) {
                if (auto* x = surface(&client, sid)) x->pending.acquire = point(t, hi, lo);
            });
        sl->on<wp_linux_drm_syncobj_surface_v1::requests::set_release_point>(
            [this, &client, sid = ws.id, point](Ref<wp_linux_drm_syncobj_timeline_v1> t, std::uint32_t hi, std::uint32_t lo) {
                if (auto* x = surface(&client, sid)) x->pending.release = point(t, hi, lo);
            });
        sl->on<wp_linux_drm_syncobj_surface_v1::requests::destroy>([this, &client, sid = ws.id] {
            if (auto* x = surface(&client, sid)) x->explicit_sync = false;
        });
        ss.bind<1>(sl);
    });
    l->on<M::requests::destroy>([] {});
    client.bind_resource<M, 1>(id, l, version);
}

// -- cursor ------------------------------------------------------------------------
//
// The pointer is drawn by the head's platform, on its own cursor plane, so
// it moves at the head machine's speed and never waits on the link or on a
// client. i4d only says what it should look like: a named shape (the head's
// theme draws it), the client's own image, or nothing.
//
// A cursor image is the one place i4d reads client pixels: it is tiny (24-64
// px square, a few KB), changes rarely, and copying it lets the buffer go
// back to the client at once instead of across a network round trip.

void Host::publish_cursor(link::down::Cursor c) { emit(ev::CursorChanged{std::move(c)}); }

void Host::cursor_committed(Surface& s) {
    // wl_surface.offset moves the image; the hotspot stays on the pointer.
    s.hot_x -= std::exchange(s.offset_x, 0);
    s.hot_y -= std::exchange(s.offset_y, 0);
    if (!s.current.buffer) return;
    const id_t bid = *s.current.buffer;
    auto it = buffers_.find({s.client, bid});
    if (it == buffers_.end()) return;
    const auto give_back = [&] { srv::Resource<wl_buffer, 1>{s.client, bid}.send<wl_buffer::events::release>(); };
    if (&s != cursor_surface_) return give_back();

    const auto* shm = std::get_if<ShmBuffer>(&it->second);
    const bool argb = shm && shm->format == wl_shm::format::argb8888;
    const bool xrgb = shm && shm->format == wl_shm::format::xrgb8888;
    if (!argb && !xrgb) {
        // A GPU cursor buffer (rare): i4d never maps those. The arrow it is.
        publish_cursor({link::down::CursorShape{1}});
        return give_back();
    }
    link::down::CursorImage img;
    img.width = shm->width;
    img.height = shm->height;
    img.hot_x = s.hot_x;
    img.hot_y = s.hot_y;
    const auto row = static_cast<std::size_t>(shm->width) * 4;
    struct stat st{};
    if (shm->width <= 0 || shm->height <= 0 || shm->width > 512 || shm->height > 512 || shm->offset < 0 ||
        static_cast<std::size_t>(shm->stride) < row || ::fstat(shm->pool->fd.get(), &st) != 0)
        return give_back();
    const auto need = static_cast<std::size_t>(shm->offset) +
                      static_cast<std::size_t>(shm->stride) * static_cast<std::size_t>(shm->height - 1) + row;
    if (need > static_cast<std::size_t>(st.st_size)) return give_back();
    void* p = ::mmap(nullptr, need, PROT_READ, MAP_SHARED, shm->pool->fd.get(), 0);
    if (p == MAP_FAILED) return give_back();
    img.argb.resize(row * static_cast<std::size_t>(shm->height));
    for (std::int32_t y = 0; y < shm->height; ++y)
        std::memcpy(img.argb.data() + row * static_cast<std::size_t>(y),
                    static_cast<const char*>(p) + shm->offset + static_cast<std::size_t>(shm->stride) * y, row);
    ::munmap(p, need);
    if (xrgb)  // opaque: every alpha byte (little-endian ARGB: byte 3) is 0xff
        for (std::size_t i = 3; i < img.argb.size(); i += 4) img.argb[i] = static_cast<char>(0xff);
    give_back();
    publish_cursor({std::move(img)});
}

void Host::bind_cursor_shape(srv::Client& client, std::uint32_t version, id_t id) {
    using M = wp_cursor_shape_manager_v1;
    using D = wp_cursor_shape_device_v1;
    auto l = std::make_shared<srv::RequestListener<M>>();
    const auto device = [this, &client](auto&& dev) {
        auto dl = std::make_shared<srv::RequestListener<D>>();
        dl->template on<D::requests::set_shape>([this, &client](std::uint32_t, D::shape shape) {
            if (!pointer_focus_ || pointer_focus_->client != &client) return;
            cursor_surface_ = nullptr;
            publish_cursor({link::down::CursorShape{static_cast<std::uint32_t>(shape)}});
        });
        dl->template on<D::requests::destroy>([] {});
        dev.template bind<2>(dl);
    };
    l->on<M::requests::get_pointer>([device](srv::NewObject<D> d, Ref<wl_pointer>) { device(d); });
    l->on<M::requests::get_tablet_tool_v2>([device](srv::NewObject<D> d, Ref<zwp_tablet_tool_v2>) { device(d); });
    l->on<M::requests::destroy>([] {});
    client.bind_resource<M, 2>(id, l, version);
}

// -- the clipboard: wl_data_device_manager --------------------------------------------
//
// Data never passes through i4d: a reader's receive(mime, fd) is forwarded
// to the source as send(mime, fd), and the two clients talk over that pipe.
// Drag and drop is accepted and not yet performed (start_drag is ignored;
// the source is told it was cancelled).

void Host::bind_data_device_manager(srv::Client& client, std::uint32_t version, id_t id) {
    auto l = std::make_shared<srv::RequestListener<wl_data_device_manager>>();

    l->on<wl_data_device_manager::requests::create_data_source>([this, &client](srv::NewObject<wl_data_source> s) {
        const Key key{&client, s.id()};
        sources_[key] = {};
        auto sl = std::make_shared<srv::RequestListener<wl_data_source>>();
        sl->on<wl_data_source::requests::offer>([this, key](std::string_view mime) {
            if (auto it = sources_.find(key); it != sources_.end()) it->second.mime_types.emplace_back(mime);
        });
        sl->on<wl_data_source::requests::set_actions>([](wl_data_device_manager_dnd_action) {});
        sl->on<wl_data_source::requests::destroy>([this, key] {
            sources_.erase(key);
            std::erase_if(offers_, [&](const auto& kv) { return kv.second == key; });
            if (selection_ == key) set_selection(std::nullopt);
        });
        s.bind<3>(sl);
    });

    l->on<wl_data_device_manager::requests::get_data_device>([this, &client](srv::NewObject<wl_data_device> d, Ref<wl_seat>) {
        auto dl = std::make_shared<srv::RequestListener<wl_data_device>>();
        dl->on<wl_data_device::requests::set_selection>([this, &client](Ref<wl_data_source> src, std::uint32_t) {
            // Only a client with keyboard focus may set the selection, and
            // only with a source of its own.
            if (!keyboard_focus_ || keyboard_focus_->client != &client) return;
            if (!src) {
                set_selection(std::nullopt);
                return;
            }
            const Key key{&client, src.id};
            if (sources_.contains(key)) set_selection(key);
        });
        dl->on<wl_data_device::requests::start_drag>(
            [&client](Ref<wl_data_source> src, Ref<wl_surface>, Ref<wl_surface>, std::uint32_t) {
                if (src) srv::Resource<wl_data_source, 3>{&client, src.id}.send<wl_data_source::events::cancelled>();
            });
        dl->on<wl_data_device::requests::release>([this, &client, did = d.id()] {
            std::erase(seats_[&client].data_devices, did);
        });
        d.bind<3>(dl);
        seats_[&client].data_devices.push_back(d.id());
        if (keyboard_focus_ && keyboard_focus_->client == &client) offer_selection(client);
    });

    l->on<wl_data_device_manager::requests::release>([] {});
    client.bind_resource<wl_data_device_manager, 3>(id, l, version);
}

void Host::set_selection(std::optional<Key> source) {
    if (selection_ == source) return;
    if (selection_ && sources_.contains(*selection_))
        srv::Resource<wl_data_source, 3>{selection_->first, selection_->second}.send<wl_data_source::events::cancelled>();
    selection_ = source;
    // Offers of the old selection now read nothing.
    std::erase_if(offers_, [&](const auto& kv) { return kv.second != selection_; });
    if (keyboard_focus_) offer_selection(*keyboard_focus_->client);
    display_.flush_clients();
}

void Host::offer_selection(srv::Client& c) {
    for (auto did : seats_[&c].data_devices) {
        srv::Resource<wl_data_device, 3> dev{&c, did};
        const auto src = selection_ ? sources_.find(*selection_) : sources_.end();
        if (src == sources_.end()) {
            dev.send<wl_data_device::events::selection>(Ref<wl_data_offer>{});
            continue;
        }
        // A fresh offer for this client: introduced, its types listed, then
        // named as the selection.
        const id_t oid = dev.send<wl_data_device::events::data_offer>();
        auto ol = std::make_shared<srv::RequestListener<wl_data_offer>>();
        const Key okey{&c, oid};
        ol->on<wl_data_offer::requests::receive>([this, okey](std::string_view mime, BorrowedFd fd) {
            OwnedFd pipe{fd.get()};  // ours to close once forwarded
            auto it = offers_.find(okey);
            if (it == offers_.end() || !sources_.contains(it->second)) return;
            srv::Resource<wl_data_source, 3>{it->second.first, it->second.second}.send<wl_data_source::events::send>(
                mime, BorrowedFd{pipe.get()});
            display_.flush_clients();
        });
        ol->on<wl_data_offer::requests::accept>([](std::uint32_t, std::optional<std::string_view>) {});
        ol->on<wl_data_offer::requests::finish>([] {});
        ol->on<wl_data_offer::requests::set_actions>(
            [](wl_data_device_manager_dnd_action, wl_data_device_manager_dnd_action) {});
        ol->on<wl_data_offer::requests::destroy>([this, okey] { offers_.erase(okey); });
        auto offer = srv::NewObject<wl_data_offer>{&c, oid, did}.bind<3>(ol);
        offers_[okey] = *selection_;
        for (const auto& mime : src->second.mime_types) offer.send<wl_data_offer::events::offer>(mime);
        dev.send<wl_data_device::events::selection>(Ref<wl_data_offer>{oid});
    }
}

}  // namespace i4::server
