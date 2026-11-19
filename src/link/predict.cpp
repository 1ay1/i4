// src/link/predict.cpp — the replica's bookkeeping.
#include "i4/link/predict.hpp"

#include "i4/base/match.hpp"

namespace i4::link {

std::expected<void, Replica::Error> Replica::receive(const Down& d) {
    using R = std::expected<void, Error>;
    return match(
        d,
        [&](const down::Welcome& w) -> R {
            confirmed_ = w.state;
            seq_ = w.seq;
            rebuild();
            return {};
        },
        [&](const down::Refused&) -> R { return std::unexpected(Error::refused); },
        [&](const down::Applied& a) -> R {
            if (!seq_) return std::unexpected(Error::before_welcome);
            if (a.seq != seq_->next()) return std::unexpected(Error::out_of_order);
            seq_ = a.seq;
            (void)wm::apply(confirmed_, a.event);
            if (a.by && !pending_.empty() && pending_.front().nonce == *a.by) pending_.pop_front();
            rebuild();
            return {};
        },
        // Pixels are the data plane: the head's renderer takes them, the
        // replica has nothing to do with them.
        [&](const down::Content&) -> R { return {}; },
        // Nor is the app list: the head's launcher keeps it.
        [&](const down::Apps&) -> R { return {}; },
        [&](const down::Notice&) -> R { return {}; },
        [&](const down::Cursor&) -> R { return {}; });
}

void Replica::rebuild() {
    wm::State next = confirmed_;
    for (const auto& p : pending_) (void)wm::apply(next, p.event);
    if (!(next == shown_) && !pending_.empty()) ++corrections_;
    shown_ = std::move(next);
}

Server::Reply Server::receive(const Up& u) {
    return match(
        u,
        [&](const up::Hello& h) -> Reply {
            if (h.fingerprint != protocol) return {.to_sender = Down{down::Refused{protocol}}};
            return {.to_sender = Down{down::Welcome{seq_, state_}}};
        },
        [&](const up::Request& r) -> Reply { return {.step = apply(r.event, r.nonce)}; },
        [&](const up::Key& k) -> Reply { return {.input = Input{k}}; },
        [&](const up::Motion& m) -> Reply { return {.input = Input{m}}; },
        [&](const up::Button& b) -> Reply { return {.input = Input{b}}; },
        [&](const up::Keyboard& k) -> Reply { return {.input = Input{k}}; },
        [&](const up::Modifiers& k) -> Reply { return {.input = Input{k}}; },
        [&](const up::Scroll& s) -> Reply { return {.input = Input{s}}; },
        [&](const up::Shown& s) -> Reply { return {.shown = s}; },
        [&](const up::Released& r) -> Reply { return {.released = r}; },
        [&](const up::Caps& c) -> Reply { return {.caps = c}; },
        // The host keeps the frame streams; it acts on this itself.
        [&](const up::Refresh&) -> Reply { return {}; },
        // The host unwraps numbered input (dedupe, order) before it gets here.
        [&](const up::Sequenced&) -> Reply { return {}; });
}

}  // namespace i4::link
