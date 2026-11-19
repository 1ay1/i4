// role.hpp — client and server as the two orientations of one algebra.
//
// A Wayland interface is a pair of message sets. Which one you SEND and which
// one you RECEIVE is the only difference between the two roles:
//
//            sends        receives
//   client   requests     events
//   server   events       requests
//
// So "the server side" is not a second implementation, it is the same record
// read with the projections swapped. That is what makes the role a *parameter*
// here rather than a fork of the codebase: `Role` picks which of an
// interface's two message sets is outbound, and every generic operation
// (send, dispatch, version checks) is written against the projection instead
// of against `requests`/`events` directly.
//
// This header is the proof that the algebra was factored correctly: if the
// data model were wrong, the dual would not drop out this cheaply.
#pragma once

#include "algebra.hpp"
#include "wire.hpp"

namespace i4::wl {

// ---------------------------------------------------------------------------
// The two roles, and the projections that distinguish them.
// ---------------------------------------------------------------------------
struct ClientRole {
    /// What this role puts on the wire.
    template <Interface I>
    using outbound = typename I::requests;
    /// What this role takes off the wire.
    template <Interface I>
    using inbound = typename I::events;

    static constexpr std::string_view name = "client";

    /// Client-allocated ids live below the server range.
    static constexpr wire::id_t id_base = 2;
    static constexpr wire::id_t id_max = 0xfeffffffu;
};

struct ServerRole {
    template <Interface I>
    using outbound = typename I::events;
    template <Interface I>
    using inbound = typename I::requests;

    static constexpr std::string_view name = "server";

    /// Server-allocated ids start at 0xff000000, per the protocol.
    static constexpr wire::id_t id_base = 0xff000000u;
    static constexpr wire::id_t id_max = 0xffffffffu;
};

template <class R>
concept Role = requires {
    { R::name } -> std::convertible_to<std::string_view>;
    { R::id_base } -> std::convertible_to<wire::id_t>;
};

/// The dual of a role. `dual_t<Client>` is `Server`, and vice versa; the
/// involution `dual_t<dual_t<R>> == R` is asserted below.
template <class R>
struct dual;
template <>
struct dual<ClientRole> {
    using type = ServerRole;
};
template <>
struct dual<ServerRole> {
    using type = ClientRole;
};
template <class R>
using dual_t = typename dual<R>::type;

static_assert(std::is_same_v<dual_t<dual_t<ClientRole>>, ClientRole>);
static_assert(std::is_same_v<dual_t<dual_t<ServerRole>>, ServerRole>);

// NOTE: the roles are deliberately named ClientRole / ServerRole rather than
// Client / Server. `i4::wl::server::Client` is a real runtime type (a connected
// application), and a role tag called `Client` sitting in the same reachable
// scope makes every mention of either ambiguous. The suffix is two characters
// and removes a whole class of confusing errors.

// ---------------------------------------------------------------------------
// Role-parametric projections. Generic code uses these, never `I::requests`.
// ---------------------------------------------------------------------------
template <class R, Interface I>
using outbound_t = typename R::template outbound<I>;

template <class R, Interface I>
using inbound_t = typename R::template inbound<I>;

template <class R, Interface I>
using outbound_messages_t = typename outbound_t<R, I>::all;

template <class R, Interface I>
using inbound_messages_t = typename inbound_t<R, I>::all;

/// Can role R send message M on interface I? This is a type-level question:
/// a client sending an event is a category error, not a runtime mistake.
template <class R, Interface I, class M>
inline constexpr bool sends = contains_v<outbound_messages_t<R, I>, M>;

template <class R, Interface I, class M>
inline constexpr bool receives = contains_v<inbound_messages_t<R, I>, M>;

}  // namespace i4::wl
