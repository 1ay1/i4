#pragma once
// i4/link/protocol.hpp — what the server and a head say to each other.
//
// The server streams EVENTS, not state. Every event it applies to its State
// it also sends, numbered, to each attached head; the head applies the same
// event to its replica with the same apply(), and arrives at the same State
// (wm::apply is deterministic, and every wm number is an integer — see
// wm/state.hpp). An event is a few dozen bytes; the State it changes, and the
// Arrangement drawn from it, are derived on the head for free.
//
// Sequence of a session:
//
//   head   -> Hello{protocol fingerprint}
//   server -> Welcome{state, seq}          the whole replica, once
//   server -> Applied{seq+1, event}        then every event, in order
//   head   -> Request{nonce, event}        a keypress, as a wm::Event
//   server -> Applied{..., by nonce}       ...comes back applied, in order
//
// A head that predicted Request #n (applied it locally ahead of time) learns
// the authoritative order when Applied{by = n} arrives — see predict.hpp.
//
// Both directions are plain variants of plain structs, so the codec, its
// totality, and the fingerprint that refuses mismatched builds all come from
// wire.hpp with no code here.

#include <cstdint>
#include <optional>
#include <variant>

#include "i4/base/id.hpp"
#include "i4/link/wire.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/state.hpp"

namespace i4::link {

/// Position in the server's event log. 64 bits: it only grows.
using Seq = Id<struct SeqTag, std::uint64_t>;
/// A head's name for one of its requests, unique per connection.
using Nonce = Id<struct NonceTag, std::uint64_t>;

// -- head -> server ------------------------------------------------------------

namespace up {
struct Hello {
    std::uint64_t fingerprint;
    friend bool operator==(const Hello&, const Hello&) = default;
};
/// Run this on the authoritative State.
struct Request {
    Nonce nonce;
    wm::Event event;
    friend bool operator==(const Request&, const Request&) = default;
};
}  // namespace up

using Up = std::variant<up::Hello, up::Request>;

// -- server -> head ------------------------------------------------------------

namespace down {
struct Welcome {
    Seq seq;
    wm::State state;
    friend bool operator==(const Welcome&, const Welcome&) = default;
};
/// The fingerprints differ: this head was built against another protocol.
struct Refused {
    std::uint64_t server_fingerprint;
    friend bool operator==(const Refused&, const Refused&) = default;
};
struct Applied {
    Seq seq;
    wm::Event event;
    /// Set when this event is the head's own Request, coming back.
    std::optional<Nonce> by;
    friend bool operator==(const Applied&, const Applied&) = default;
};
}  // namespace down

using Down = std::variant<down::Welcome, down::Refused, down::Applied>;

// -- the protocol's identity --------------------------------------------------------
//
// Both message sets, their every field down to the last enum: change any of
// it and old and new builds refuse each other at Hello rather than misread.

inline constexpr std::uint64_t protocol = detail::mix(fingerprint<Up>, fingerprint<Down>);

static_assert(Wire<Up> && Wire<Down>);
static_assert(Wire<wm::State>, "the replica must cross the wire whole");

}  // namespace i4::link
