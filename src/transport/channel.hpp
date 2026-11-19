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
// POSIX. A head on another OS gets its own transport behind the same
// encode/decode; the link layer above never knows which.

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
    /// Listen on a path (unlinking a stale one first), and accept one end.
    [[nodiscard]] static std::expected<Fd, ChannelError> listen(const std::string& path);
    [[nodiscard]] static std::expected<Channel, ChannelError> accept(const Fd& listener);
    [[nodiscard]] static std::expected<Channel, ChannelError> connect(const std::string& path);

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

    /// Largest packet: a whole wm::State at Welcome has to fit.
    static constexpr std::size_t max_packet = 4u << 20;

private:
    Fd socket_;
    std::deque<Packet> out_;
};

}  // namespace i4::transport
