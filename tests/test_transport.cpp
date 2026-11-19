// tests/test_transport.cpp — the network link: framing and the secure handshake.
#include "check.hpp"

#include <chrono>
#include <future>
#include <thread>

#include <poll.h>
#include <sys/socket.h>

#include "transport/channel.hpp"

using namespace i4;
using namespace i4::transport;

namespace {

std::vector<std::byte> bytes(std::string_view s) {
    return {reinterpret_cast<const std::byte*>(s.data()), reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

/// Pump `ch` until it yields packets (or ~2 s pass).
std::expected<std::vector<Packet>, ChannelError> pump(Channel& ch) {
    for (int i = 0; i < 200; ++i) {
        pollfd p{ch.fd(), POLLIN, 0};
        ::poll(&p, 1, 10);
        auto r = ch.receive();
        if (!r || !r->empty()) return r;
        (void)ch.flush();
    }
    return std::vector<Packet>{};
}

Key key_of(std::uint8_t b) {
    Key k{};
    k.fill(b);
    return k;
}

struct Pair {
    Channel server, client;
    bool ok = false;
};

/// A listener on a free loopback port; a client connects with `ck` while the
/// server (keyed `sk`) pumps the handshake.
Pair connect_pair(const Key* sk, const Key* ck, int port) {
    Pair out;
    const std::string addr = "tcp:127.0.0.1:" + std::to_string(port);
    auto l = Channel::listen(addr);
    if (!l) return out;
    auto client = std::async(std::launch::async, [&] { return Channel::connect(addr, ck); });
    // Accept, then pump until the client finishes (or fails) its handshake.
    std::optional<Channel> server;
    for (int i = 0; i < 300 && !server; ++i) {
        if (auto a = Channel::accept(*l, sk)) server = std::move(*a);
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!server) return out;
    while (client.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready) {
        (void)server->receive();
        (void)server->flush();
    }
    auto c = client.get();
    out.server = std::move(*server);
    if (!c) return out;
    out.client = std::move(*c);
    out.ok = true;
    return out;
}

}  // namespace

TEST(keys_round_trip_through_hex) {
    const auto k = key_of(0xab);
    CHECK(parse_key(key_hex(k)) == k);
    CHECK(!parse_key("nothex").has_value());
    CHECK(!parse_key(std::string(62, 'a')).has_value());
}

TEST(plain_tcp_frames_packets) {
    auto p = connect_pair(nullptr, nullptr, 47411);
    CHECK(p.ok);
    if (!p.ok) return;
    CHECK(p.client.remote() && !p.client.secure());
    (void)p.client.send(bytes("hello"));
    (void)p.client.send(bytes("world"));
    auto got = pump(p.server);
    std::vector<std::string> s;
    for (int i = 0; i < 5 && got && s.size() < 2; ++i) {
        for (auto& pk : *got) s.emplace_back(reinterpret_cast<const char*>(pk.bytes.data()), pk.bytes.size());
        if (s.size() < 2) got = pump(p.server);
    }
    CHECK((s == std::vector<std::string>{"hello", "world"}));
}

TEST(the_right_key_gets_an_encrypted_link_both_ways) {
    const auto k = key_of(7);
    auto p = connect_pair(&k, &k, 47412);
    CHECK(p.ok);
    if (!p.ok) return;
    CHECK(p.client.secure());
    (void)p.client.send(bytes("up"));
    auto got = pump(p.server);
    CHECK(got && got->size() == 1 && got->front().bytes == bytes("up"));
    CHECK(p.server.secure());
    (void)p.server.send(bytes("down"));
    got = pump(p.client);
    CHECK(got && got->size() == 1 && got->front().bytes == bytes("down"));
}

TEST(a_wrong_key_never_connects) {
    const auto k = key_of(7), wrong = key_of(8);
    auto p = connect_pair(&k, &wrong, 47413);
    CHECK(!p.ok);
}

TEST(a_tampered_packet_closes_the_link) {
    const auto k = key_of(9);
    auto p = connect_pair(&k, &k, 47414);
    CHECK(p.ok);
    if (!p.ok) return;
    (void)p.client.send(bytes("first"));
    CHECK(pump(p.server).has_value());
    // Raw bytes on the wire, not sealed with the session key: refused.
    const unsigned char forged[] = {20, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    (void)::send(p.client.fd(), forged, sizeof(forged), 0);
    CHECK(!pump(p.server).has_value());
}
