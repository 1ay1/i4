// listener.hpp — typed event dispatch.
//
// You hand `on()` a lambda per event; the library builds the coproduct
// eliminator. The lambda's parameters are the event's decoded arguments, so
// their types are checked against the protocol at compile time:
//
//   registry.on<global>([](uint32_t name, string_view iface, uint32_t ver) {...});
//                        ^^^^^^^^^^^^^^^^ wrong type here = compile error
//
// The dispatch table is one switch over a dense opcode, built by a fold. There
// is no virtual call and no per-event generated code anywhere in this file.
#pragma once

#include "connection.hpp"

namespace i4::wl {

namespace detail {

// A handler set: one std::function slot per event of the interface, indexed by
// opcode. Dense opcodes (a law checked by the generator) make this an array.
template <class Events>
struct HandlerTable;

template <class... E>
struct HandlerTable<List<E...>> {
    static constexpr std::size_t count = sizeof...(E);
    std::array<std::function<void(wire::Decoder&)>, count> slots{};

    /// How many descriptors the message at `opcode` carries. Read off each
    /// event's signature at compile time, so the connection can account for
    /// received fds without knowing anything about the protocol.
    static constexpr std::array<std::size_t, count> fd_counts{fd_count_v<typename E::sig>...};

    void invoke(std::uint32_t opcode, wire::Decoder& d) const {
        if (opcode >= count) return;  // unknown event: forward compatible
        const auto& f = slots[opcode];
        if (f) f(d);
    }
};

// An interface with no events at all.
template <>
struct HandlerTable<List<>> {
    static constexpr std::size_t count = 0;
    std::array<std::function<void(wire::Decoder&)>, 0> slots{};
    static constexpr std::array<std::size_t, 0> fd_counts{};
    void invoke(std::uint32_t, wire::Decoder&) const {}
};

// Wrap a user callable as a decoder-driven thunk: decode the whole signature
// into a tuple, then apply. `std::apply` is where the type check bites.
template <class M, class F>
std::function<void(wire::Decoder&)> make_thunk(F&& f) {
    return [fn = std::forward<F>(f)](wire::Decoder& d) mutable {
        auto args = decode_fold(d, typename M::sig{});
        if (d.bad) return;  // malformed event: drop, never invoke with garbage
        std::apply(fn, std::move(args));
    };
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Listener<I> — owns the handler table for one object and keeps it alive.
// ---------------------------------------------------------------------------
template <Interface I>
class Listener {
public:
    using events = typename I::events::all;

    /// Install a handler for event M. The callable must accept exactly M's
    /// decoded arguments.
    template <class M, class F>
    Listener& on(F&& f) {
        static_assert(std::is_same_v<typename M::iface, I>,
                      "i4::wl: that event belongs to a different interface.");
        check_invocable<M, F>(static_cast<decoded_t<typename M::sig>*>(nullptr));
        table_.slots[M::opcode] = detail::make_thunk<M>(std::forward<F>(f));
        return *this;
    }

    void dispatch(const RawEvent& ev) const {
        wire::Decoder d{ev.body, ev.fds, 0, 0, false};
        table_.invoke(ev.opcode, d);
    }

    /// Per-opcode descriptor counts, read off the event signatures. The
    /// connection's reader thread needs these to route fds without running a
    /// handler, so they are exposed as a static span into constexpr storage.
    static std::span<const std::size_t> fd_counts() {
        return std::span<const std::size_t>(detail::HandlerTable<events>::fd_counts.data(),
                                            detail::HandlerTable<events>::fd_counts.size());
    }

private:
    // Produce a readable error when the lambda's signature does not match.
    template <class M, class F, class... A>
    static void check_invocable(std::tuple<A...>*) {
        static_assert(std::is_invocable_v<F&, A...>,
                      "i4::wl: this handler's parameters do not match the event's "
                      "arguments. Check the generated signature for M::sig.");
    }

    detail::HandlerTable<events> table_;
};

// ---------------------------------------------------------------------------
// Owned<I> — a proxy plus its listener, with the handler kept alive for as long
// as the object lives. This is the type most user code holds.
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V = I::wl_version>
class Owned {
public:
    using proxy_type = Proxy<I, V>;

    Owned() = default;
    explicit Owned(proxy_type p) : proxy_(p), listener_(std::make_shared<Listener<I>>()) {
        if (proxy_) {
            auto l = listener_;
            proxy_.connection()->set_handler(
                proxy_.id(), [l](const RawEvent& ev) { l->dispatch(ev); },
                Listener<I>::fd_counts());
        }
    }

    Owned(Owned&&) noexcept = default;
    Owned& operator=(Owned&&) noexcept = default;
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    template <class M, class F>
    Owned& on(F&& f) {
        listener_->template on<M>(std::forward<F>(f));
        return *this;
    }

    template <class M, class... Args>
    decltype(auto) send(Args&&... args) const {
        return proxy_.template send<M>(std::forward<Args>(args)...);
    }

    const proxy_type& proxy() const { return proxy_; }
    wire::id_t id() const { return proxy_.id(); }
    Ref<I> ref() const { return proxy_.ref(); }
    explicit operator bool() const { return static_cast<bool>(proxy_); }
    static constexpr std::uint32_t version = V;

private:
    proxy_type proxy_{};
    std::shared_ptr<Listener<I>> listener_{};
};

/// Attach a listener to a freshly created proxy: `auto s = own(c.send<...>())`.
template <class I, std::uint32_t V>
Owned<I, V> own(Proxy<I, V> p) {
    return Owned<I, V>{p};
}

// ---------------------------------------------------------------------------
// send_bind — the one request whose result type the *caller* chooses.
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V>
template <class Target, std::uint32_t TV, class M, class... Args>
Proxy<Target, TV> Proxy<I, V>::send_bind(Args&&... args) const {
    detail::check_owner<M, I>();
    detail::check_version<M, V>();
    static_assert(TV <= Target::wl_version,
                  "i4::wl: cannot bind a global above the version the protocol "
                  "definition declares.");

    wire::id_t id = 0;
    auto tup = std::forward_as_tuple(std::forward<Args>(args)...);
    {
        auto msg = conn_->message(id_, M::opcode);
        id = msg.allocate_id();
        detail::encode_fold<0>(msg.encoder(), typename M::sig{}, tup, id);
    }
    return Proxy<Target, TV>{conn_, id};
}

}  // namespace i4::wl
