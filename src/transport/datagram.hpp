#pragma once
// src/transport/datagram.hpp — encrypted UDP, for frames to a remote head.
//
// The TCP link stays the reliable, ordered channel (input, window manager,
// cursor). Frames go here instead: a lost datagram never holds up the next
// one (no head-of-line blocking), and remote/frames.hpp's parity rebuilds
// most losses on arrival.
//
// Every datagram is
//
//   u64 token | u64 packet number | ChaCha20-Poly1305(payload)
//
// with keys from the link's handshake (Channel::datagram_keys), the packet
// number as nonce (explicit: UDP reorders), the header authenticated too,
// and a replay window. `token` names the session (derived from the keys),
// so one server socket serves every head and a datagram with a wrong token,
// key, or a replayed number is dropped unread.
//
// A head says hello (and keeps NAT bindings open) with a datagram of its
// own every second; the server learns its address only from an
// authenticated one.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

#include "channel.hpp"

namespace i4::transport {

/// One end's crypto state for a session's datagrams.
class DatagramSession {
public:
    DatagramSession(const Key& send, const Key& receive);
    /// The session's id on the wire (the same at both ends).
    [[nodiscard]] std::uint64_t token() const { return token_; }
    [[nodiscard]] std::vector<std::byte> seal(std::span<const std::byte> payload);
    /// The payload, or nullopt: not ours, forged, or replayed.
    [[nodiscard]] std::optional<std::vector<std::byte>> open(std::span<const std::byte> datagram);
    /// A datagram's token, to find its session.
    [[nodiscard]] static std::optional<std::uint64_t> token_of(std::span<const std::byte> datagram);

private:
    Key send_{}, recv_{};
    std::uint64_t token_ = 0;
    std::uint64_t next_ = 0;
    std::uint64_t highest_ = 0;
    std::uint64_t window_ = 0;  // bit i: highest_ - i was seen
    bool any_ = false;
};

/// A UDP socket. The server binds one to the --remote address; a head
/// connects one to the session's.
class DatagramSocket {
public:
    [[nodiscard]] static std::expected<DatagramSocket, ChannelError> bind(const std::string& tcp_address);
    [[nodiscard]] static std::expected<DatagramSocket, ChannelError> connect(const std::string& tcp_address);
    [[nodiscard]] int fd() const { return fd_.get(); }

    struct Peer {
        sockaddr_storage addr{};
        socklen_t len = 0;
        friend bool operator==(const Peer& a, const Peer& b) {
            return a.len == b.len && std::memcmp(&a.addr, &b.addr, a.len) == 0;
        }
    };
    /// Send (to `to`, or the connected peer). false: the kernel had no room
    /// (the frame is lost; the head will ask for a refresh).
    bool send(std::span<const std::byte> datagram, const Peer* to = nullptr);
    /// Every datagram waiting, with who sent it.
    [[nodiscard]] std::vector<std::pair<std::vector<std::byte>, Peer>> receive();

private:
    Fd fd_;
};

}  // namespace i4::transport
