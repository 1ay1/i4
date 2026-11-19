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
        [&](const down::Content&) -> R { return {}; });
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
            if (h.fingerprint != protocol) return {Down{down::Refused{protocol}}, std::nullopt, {}, {}};
            return {Down{down::Welcome{seq_, state_}}, std::nullopt, {}, {}};
        },
        [&](const up::Request& r) -> Reply { return {std::nullopt, apply(r.event, r.nonce), {}, {}}; },
        [&](const up::Key& k) -> Reply { return {{}, {}, Input{k}, {}}; },
        [&](const up::Motion& m) -> Reply { return {{}, {}, Input{m}, {}}; },
        [&](const up::Button& b) -> Reply { return {{}, {}, Input{b}, {}}; },
        [&](const up::Shown& s) -> Reply { return {{}, {}, {}, s}; });
}

}  // namespace i4::link
