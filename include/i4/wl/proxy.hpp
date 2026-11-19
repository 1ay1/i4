// proxy.hpp — typed object handles.
//
// Proxy<I, V> is an object of interface I, live at version V. V is a *template
// parameter*, so `since > V` is a compile error rather than a protocol error
// delivered to your error handler three frames later.
//
//   auto surface = compositor.create_surface();   // Proxy<wl_surface, 6>
//   surface.send<offset>(1, 2);                   // ok   (since 5 <= 6)
//   Proxy<wl_surface,4>{...}.send<offset>(1, 2);  // COMPILE ERROR
//
// send() is one function. It reads the signature off the message type, folds
// the encoder over it, and returns the created object's proxy when the
// signature has a new_id. No generated code, no virtuals, no type erasure.
#pragma once

#include <cassert>
#include <memory>

#include "algebra.hpp"
#include "queue.hpp"
#include "wire.hpp"

namespace i4::wl {

class Connection;

// ---------------------------------------------------------------------------
// Ref<I>: a typed object id that MAY be null (id 0).
//
// Wayland's `allow-null="true"` is the exception, not the rule. So the default
// lowering of an Object<I> argument is `NonNull<I>`, which has no default
// constructor and cannot be built from a null Ref without saying so. A
// nullable argument lowers to plain `Ref<I>`, where 0 is meaningful.
// ---------------------------------------------------------------------------
template <class I>
struct Ref {
    using interface_type = I;
    wire::id_t id = 0;
    constexpr explicit operator bool() const { return id != 0; }
    friend constexpr bool operator==(Ref, Ref) = default;
};

template <class T> struct is_ref : std::false_type {};
template <class I> struct is_ref<Ref<I>> : std::true_type {};
template <class T> inline constexpr bool is_ref_v = is_ref<T>::value;

/// A reference the protocol guarantees is present. There is no way to make one
/// out of thin air: it comes from a live object (implicitly), or from an
/// explicit, greppable `NonNull<I>::unchecked(ref)`.
template <class I>
class NonNull {
public:
    using interface_type = I;

    NonNull() = delete;  // the entire point

    /// From a possibly-null Ref, only when the caller has checked.
    static constexpr NonNull unchecked(Ref<I> r) { return NonNull(r.id); }

    /// The checked path: nullopt when the ref is null.
    static constexpr std::optional<NonNull> from(Ref<I> r) {
        if (!r) return std::nullopt;
        return NonNull(r.id);
    }

    constexpr wire::id_t id() const { return id_; }
    constexpr Ref<I> ref() const { return Ref<I>{id_}; }

    friend constexpr bool operator==(NonNull, NonNull) = default;

    // Proxy converts into this implicitly; see the deduction below.
    constexpr explicit NonNull(wire::id_t id) : id_(id) {}

private:
    wire::id_t id_;
};

template <class T> struct is_nonnull : std::false_type {};
template <class I> struct is_nonnull<NonNull<I>> : std::true_type {};
template <class T> inline constexpr bool is_nonnull_v = is_nonnull<T>::value;

// ---------------------------------------------------------------------------
// Compile-time diagnostics. These produce the error message the user reads, so
// they are worth more than their size.
// ---------------------------------------------------------------------------
namespace detail {
template <class M, std::uint32_t V>
constexpr void check_version() {
    static_assert(M::since <= V,
                  "i4::wl: this request is newer than the version this object is "
                  "bound at. Bind the global at a higher version, or guard the "
                  "call with `if constexpr (version >= M::since)`.");
}

template <class M, class I>
constexpr void check_owner() {
    static_assert(std::is_same_v<typename M::iface, I>,
                  "i4::wl: this message belongs to a different interface.");
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Proxy
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V = I::wl_version>
class Proxy {
    static_assert(V >= 1 && V <= I::wl_version,
                  "i4::wl: version out of range for this interface.");

public:
    using interface = I;
    static constexpr std::uint32_t version = V;
    static constexpr std::string_view name = I::wl_name;

    constexpr Proxy() = default;
    constexpr Proxy(Connection* c, wire::id_t id) : conn_(c), id_(id) {}

    constexpr wire::id_t id() const { return id_; }
    constexpr Connection* connection() const { return conn_; }
    constexpr explicit operator bool() const { return id_ != 0 && conn_ != nullptr; }
    constexpr Ref<I> ref() const { return Ref<I>{id_}; }

    /// A live proxy is by construction a non-null reference, so it converts
    /// implicitly wherever the protocol demands one.
    constexpr operator NonNull<I>() const { return NonNull<I>{id_}; }  // NOLINT(*-explicit)

    /// Does this object, at its bound version, support message M?
    template <class M>
    static constexpr bool supports = M::since <= V;

    /// Send request M. Returns Proxy of the created object when M creates one,
    /// void otherwise. Defined in connection.hpp (needs Connection complete).
    template <class M, class... Args>
    decltype(auto) send(Args&&... args) const;

    /// Bind a dynamic new_id (wl_registry.bind) to a concrete interface.
    template <class Target, std::uint32_t TV = Target::wl_version, class M, class... Args>
    Proxy<Target, TV> send_bind(Args&&... args) const;

    /// Reinterpret at a lower version. Always safe: fewer messages allowed.
    template <std::uint32_t NV>
    constexpr Proxy<I, NV> downgrade() const {
        static_assert(NV <= V, "i4::wl: downgrade() cannot raise a version.");
        return Proxy<I, NV>{conn_, id_};
    }

    /// A view of this object that creates its children on `queue`.
    ///
    /// The equivalent of wl_proxy_create_wrapper: the wrapper is the SAME
    /// protocol object (same id, no extra wire traffic), but objects created
    /// through it are routed to `queue` instead of the parent's. That lets a
    /// worker thread issue a request and receive the reply itself, without
    /// racing whichever thread owns the parent.
    ///
    /// Unlike libwayland's version there is nothing to destroy: the wrapper is
    /// a value, and wl_proxy_wrapper_destroy has no counterpart to forget.
    Proxy wrapped_on(std::shared_ptr<EventQueue> queue) const {
        Proxy w = *this;
        w.child_queue_ = std::move(queue);
        return w;
    }

    /// The queue this proxy's children are created on, if overridden.
    const std::shared_ptr<EventQueue>& child_queue() const { return child_queue_; }

private:
    Connection* conn_ = nullptr;
    wire::id_t id_ = 0;
    std::shared_ptr<EventQueue> child_queue_;  // set by wrapped_on()
};

// ---------------------------------------------------------------------------
// The return type of send<M>(): Proxy of the new_id's interface, or void.
// The child inherits min(parent version, child maximum), exactly as the
// protocol specifies — computed by the compiler, not by you.
// ---------------------------------------------------------------------------
namespace detail {
template <class A, std::uint32_t V>
struct result_of_new_id;
template <class I, std::uint32_t V>
struct result_of_new_id<NewId<I>, V> {
    using type = Proxy<I, inherited_version<I, V>>;
};
template <std::uint32_t V>
struct result_of_new_id<void, V> {
    using type = void;
};
}  // namespace detail

template <class M, std::uint32_t V>
using send_result_t = typename detail::result_of_new_id<new_id_arg_t<typename M::sig>, V>::type;

}  // namespace i4::wl
