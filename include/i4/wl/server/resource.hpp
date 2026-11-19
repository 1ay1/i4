// resource.hpp — a server-side object: the dual of Proxy.
//
// A Proxy sends requests and receives events. A Resource sends events and
// receives requests. Same interface record, same version gating, same
// signature folds — the only thing that changes is which of the interface's
// two message sets is outbound, which wl/role.hpp already made a parameter.
//
//   Proxy<wl_surface, 4>    client side: send<...requests::commit>()
//   Resource<wl_surface, 4> server side: send<...events::enter>(output)
//
// So the compile-time guarantees carry over for free: an event newer than the
// resource's version is a compile error, a wl_output where a wl_surface
// belongs is a compile error, and a *request* sent from the server does not
// typecheck at all.
#pragma once

#include "i4/wl/listener.hpp"
#include "i4/wl/role.hpp"

#include <string_view>
#include <vector>

namespace i4::wl::server {

class Client;
class Display;

// ---------------------------------------------------------------------------
// Resource<I, V> — one object, as the server sees it.
// ---------------------------------------------------------------------------
template <Interface I, std::uint32_t V = I::wl_version>
class Resource {
    static_assert(V >= 1 && V <= I::wl_version,
                  "i4::wl: version out of range for this interface.");

public:
    using interface = I;
    static constexpr std::uint32_t version = V;
    static constexpr std::string_view name = I::wl_name;

    constexpr Resource() = default;
    constexpr Resource(Client* c, wire::id_t id) : client_(c), id_(id) {}

    constexpr wire::id_t id() const { return id_; }
    constexpr Client* client() const { return client_; }
    constexpr explicit operator bool() const { return id_ != 0 && client_ != nullptr; }
    constexpr Ref<I> ref() const { return Ref<I>{id_}; }

    /// Does this object, at its bound version, support event M?
    template <class M>
    static constexpr bool supports = M::since <= V;

    /// Send event M to the client.
    ///
    /// Only EVENTS are sendable from here: `sends<ServerRole, I, M>` is false for a
    /// request, so trying to send one is a category error the compiler
    /// rejects, not a protocol violation discovered at runtime.
    ///
    /// An event that creates an object (wl_data_device.data_offer) returns
    /// the id the server allocated for it, so the compositor can bind a
    /// listener to it; any other event returns void.
    template <class M, class... Args>
    auto send(Args&&... args) const;

    /// Send a protocol error and mark the client for disconnection. This is
    /// the server's half of wl_display.error.
    template <class E>
    void post_error(E code, std::string_view message) const;

private:
    Client* client_ = nullptr;
    wire::id_t id_ = 0;
};

template <Interface I> class RequestListener;

/// Where a request came from: which client, and which object it was sent to.
struct Origin {
    Client* client = nullptr;
    wire::id_t object = 0;
};

// ---------------------------------------------------------------------------
// NewObject<I> — an object a client just asked the compositor to create.
//
// A request with a typed new_id (wl_compositor.create_surface,
// xdg_surface.get_toplevel, ...) creates an object whose version is, by the
// protocol's rule, its creator's. NewObject carries all three facts (client,
// id, creator), so binding it can't forget the version or name the wrong
// client: `obj.bind<V>(listener)` is the only way in.
// ---------------------------------------------------------------------------
template <Interface I>
class NewObject {
public:
    NewObject(Client* client, wire::id_t id, wire::id_t creator)
        : client_(client), id_(id), creator_(creator) {}

    wire::id_t id() const { return id_; }
    Client& client() const { return *client_; }
    /// As a plain reference, for the rare handler that only needs the id.
    Ref<I> ref() const { return Ref<I>{id_}; }

    /// Give it its request handlers. V is the newest version this code
    /// handles; the object speaks min(V, its creator's version).
    template <std::uint32_t V = I::wl_version>
    Resource<I, V> bind(std::shared_ptr<RequestListener<I>> listener) const;

private:
    Client* client_;
    wire::id_t id_;
    wire::id_t creator_;
};

// ---------------------------------------------------------------------------
// The handler a resource installs for incoming REQUESTS.
//
// Mirror image of Listener<I>: same table, but keyed on the interface's
// requests rather than its events.
// ---------------------------------------------------------------------------
namespace detail {

template <class Requests>
struct RequestTable;

template <class... R>
struct RequestTable<List<R...>> {
    static constexpr std::size_t count = sizeof...(R);
    std::array<std::function<void(wire::Decoder&, Origin)>, count> slots{};
    static constexpr std::array<std::size_t, count> fd_counts{
        fd_count_v<typename R::sig>...};
    /// Which opcodes end the object (the XML's type="destructor").
    static constexpr std::array<bool, count> destructors{R::destructor...};

    void invoke(std::uint32_t opcode, wire::Decoder& d, Origin o) const {
        if (opcode >= count) return;
        if (const auto& f = slots[opcode]) f(d, o);
    }

    bool handles(std::uint32_t opcode) const { return opcode < count && slots[opcode]; }
};

template <>
struct RequestTable<List<>> {
    static constexpr std::size_t count = 0;
    std::array<std::function<void(wire::Decoder&, Origin)>, 0> slots{};
    static constexpr std::array<std::size_t, 0> fd_counts{};
    static constexpr std::array<bool, 0> destructors{};
    void invoke(std::uint32_t, wire::Decoder&, Origin) const {}
    bool handles(std::uint32_t) const { return false; }
};

// What a server handler receives for each argument: the client-side decode,
// except a typed new_id, which becomes a NewObject that knows its creator.
template <class A>
struct server_value_of {
    using type = decode_value_t<A>;
};
template <class I>
struct server_value_of<NewId<I>> {
    using type = NewObject<I>;
};
template <class S> struct server_args;
template <class... A>
struct server_args<Sig<A...>> {
    using type = std::tuple<typename server_value_of<A>::type...>;
};

template <class A>
typename server_value_of<A>::type decode_server_one(wire::Decoder& d, Origin o) {
    if constexpr (is_new_id_v<A>) {
        return NewObject<typename A::iface>{o.client, d.get_u32(), o.object};
    } else {
        return i4::wl::detail::decode_one<A>(d);
    }
}

template <class... A>
typename server_args<Sig<A...>>::type decode_server_fold(wire::Decoder& d,
                                                        [[maybe_unused]] Origin o, Sig<A...>) {
    // Braced init: left to right, as the wire requires.
    return typename server_args<Sig<A...>>::type{decode_server_one<A>(d, o)...};
}

template <class S>
using server_args_t = typename server_args<S>::type;

/// The names of I's requests, by opcode, for reports.
template <class L> struct request_names;
template <class... R>
struct request_names<List<R...>> {
    static constexpr std::array<std::string_view, sizeof...(R)> value{R::wl_name...};
};

/// Called when a request arrives that the compositor installed no handler
/// for: (interface, request). Set by whoever wants to hear it (a log, a
/// test that fails). Unset, it's silent.
inline std::function<void(std::string_view, std::string_view)>& unhandled_request_hook() {
    static std::function<void(std::string_view, std::string_view)> h;
    return h;
}

/// Whether request `opcode` of I is a destructor.
template <class I>
constexpr bool is_destructor_opcode(std::uint32_t opcode) {
    using T = RequestTable<typename I::requests::all>;
    return opcode < T::count && T::destructors[opcode];
}

}  // namespace detail

/// RequestListener<I> — what a compositor implements for an interface.
///
/// A request that creates an object (a typed new_id) hands its handler a
/// NewObject<J>, not a bare id. It knows the client, the id and the version
/// the protocol gives it (the creating object's), so binding it is
/// `obj.bind(listener)`: there's nothing to pass, and nothing to get wrong.
template <Interface I>
class RequestListener {
public:
    using requests = typename I::requests::all;

    /// Install a handler for request M. The callable must accept exactly M's
    /// decoded arguments — checked against the protocol, same as the client
    /// side's events — except that a new_id argument arrives as NewObject.
    template <class M, class F>
    RequestListener& on(F&& f) {
        static_assert(std::is_same_v<typename M::iface, I>,
                      "i4::wl: that request belongs to a different interface.");
        static_assert(contains_v<requests, M>,
                      "i4::wl: a server receives requests, not events.");
        check_invocable<M, F>(static_cast<detail::server_args_t<typename M::sig>*>(nullptr));
        table_.slots[M::opcode] = make_thunk<M>(std::forward<F>(f));
        return *this;
    }

    void dispatch(Client& client, wire::id_t self, std::uint32_t opcode,
                  wire::Decoder& d) const {
        if (!table_.handles(opcode)) {
            // A destructor with no handler is fine: bind_resource retires
            // the object, which is all it asks for. Anything else is a
            // request the compositor silently ignores: report it.
            if (!detail::is_destructor_opcode<I>(opcode))
                if (auto& h = detail::unhandled_request_hook())
                    if (opcode < detail::request_names<requests>::value.size())
                        h(I::wl_name, detail::request_names<requests>::value[opcode]);
            return;
        }
        table_.invoke(opcode, d, Origin{&client, self});
    }

    /// The requests of I this listener has no handler for, destructors
    /// excepted. Empty means every request the protocol defines is handled.
    std::vector<std::string_view> unhandled() const {
        std::vector<std::string_view> out;
        constexpr auto& names = detail::request_names<requests>::value;
        for (std::uint32_t op = 0; op < names.size(); ++op)
            if (!table_.handles(op) && !detail::is_destructor_opcode<I>(op))
                out.push_back(names[op]);
        return out;
    }

    static std::span<const std::size_t> fd_counts() {
        return {detail::RequestTable<requests>::fd_counts.data(),
                detail::RequestTable<requests>::fd_counts.size()};
    }

private:
    template <class M, class F>
    static std::function<void(wire::Decoder&, Origin)> make_thunk(F&& f) {
        return [fn = std::forward<F>(f)](wire::Decoder& d, Origin o) mutable {
            auto args = detail::decode_server_fold(d, o, typename M::sig{});
            if (d.bad) return;
            std::apply(fn, std::move(args));
        };
    }

    template <class M, class F, class... A>
    static void check_invocable(std::tuple<A...>*) {
        static_assert(std::is_invocable_v<F&, A...>,
                      "i4::wl: this handler's parameters do not match the "
                      "request's arguments.");
    }

    detail::RequestTable<requests> table_;
};

}  // namespace i4::wl::server
