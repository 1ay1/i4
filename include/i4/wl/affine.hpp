// affine.hpp — object lifetime in the type system.
//
// A Wayland object is a RESOURCE, not a value: the protocol marks some requests
// `type="destructor"`, and after one of those the id is dead. Sending to a dead
// object is a protocol error the compositor punishes, and the ordinary C++ way
// to express "you may use this at most once" is an affine type.
//
// `Unique<I, V>` is a move-only handle whose destructor request CONSUMES it:
//
//   auto s = make_unique_object(compositor.send<...create_surface>());
//   s.commit();                 // ok
//   std::move(s).destroy();     // consumes: s is now moved-from
//   s.commit();                 // runtime-checked null; see below
//
// C++ cannot reject a use-after-move at compile time (the object still exists,
// just in a valid-but-unspecified state), so this is affine-by-convention with
// a hard runtime trap, not linear types. What IS compile-time is that
// `destroy()` requires an rvalue: you cannot destroy a handle you only borrow,
// and you cannot call it twice on the same lvalue without an explicit
// std::move each time. That kills the common bugs; the residue is documented
// rather than pretended away.
#pragma once

#include "listener.hpp"

namespace i4::wl {

// ---------------------------------------------------------------------------
// Which of an interface's requests are destructors? The generator records it,
// so this is a lookup, not a list we maintain.
// ---------------------------------------------------------------------------
template <class M>
concept DestructorMessage = Message<M> && M::destructor;

namespace detail {
template <class L>
struct find_destructor {
    using type = void;
};
template <class H, class... T>
struct find_destructor<List<H, T...>> {
    using type = std::conditional_t<H::destructor, H,
                                    typename find_destructor<List<T...>>::type>;
};
template <>
struct find_destructor<List<>> {
    using type = void;
};
}  // namespace detail

/// The destructor request of interface I, or void when it has none.
template <Interface I>
using destructor_of = typename detail::find_destructor<typename I::requests::all>::type;

template <Interface I>
inline constexpr bool has_destructor = !std::is_void_v<destructor_of<I>>;

// ---------------------------------------------------------------------------
// Unique — an owning, move-only object handle.
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V = I::wl_version>
class Unique {
public:
    using interface = I;
    using proxy_type = Proxy<I, V>;
    static constexpr std::uint32_t version = V;

    Unique() = default;

    explicit Unique(proxy_type p)
        : proxy_(p), listener_(std::make_shared<Listener<I>>()) {
        if (proxy_) {
            auto l = listener_;
            proxy_.connection()->set_handler(
                proxy_.id(), [l](const RawEvent& ev) { l->dispatch(ev); },
                Listener<I>::fd_counts());
        }
    }

    Unique(Unique&& o) noexcept
        : proxy_(std::exchange(o.proxy_, proxy_type{})),
          listener_(std::move(o.listener_)) {}

    Unique& operator=(Unique&& o) noexcept {
        if (this != &o) {
            release();
            proxy_ = std::exchange(o.proxy_, proxy_type{});
            listener_ = std::move(o.listener_);
        }
        return *this;
    }

    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;

    /// Destroying the handle sends the protocol destructor, so an object is
    /// never leaked by an early return or a thrown exception.
    ~Unique() { release(); }

    // -- requests -----------------------------------------------------------

    /// Send a non-destructor request. Requires a live handle.
    template <class M, class... Args>
        requires(!DestructorMessage<M>)
    decltype(auto) send(Args&&... args) const {
        check_live();
        return proxy_.template send<M>(std::forward<Args>(args)...);
    }

    /// Send the destructor. `&&` means this only binds to an rvalue: you must
    /// write `std::move(obj).destroy()`, and the handle is empty afterwards.
    template <class M = destructor_of<I>>
        requires DestructorMessage<M>
    void destroy() && {
        check_live();
        proxy_.template send<M>();
        proxy_.connection()->retire_id(proxy_.id());
        forget();
    }

    // -- events -------------------------------------------------------------
    template <class M, class F>
    Unique& on(F&& f) {
        listener_->template on<M>(std::forward<F>(f));
        return *this;
    }

    // -- accessors ----------------------------------------------------------
    const proxy_type& proxy() const { return proxy_; }
    wire::id_t id() const { return proxy_.id(); }
    Ref<I> ref() const { return proxy_.ref(); }
    bool live() const { return static_cast<bool>(proxy_); }
    explicit operator bool() const { return live(); }

    /// Give up ownership without sending the destructor. For the rare object
    /// the compositor destroys on its own (wl_callback fires once and dies).
    proxy_type release_ownership() {
        auto p = proxy_;
        forget();
        return p;
    }

private:
    void forget() {
        proxy_ = proxy_type{};
        listener_.reset();
    }

    void release() {
        if (!proxy_) return;
        if constexpr (has_destructor<I>) {
            proxy_.template send<destructor_of<I>>();
        }
        // The id is NOT reusable yet: Wayland requires waiting for the
        // server's wl_display.delete_id, because events already in flight may
        // still name it. Connection recycles it when that arrives.
        proxy_.connection()->retire_id(proxy_.id());
        forget();
    }

    // A moved-from handle is empty. Using one is a bug in the caller, and a
    // loud abort beats a silent message to id 0 that kills the connection with
    // a confusing compositor-side error.
    void check_live() const {
        if (!proxy_) {
            std::fprintf(stderr,
                         "i4::wl: request sent through a moved-from or destroyed "
                         "%.*s handle\n",
                         static_cast<int>(I::wl_name.size()), I::wl_name.data());
            std::abort();
        }
    }

    proxy_type proxy_{};
    std::shared_ptr<Listener<I>> listener_{};
};

/// Wrap a freshly created proxy in an owning handle.
template <class I, std::uint32_t V>
Unique<I, V> adopt(Proxy<I, V> p) {
    return Unique<I, V>{p};
}

}  // namespace i4::wl
