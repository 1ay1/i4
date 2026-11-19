// src/transport/channel.cpp — SOCK_SEQPACKET with SCM_RIGHTS.
#include "channel.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace i4::transport {

namespace {

constexpr std::size_t max_fds = 8;

std::expected<sockaddr_un, ChannelError> address(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (path.size() >= sizeof(a.sun_path)) return std::unexpected(ChannelError::io);
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return a;
}

}  // namespace

Channel::Channel(Fd socket) : socket_(std::move(socket)) {}

std::expected<std::pair<Channel, Channel>, ChannelError> Channel::pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0)
        return std::unexpected(ChannelError::io);
    return std::pair{Channel{Fd{fds[0]}}, Channel{Fd{fds[1]}}};
}

std::expected<Fd, ChannelError> Channel::listen(const std::string& path) {
    auto a = address(path);
    if (!a) return std::unexpected(a.error());
    Fd s{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!s) return std::unexpected(ChannelError::io);
    ::unlink(path.c_str());
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&*a), sizeof(*a)) != 0 || ::listen(s.get(), 8) != 0)
        return std::unexpected(ChannelError::io);
    return s;
}

std::expected<Channel, ChannelError> Channel::accept(const Fd& listener) {
    const int c = ::accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c < 0) return std::unexpected(ChannelError::io);
    return Channel{Fd{c}};
}

std::expected<Channel, ChannelError> Channel::connect(const std::string& path) {
    auto a = address(path);
    if (!a) return std::unexpected(a.error());
    Fd s{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)};
    if (!s) return std::unexpected(ChannelError::io);
    // Blocking connect (a local socket connects at once), then non-blocking.
    if (::connect(s.get(), reinterpret_cast<const sockaddr*>(&*a), sizeof(*a)) != 0)
        return std::unexpected(ChannelError::closed);
    ::fcntl(s.get(), F_SETFL, ::fcntl(s.get(), F_GETFL) | O_NONBLOCK);
    return Channel{std::move(s)};
}

std::expected<void, ChannelError> Channel::send(std::vector<std::byte> bytes, std::vector<Fd> fds) {
    if (bytes.size() > max_packet || fds.size() > max_fds) return std::unexpected(ChannelError::too_big);
    out_.push_back({std::move(bytes), std::move(fds)});
    return flush();
}

std::expected<void, ChannelError> Channel::flush() {
    if (!socket_) return std::unexpected(ChannelError::closed);
    while (!out_.empty()) {
        auto& p = out_.front();
        iovec io{p.bytes.data(), p.bytes.size()};
        msghdr msg{};
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * max_fds)> control{};
        if (!p.fds.empty()) {
            msg.msg_control = control.data();
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * p.fds.size());
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * p.fds.size());
            for (std::size_t i = 0; i < p.fds.size(); ++i) {
                const int fd = p.fds[i].get();
                std::memcpy(CMSG_DATA(c) + i * sizeof(int), &fd, sizeof(int));
            }
        }
        if (::sendmsg(socket_.get(), &msg, MSG_NOSIGNAL) < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
            if (errno == EINTR) continue;
            return std::unexpected(errno == EPIPE || errno == ECONNRESET ? ChannelError::closed : ChannelError::io);
        }
        out_.pop_front();  // its fds close here: the receiver has its own copies
    }
    return {};
}

std::expected<std::vector<Packet>, ChannelError> Channel::receive() {
    if (!socket_) return std::unexpected(ChannelError::closed);
    std::vector<Packet> out;
    std::vector<std::byte> buf(max_packet);
    for (;;) {
        iovec io{buf.data(), buf.size()};
        msghdr msg{};
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * max_fds)> control{};
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();
        const ssize_t n = ::recvmsg(socket_.get(), &msg, MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return out;
            if (errno == EINTR) continue;
            return std::unexpected(ChannelError::io);
        }
        Packet p;
        // Adopt every fd first, so none leaks even if the packet is refused.
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const auto count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                p.fds.emplace_back(fd);
            }
        }
        if (n == 0) {
            // SEQPACKET: zero bytes and no fds is the peer hanging up.
            if (p.fds.empty()) {
                if (!out.empty()) return out;
                return std::unexpected(ChannelError::closed);
            }
        }
        if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) return std::unexpected(ChannelError::too_big);
        p.bytes.assign(buf.begin(), buf.begin() + n);
        out.push_back(std::move(p));
    }
}

}  // namespace i4::transport
