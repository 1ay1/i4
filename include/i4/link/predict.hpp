#pragma once
// i4/link/predict.hpp — the head's replica, and acting before the server
// answers.
//
// Two states, kept apart:
//
//   confirmed   exactly the server's State at the last Applied seq. Only
//               server messages change it, so it can never drift.
//   shown       confirmed, plus this head's requests the server has not
//               applied yet, in the order they were made. This is what the
//               head draws.
//
// A keypress is applied to `shown` at once (in the same frame) and sent as a
// Request. When the server's events arrive they go into `confirmed`, and
// `shown` is rebuilt as confirmed + whatever is still pending. When the
// server applied our request with nothing in between, the rebuild gives the
// same State the prediction did and nothing on screen moves. When another
// event got in first (a window opened, another head typed), the rebuild is
// the correct state: the prediction is corrected, never trusted.
//
// That rebuild is why prediction needs determinism, and why wm::State holds
// no floating point: the head's apply() must agree with the server's bit for
// bit, or every confirmation would be a correction.
//
// Pure: no sockets, no clocks. A transport feeds it Down messages and sends
// what it returns.

#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "i4/link/protocol.hpp"
#include "i4/wm/apply.hpp"
#include "i4/wm/state.hpp"

namespace i4::link {

class Replica {
public:
    /// What to send first.
    [[nodiscard]] static Up hello() { return up::Hello{protocol}; }

    /// Act locally now; send the returned Request to the server. Effects of
    /// a predicted event (spawn, close) are the SERVER's to carry out, once,
    /// so they are dropped here.
    [[nodiscard]] up::Request act(wm::Event e) {
        const Nonce n = next_;
        next_ = next_.next();
        if (ready()) (void)wm::apply(shown_, e);
        pending_.push_back({n, std::move(e)});
        return {n, pending_.back().event};
    }

    enum class Error : std::uint8_t { refused, out_of_order, before_welcome };

    /// One message from the server.
    std::expected<void, Error> receive(const Down& d);

    [[nodiscard]] bool ready() const { return seq_.has_value(); }
    [[nodiscard]] const wm::State& shown() const { return shown_; }
    [[nodiscard]] const wm::State& confirmed() const { return confirmed_; }
    [[nodiscard]] std::size_t pending() const { return pending_.size(); }
    [[nodiscard]] std::optional<Seq> seq() const { return seq_; }

    /// Predictions that turned out wrong: the rebuilt state differed from
    /// what had been shown. For tests and for a latency HUD.
    [[nodiscard]] std::uint64_t corrections() const { return corrections_; }

private:
    struct Pending {
        Nonce nonce;
        wm::Event event;
    };

    void rebuild();

    wm::State confirmed_;
    wm::State shown_;
    std::deque<Pending> pending_;
    std::optional<Seq> seq_;
    Nonce next_{1};
    std::uint64_t corrections_ = 0;
};

/// The server's side of one attached head: who it is and where it is in the
/// log. The server itself is one log and one State; see Server below.
class Server {
public:
    Server() = default;
    explicit Server(wm::State initial) : state_(std::move(initial)) {}

    /// Apply an event that happened on the server (a window mapped, an
    /// output plugged). Returns what to broadcast and what to carry out.
    struct Step {
        down::Applied broadcast;
        wm::Outcome outcome;
    };
    Step apply(wm::Event e, std::optional<Nonce> by = std::nullopt) {
        seq_ = seq_.next();
        auto outcome = wm::apply(state_, e);
        return {down::Applied{seq_, std::move(e), by}, std::move(outcome)};
    }

    /// A message from a head. Hello is answered with the whole state (or a
    /// refusal); a Request is applied like any other event.
    struct Reply {
        std::optional<Down> to_sender;
        std::optional<Step> step;
    };
    Reply receive(const Up& u);

    [[nodiscard]] const wm::State& state() const { return state_; }
    [[nodiscard]] Seq seq() const { return seq_; }

private:
    wm::State state_;
    Seq seq_{0};
};

}  // namespace i4::link
