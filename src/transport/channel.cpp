// src/transport/channel.cpp — SOCK_SEQPACKET with SCM_RIGHTS, or TCP.
#include "channel.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>

#include <fcntl.h>
#include <linux/sockios.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
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

/// tcp:HOST:PORT -> (host, port). HOST may be empty (any), a name, an IPv4
/// address or a bracketed IPv6 one: tcp:[::1]:7400.
std::optional<std::pair<std::string, std::string>> split_tcp(const std::string& address) {
    std::string rest = address.substr(4);
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos || colon + 1 == rest.size()) return std::nullopt;
    std::string host = rest.substr(0, colon), port = rest.substr(colon + 1);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    return std::pair{host, port};
}

struct AddrinfoFree {
    void operator()(addrinfo* a) const { ::freeaddrinfo(a); }
};

std::unique_ptr<addrinfo, AddrinfoFree> resolve(const std::string& host, const std::string& port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    addrinfo* out = nullptr;
    if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(), &hints, &out) != 0) return nullptr;
    return std::unique_ptr<addrinfo, AddrinfoFree>{out};
}

/// Interactive traffic: every packet leaves at once (no Nagle), the kernel
/// buffers are big enough for a frame at link speed, and a dead peer is
/// noticed (keepalive) rather than waited on forever.
void tune(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    const int idle = 10, interval = 5, count = 3;
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    const int buf = 8 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
}

bool is_stream(int fd) {
    int type = 0;
    socklen_t len = sizeof(type);
    return ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_STREAM;
}

void put_u32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xff);
}
std::uint32_t get_u32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace

Channel::Channel(Fd socket) : socket_(std::move(socket)) {
    stream_ = socket_ && is_stream(socket_.get());
    if (stream_) tune(socket_.get());
}

std::expected<std::pair<Channel, Channel>, ChannelError> Channel::pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0)
        return std::unexpected(ChannelError::io);
    return std::pair{Channel{Fd{fds[0]}}, Channel{Fd{fds[1]}}};
}

std::expected<Fd, ChannelError> Channel::listen(const std::string& path) {
    if (is_tcp(path)) {
        const auto hp = split_tcp(path);
        if (!hp) return std::unexpected(ChannelError::io);
        const auto ai = resolve(hp->first, hp->second, true);
        if (!ai) return std::unexpected(ChannelError::io);
        for (const addrinfo* a = ai.get(); a != nullptr; a = a->ai_next) {
            Fd s{::socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
            if (!s) continue;
            const int one = 1;
            ::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (a->ai_family == AF_INET6) {
                const int zero = 0;  // [::] takes IPv4 too
                ::setsockopt(s.get(), IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
            }
            if (::bind(s.get(), a->ai_addr, a->ai_addrlen) == 0 && ::listen(s.get(), 8) == 0) return s;
        }
        return std::unexpected(ChannelError::io);
    }
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
    if (is_tcp(path)) {
        const auto hp = split_tcp(path);
        if (!hp) return std::unexpected(ChannelError::io);
        const auto ai = resolve(hp->first, hp->second, false);
        if (!ai) return std::unexpected(ChannelError::closed);
        for (const addrinfo* a = ai.get(); a != nullptr; a = a->ai_next) {
            Fd s{::socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0)};
            if (!s) continue;
            // Blocking connect (a head has nothing to do until it is
            // connected), then non-blocking like every other channel.
            if (::connect(s.get(), a->ai_addr, a->ai_addrlen) != 0) continue;
            ::fcntl(s.get(), F_SETFL, ::fcntl(s.get(), F_GETFL) | O_NONBLOCK);
            return Channel{std::move(s)};
        }
        return std::unexpected(ChannelError::closed);
    }
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
    if (stream_) {
        // No fds cross a network; the owner never offers any to a remote
        // channel (it sends pixels instead). Framed: u32 length, then bytes.
        if (bytes.size() > max_stream_packet || !fds.empty()) return std::unexpected(ChannelError::too_big);
        std::vector<std::byte> framed(4 + bytes.size());
        put_u32(framed.data(), static_cast<std::uint32_t>(bytes.size()));
        std::memcpy(framed.data() + 4, bytes.data(), bytes.size());
        out_.push_back({std::move(framed), {}});
        return flush();
    }
    if (bytes.size() > max_packet || fds.size() > max_fds) return std::unexpected(ChannelError::too_big);
    out_.push_back({std::move(bytes), std::move(fds)});
    return flush();
}

std::size_t Channel::unsent() const {
    std::size_t n = 0;
    for (const auto& p : out_) n += p.bytes.size();
    n -= written_;
    int queued = 0;
    if (socket_ && ::ioctl(socket_.get(), SIOCOUTQ, &queued) == 0 && queued > 0) n += static_cast<std::size_t>(queued);
    return n;
}

std::expected<void, ChannelError> Channel::flush() {
    if (!socket_) return std::unexpected(ChannelError::closed);
    if (stream_) {
        while (!out_.empty()) {
            auto& p = out_.front();
            const ssize_t n = ::send(socket_.get(), p.bytes.data() + written_, p.bytes.size() - written_, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return {};
                if (errno == EINTR) continue;
                return std::unexpected(errno == EPIPE || errno == ECONNRESET ? ChannelError::closed : ChannelError::io);
            }
            written_ += static_cast<std::size_t>(n);
            if (written_ == p.bytes.size()) {
                out_.pop_front();
                written_ = 0;
            }
        }
        return {};
    }
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
    if (stream_) {
        std::array<std::byte, 1 << 16> chunk;
        bool closed = false;
        for (;;) {
            const ssize_t n = ::recv(socket_.get(), chunk.data(), chunk.size(), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                return std::unexpected(ChannelError::io);
            }
            if (n == 0) {
                closed = true;
                break;
            }
            in_.insert(in_.end(), chunk.begin(), chunk.begin() + n);
        }
        // Every whole packet; a partial one waits for the rest.
        std::size_t at = 0;
        while (in_.size() - at >= 4) {
            const auto len = get_u32(in_.data() + at);
            if (len > max_stream_packet) return std::unexpected(ChannelError::too_big);
            if (in_.size() - at - 4 < len) break;
            out.push_back({std::vector<std::byte>(in_.begin() + static_cast<std::ptrdiff_t>(at + 4),
                                                  in_.begin() + static_cast<std::ptrdiff_t>(at + 4 + len)),
                           {}});
            at += 4 + len;
        }
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(at));
        if (closed && out.empty()) return std::unexpected(ChannelError::closed);
        return out;
    }
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
