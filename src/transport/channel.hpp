#pragma once
// src/transport/channel.hpp — link messages between two processes.
//
//   Channel a, b;  std::tie(a, b) = Channel::pair();
//   a.send(encode(up), {});                    // a packet, no fds
//   b.send(encode(content), {std::move(fd)});  // a packet, one memfd riding along
//
// SOCK_SEQPACKET, so the kernel keeps message boundaries: one packet is one
// encoded Up or Down, and there is no framing code to get wrong. File
// descriptors travel in the same packet (SCM_RIGHTS), which is how a
// window's pixels reach a head on the same machine without being copied:
// the server passes the memfd it filled, the head maps it.
//
// Non-blocking both ways. A send that would block queues, and flush()
// drains the queue when the socket is writable again; the owner watches
// for writability while wants_write(). Packets go out in the order they
// were sent, always — the replica's log depends on it.
//
// Two kinds of socket behind one interface, chosen by the address:
//
//   /run/user/1000/i4-link     AF_UNIX SOCK_SEQPACKET: packets AND fds (a
//                              local head maps the client's own memory)
//   tcp:HOST:PORT              TCP, TCP_NODELAY, each packet as a u32 LE
//                              length + bytes; no fds (a remote head is sent
//                              pixels, see server/remote.hpp)
//
// The kind is read off the socket itself (SO_TYPE), so an accepted
// connection knows which it is. remote() tells the owner; unsent() is how
// far behind the network is (queued here + still in the kernel's buffer),
// which is what lets a sender drop stale frames instead of queueing them.

#include <cstddef>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jaal/platform/handle.hpp>

namespace i4::transport {

using Fd = jaal::platform::owned_handle;

struct Packet {
    std::vector<std::byte> bytes;
    std::vector<Fd> fds;
};

enum class ChannelError : std::uint8_t { closed, io, too_big };

class Channel {
public:
    Channel() = default;
    explicit Channel(Fd socket);

    /// Two connected ends, for one process to hand the other half to a child
    /// (or to itself, in tests).
    [[nodiscard]] static std::expected<std::pair<Channel, Channel>, ChannelError> pair();
    /// Listen on an address: a path (a stale one is unlinked first) or
    /// tcp:HOST:PORT. Accept one end.
    [[nodiscard]] static std::expected<Fd, ChannelError> listen(const std::string& address);
    [[nodiscard]] static std::expected<Channel, ChannelError> accept(const Fd& listener);
    [[nodiscard]] static std::expected<Channel, ChannelError> connect(const std::string& address);
    /// tcp:HOST:PORT (the network) rather than a local path.
    [[nodiscard]] static bool is_tcp(const std::string& address) { return address.starts_with("tcp:"); }

    /// Queue a packet and write as much of the queue as the socket takes.
    std::expected<void, ChannelError> send(std::vector<std::byte> bytes, std::vector<Fd> fds = {});
    /// Write what is queued. Call when the socket is writable.
    std::expected<void, ChannelError> flush();
    [[nodiscard]] bool wants_write() const { return !out_.empty(); }

    /// Every whole packet waiting to be read. Empty when there is nothing
    /// more (would block); an error when the other end is gone.
    std::expected<std::vector<Packet>, ChannelError> receive();

    [[nodiscard]] int fd() const { return socket_.get(); }
    [[nodiscard]] bool open() const { return socket_.valid(); }
    /// Across a network: no fds, so frames travel as bytes.
    [[nodiscard]] bool remote() const { return stream_; }
    /// Bytes sent but not yet on the wire: queued here plus what the kernel
    /// still holds. A frame sender skips frames while this is large.
    [[nodiscard]] std::size_t unsent() const;

    /// Largest packet: a whole wm::State at Welcome has to fit, and over a
    /// network a whole keyframe (a 4K window compressed, worst case raw).
    static constexpr std::size_t max_packet = 4u << 20;
    static constexpr std::size_t max_stream_packet = 96u << 20;

private:
    Fd socket_;
    std::deque<Packet> out_;
    bool stream_ = false;
    std::size_t written_ = 0;          // stream: bytes of out_.front() (with its header) already sent
    std::vector<std::byte> in_;        // stream: bytes read, not yet a whole packet
};

}  // namespace i4::transport
