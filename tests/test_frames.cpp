// tests/test_frames.cpp — frames as datagrams: FEC rebuilds, loss is reported.
#include "check.hpp"

#include <random>

#include "remote/frames.hpp"

using namespace i4;

namespace {

std::vector<std::byte> payload(std::size_t n, std::uint32_t seed) {
    std::mt19937 r{seed};
    std::vector<std::byte> out(n);
    for (auto& b : out) b = static_cast<std::byte>(r());
    return out;
}

}  // namespace

TEST(a_frame_arrives_whole) {
    const auto bytes = payload(10000, 1);
    const auto frags = remote::fragment(0, 7, bytes);
    remote::Reassembly r;
    std::optional<remote::Reassembly::Frame> got;
    for (const auto& f : frags)
        if (auto x = r.add(f)) got = x;
    CHECK(got && got->bytes == bytes && got->window == 7);
}

TEST(one_loss_per_group_is_rebuilt_without_a_round_trip) {
    const auto bytes = payload(30000, 2);  // 27 data fragments, 4 groups
    const auto frags = remote::fragment(0, 1, bytes);
    for (std::size_t drop : {0u, 5u, 26u}) {
        remote::Reassembly r;
        std::optional<remote::Reassembly::Frame> got;
        for (std::size_t i = 0; i < frags.size(); ++i)
            if (i != drop)
                if (auto x = r.add(frags[i])) got = x;
        CHECK(got && got->bytes == bytes);
    }
}

TEST(a_one_fragment_frame_survives_losing_either_copy) {
    const auto bytes = payload(600, 3);  // a keystroke
    const auto frags = remote::fragment(0, 1, bytes);
    CHECK(frags.size() == 3u);
    remote::Reassembly r;
    const auto got = r.add(frags[1]);  // only the parity copy arrives
    CHECK(got && got->bytes == bytes);
}

TEST(an_unrecoverable_frame_is_reported_by_window) {
    const auto a = remote::fragment(0, 4, payload(20000, 4));
    const auto b = remote::fragment(1, 5, payload(500, 5));
    const auto c = remote::fragment(2, 5, payload(500, 6));
    const auto d = remote::fragment(3, 5, payload(500, 7));
    remote::Reassembly r;
    (void)r.add(a[0]);  // two losses in its first group: gone
    for (const auto* f : {&b, &c, &d})
        for (const auto& x : *f) (void)r.add(x);
    const auto lost = r.expire(2);
    CHECK((lost.windows == std::vector<std::uint32_t>{4}));
    CHECK(lost.unknown == 0u);
}

TEST(a_frame_of_which_nothing_arrived_is_counted) {
    remote::Reassembly r;
    for (std::uint32_t id : {0u, 2u, 3u, 4u})
        for (const auto& x : remote::fragment(id, 1, payload(100, id))) (void)r.add(x);
    const auto lost = r.expire(2);
    CHECK(lost.unknown == 1u && lost.windows.empty());
}

TEST(garbage_is_ignored) {
    remote::Reassembly r;
    std::vector<std::byte> junk(64, std::byte{0xff});
    CHECK(!r.add(junk).has_value());
    CHECK(!r.add(std::span<const std::byte>{}).has_value());
}
