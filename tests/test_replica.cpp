// tests/test_replica.cpp — prediction: heads that act before the server
// answers always end exactly where the server is.
//
// A simulated network: one Server, several heads, every message ENCODED and
// DECODED on its way (so the codec is in the loop), arbitrary delays in both
// directions, server-side events interleaving with the heads' requests. At
// every point, after the queues drain, each head's shown state must equal
// the server's.
#include <deque>

#include "check.hpp"
#include "i4/link/predict.hpp"
#include "session.hpp"

using namespace i4;
using namespace i4::link;

namespace {

template <class T>
T over_the_wire(const T& v) {
    auto back = decode<T>(encode(v));
    if (!back) {
        i4::test::fail(std::string("wire refused its own message: ") + std::string(describe(back.error())));
        return v;
    }
    return *back;
}

struct Head {
    Replica replica;
    std::deque<Down> inbox;  // in flight from the server, in order
    bool attached = false;   // the server has sent it Welcome
};

struct Net {
    Server server;
    std::vector<Head> heads;
    std::deque<std::pair<std::size_t, Up>> to_server;  // in flight, in order

    explicit Net(std::size_t n) : heads(n) {
        for (std::size_t i = 0; i < n; ++i) to_server.emplace_back(i, Replica::hello());
    }

    void broadcast(const down::Applied& a) {
        // Only to heads already welcomed: the Welcome they got carries every
        // event before it, so anything earlier would be applied twice.
        for (auto& h : heads)
            if (h.attached) h.inbox.push_back(over_the_wire(Down{a}));
    }

    /// The server's half of a kill, as in a real server: it unmaps.
    void carry_out(const wm::Outcome& o) {
        for (const auto& fx : o.effects)
            if (const auto* c = std::get_if<wm::fx::Close>(&fx)) broadcast(server.apply(wm::ev::WindowUnmapped{c->window}).broadcast);
    }

    void server_step() {
        if (to_server.empty()) return;
        auto [from, msg] = std::move(to_server.front());
        to_server.pop_front();
        auto reply = server.receive(over_the_wire(msg));
        if (reply.to_sender) {
            heads[from].attached = is<down::Welcome>(*reply.to_sender);
            heads[from].inbox.push_back(over_the_wire(*reply.to_sender));
        }
        if (reply.step) {
            broadcast(reply.step->broadcast);
            carry_out(reply.step->outcome);
        }
    }

    bool deliver(std::size_t h) {
        auto& head = heads[h];
        if (head.inbox.empty()) return true;
        auto r = head.replica.receive(head.inbox.front());
        head.inbox.pop_front();
        if (!r) i4::test::fail("replica refused a message");
        return r.has_value();
    }

    void drain() {
        while (!to_server.empty()) server_step();
        for (std::size_t h = 0; h < heads.size(); ++h)
            while (!heads[h].inbox.empty()) deliver(h);
    }
};

}  // namespace

TEST(a_single_head_predicts_and_is_confirmed_without_correction) {
    Net net(1);
    net.drain();
    net.broadcast(net.server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}}).broadcast);
    net.broadcast(net.server.apply(wm::ev::WindowMapped{wm::WindowId{1}, {"a", "", {}, false}}).broadcast);
    net.broadcast(net.server.apply(wm::ev::WindowMapped{wm::WindowId{2}, {"b", "", {}, false}}).broadcast);
    net.drain();
    auto& r = net.heads[0].replica;
    CHECK(r.ready());

    // The keypress shows at once, before the server has seen it.
    net.to_server.emplace_back(0, r.act(wm::ev::Run{*wm::parse("focus left")}));
    CHECK_EQ(r.pending(), 1u);
    CHECK_EQ(wm::shape(r.shown()), std::string("H[a* b]"));
    CHECK_EQ(wm::shape(r.confirmed()), std::string("H[a b*]"));

    net.drain();
    CHECK_EQ(r.pending(), 0u);
    CHECK(r.shown() == net.server.state());
    CHECK_EQ(r.corrections(), 0u);
}

TEST(a_prediction_overtaken_by_another_event_is_corrected) {
    Net net(1);
    net.broadcast(net.server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}}).broadcast);
    net.broadcast(net.server.apply(wm::ev::WindowMapped{wm::WindowId{1}, {"a", "", {}, false}}).broadcast);
    net.drain();
    auto& r = net.heads[0].replica;

    // The head moves to workspace 2; meanwhile a window maps on the server.
    net.to_server.emplace_back(0, r.act(wm::ev::Run{*wm::parse("workspace 2")}));
    net.broadcast(net.server.apply(wm::ev::WindowMapped{wm::WindowId{2}, {"b", "", {}, false}}).broadcast);
    net.drain();
    // b opened on workspace 1 (the server's order), and the head shows that.
    CHECK(r.shown() == net.server.state());
    CHECK_EQ(r.corrections(), 1u);
}

TEST(a_mismatched_build_is_refused) {
    Server server;
    auto reply = server.receive(up::Hello{protocol ^ 1});
    CHECK(reply.to_sender && is<down::Refused>(*reply.to_sender));
    Replica r;
    CHECK(r.receive(*reply.to_sender).error() == Replica::Error::refused);
}

TEST(a_gap_in_the_log_is_detected) {
    Server server;
    Replica r;
    CHECK(r.receive(server.receive(Replica::hello()).to_sender.value()).has_value());
    (void)server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}});  // lost
    auto second = server.apply(wm::ev::OutputAdded{wm::OutputInfo{"B", {1000, 0, 1000, 800}}});
    CHECK(r.receive(second.broadcast).error() == Replica::Error::out_of_order);
}

TEST(many_heads_random_delays_always_converge) {
    const unsigned seeds = i4::test::seeds_from_env(6);
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        i4::test::Generator gen(seed);
        std::mt19937 net_rng(seed * 7919u);
        Net net(3);
        net.broadcast(net.server.apply(wm::ev::OutputAdded{wm::OutputInfo{"A", {0, 0, 1000, 800}}}).broadcast);

        for (int i = 0; i < 1500; ++i) {
            switch (net_rng() % 6) {
                case 0: {  // something happens on the server
                    auto [e, note] = gen.next(net.server.state());
                    if (is<wm::ev::Run>(e)) break;  // commands come from heads
                    auto step = net.server.apply(std::move(e));
                    net.broadcast(step.broadcast);
                    net.carry_out(step.outcome);
                    break;
                }
                case 1:
                case 2: {  // a head acts on what it shows
                    const auto h = net_rng() % net.heads.size();
                    auto& r = net.heads[h].replica;
                    if (!r.ready()) break;
                    auto [e, note] = gen.next(r.shown());
                    if (!is<wm::ev::Run>(e)) break;  // heads only send commands
                    net.to_server.emplace_back(h, r.act(std::move(e)));
                    break;
                }
                case 3: net.server_step(); break;
                default: {
                    const auto h = net_rng() % net.heads.size();
                    for (auto n = net_rng() % 4; n > 0; --n)
                        if (!net.deliver(h)) return;
                    break;
                }
            }
            // Never ahead of the truth: confirmed is always a real past state.
            for (const auto& h : net.heads)
                if (h.replica.ready() && !check(h.replica.shown())) {
                    i4::test::fail("a head shows an invalid state");
                    return;
                }
        }
        net.drain();
        for (std::size_t h = 0; h < net.heads.size(); ++h) {
            const auto& r = net.heads[h].replica;
            if (r.pending() != 0 || !(r.shown() == net.server.state()) || !(r.confirmed() == net.server.state())) {
                i4::test::fail("seed " + std::to_string(seed) + ": head " + std::to_string(h) + " diverged");
                return;
            }
        }
    }
}
