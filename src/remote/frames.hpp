#pragma once
// src/remote/frames.hpp — a frame as datagrams, with forward error correction.
//
// Over UDP a lost packet is lost: waiting for a retransmission costs a round
// trip, which is the latency we are trying not to have. So each frame is cut
// into fragments of at most `chunk` bytes, and every group of `group` data
// fragments carries one XOR parity fragment: any ONE loss per group is
// rebuilt on arrival, with no round trip. A frame of one fragment (a
// keystroke: a few hundred bytes) is simply sent twice.
//
// A frame that cannot be rebuilt is reported lost (by window, when any of
// its fragments arrived), and the head asks the session to refresh that
// window: the next frame stands alone (a keyframe), so nothing waits on the
// lost one.
//
// Pure bytes: no sockets, no crypto (transport/datagram.hpp adds those), so
// tests/test_frames.cpp checks loss patterns exactly.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace i4::remote {

struct FragmentHeader {
    std::uint32_t frame = 0;
    std::uint32_t window = 0;
    std::uint16_t index = 0;       // < data_count: data; else parity of group index - data_count
    std::uint16_t data_count = 0;
    std::uint32_t total = 0;       // the frame's bytes
    std::uint16_t group = 0;       // data fragments per parity
    std::uint16_t chunk = 0;       // a full data fragment's bytes
    static constexpr std::size_t size = 20;
};

/// Fragments of `bytes` (each: header + payload), data first, then parity.
[[nodiscard]] std::vector<std::vector<std::byte>> fragment(std::uint32_t frame, std::uint32_t window,
                                                           std::span<const std::byte> bytes, std::size_t chunk = 1140,
                                                           std::size_t group = 8);

/// Collects fragments into frames.
class Reassembly {
public:
    struct Frame {
        std::uint32_t frame;
        std::uint32_t window;
        std::vector<std::byte> bytes;
    };
    /// One fragment in; a frame out when this one completed it (or made it
    /// rebuildable). Malformed fragments are ignored.
    [[nodiscard]] std::optional<Frame> add(std::span<const std::byte> fragment);
    /// Frames that will never complete: older than `newest - slack` frames.
    /// Each lost frame's window (when known) is returned once; `unknown`
    /// counts frames of which nothing at all arrived.
    struct Lost {
        std::vector<std::uint32_t> windows;
        std::size_t unknown = 0;
    };
    [[nodiscard]] Lost expire(std::uint32_t slack = 2);
    /// Also give up on frames with fragments missing and none arriving for
    /// `quiet` (a LAN delivers a frame's fragments within microseconds of
    /// each other): loss is reported without waiting for newer frames.
    [[nodiscard]] Lost expire_quiet(std::chrono::steady_clock::time_point now, std::chrono::microseconds quiet);
    /// Some frame is partly here (worth checking again soon).
    [[nodiscard]] bool pending() const { return !partial_.empty(); }

private:
    struct Partial {
        FragmentHeader h;
        std::map<std::uint16_t, std::vector<std::byte>> pieces;  // index -> payload
        std::chrono::steady_clock::time_point last{};
    };
    std::optional<Frame> try_complete(Partial& p);
    std::map<std::uint32_t, Partial> partial_;
    std::optional<std::uint32_t> newest_;  // highest frame id seen
    std::uint32_t done_below_ = 0;         // every frame below this is complete or reported
    std::map<std::uint32_t, bool> finished_;  // completed frames at or above done_below_
};

}  // namespace i4::remote
