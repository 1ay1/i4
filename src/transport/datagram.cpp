// src/transport/datagram.cpp — UDP + ChaCha20-Poly1305 + replay window.
#include "datagram.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <sodium.h>
#include <unistd.h>

namespace i4::transport {

namespace {

constexpr std::size_t kHeader = 16;
constexpr std::size_t kTag = crypto_aead_chacha20poly1305_ietf_ABYTES;

void put64(std::byte* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::byte>(v >> (8 * i));
}
std::uint64_t get64(const std::byte* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::to_integer<std::uint64_t>(p[i]) << (8 * i);
    return v;
}
void nonce(std::uint64_t n, unsigned char* out) {
    std::memset(out, 0, crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<unsigned char>(n >> (8 * i));
}

/// tcp:HOST:PORT -> resolved UDP addresses.
struct Resolved {
    addrinfo* ai = nullptr;
    ~Resolved() {
        if (ai) ::freeaddrinfo(ai);
    }
};
bool resolve(const std::string& address, bool passive, Resolved& out) {
    if (!address.starts_with("tcp:")) return false;
    std::string rest = address.substr(4);
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos) return false;
    std::string host = rest.substr(0, colon), port = rest.substr(colon + 1);
    if (host.size() >= 2 && host.front() == '[') host = host.substr(1, host.size() - 2);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    return ::getaddrinfo(host.empty() ? nullptr : host.c_str(), port.c_str(), &hints, &out.ai) == 0;
}

void tune(int fd) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    // Room for a keyframe burst both ways; low-delay marking for the LAN.
    const int buf = 8 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    const int ef = 0xb8;
    ::setsockopt(fd, IPPROTO_IP, IP_TOS, &ef, sizeof(ef));
#if defined(IPV6_TCLASS)
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &ef, sizeof(ef));
#endif
}

}  // namespace

DatagramSession::DatagramSession(const Key& send, const Key& receive) : send_(send), recv_(receive) {
    // The token: the same at both ends (one's send key is the other's
    // receive key), and reveals nothing about either.
    const auto& a = std::min(send, receive), &b = std::max(send, receive);
    std::array<unsigned char, 64> both{};
    std::memcpy(both.data(), a.data(), 32);
    std::memcpy(both.data() + 32, b.data(), 32);
    unsigned char t[8];
    crypto_generichash(t, sizeof(t), both.data(), both.size(), nullptr, 0);
    for (int i = 0; i < 8; ++i) token_ |= static_cast<std::uint64_t>(t[i]) << (8 * i);
}

std::vector<std::byte> DatagramSession::seal(std::span<const std::byte> payload) {
    std::vector<std::byte> out(kHeader + payload.size() + kTag);
    const auto n = next_++;
    put64(out.data(), token_);
    put64(out.data() + 8, n);
    unsigned char nn[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    nonce(n, nn);
    unsigned long long len = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(out.data() + kHeader), &len,
                                              reinterpret_cast<const unsigned char*>(payload.data()), payload.size(),
                                              reinterpret_cast<const unsigned char*>(out.data()), kHeader, nullptr, nn,
                                              send_.data());
    out.resize(kHeader + static_cast<std::size_t>(len));
    return out;
}

std::optional<std::uint64_t> DatagramSession::token_of(std::span<const std::byte> d) {
    if (d.size() < kHeader + kTag) return std::nullopt;
    return get64(d.data());
}

std::optional<std::vector<std::byte>> DatagramSession::open(std::span<const std::byte> d) {
    if (d.size() < kHeader + kTag || get64(d.data()) != token_) return std::nullopt;
    const auto n = get64(d.data() + 8);
    // Replay window: 64 packets behind the highest seen.
    if (any_ && n <= highest_ && (highest_ - n >= 64 || (window_ >> (highest_ - n)) & 1u)) return std::nullopt;
    std::vector<std::byte> out(d.size() - kHeader - kTag);
    unsigned char nn[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
    nonce(n, nn);
    unsigned long long len = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &len, nullptr,
                                                  reinterpret_cast<const unsigned char*>(d.data() + kHeader),
                                                  d.size() - kHeader, reinterpret_cast<const unsigned char*>(d.data()),
                                                  kHeader, nn, recv_.data()) != 0)
        return std::nullopt;
    if (!any_ || n > highest_) {
        const auto shift = any_ ? n - highest_ : 64;
        window_ = shift >= 64 ? 1u : (window_ << shift) | 1u;
        highest_ = n;
        any_ = true;
    } else {
        window_ |= std::uint64_t{1} << (highest_ - n);
    }
    out.resize(static_cast<std::size_t>(len));
    return out;
}

std::expected<DatagramSocket, ChannelError> DatagramSocket::bind(const std::string& address) {
    Resolved r;
    if (!resolve(address, true, r)) return std::unexpected(ChannelError::io);
    for (const addrinfo* a = r.ai; a != nullptr; a = a->ai_next) {
        Fd s{::socket(a->ai_family, SOCK_DGRAM, 0)};
        if (!s) continue;
        if (a->ai_family == AF_INET6) {
            const int zero = 0;
            ::setsockopt(s.get(), IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        }
        if (::bind(s.get(), a->ai_addr, a->ai_addrlen) != 0) continue;
        tune(s.get());
        DatagramSocket out;
        out.fd_ = std::move(s);
        return out;
    }
    return std::unexpected(ChannelError::io);
}

std::expected<DatagramSocket, ChannelError> DatagramSocket::connect(const std::string& address) {
    Resolved r;
    if (!resolve(address, false, r)) return std::unexpected(ChannelError::io);
    for (const addrinfo* a = r.ai; a != nullptr; a = a->ai_next) {
        Fd s{::socket(a->ai_family, SOCK_DGRAM, 0)};
        if (!s || ::connect(s.get(), a->ai_addr, a->ai_addrlen) != 0) continue;
        tune(s.get());
        DatagramSocket out;
        out.fd_ = std::move(s);
        return out;
    }
    return std::unexpected(ChannelError::closed);
}

bool DatagramSocket::send(std::span<const std::byte> d, const Peer* to) {
    for (;;) {
        const auto n = to ? ::sendto(fd_.get(), d.data(), d.size(), 0, reinterpret_cast<const sockaddr*>(&to->addr), to->len)
                          : ::send(fd_.get(), d.data(), d.size(), 0);
        if (n >= 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

std::vector<std::pair<std::vector<std::byte>, DatagramSocket::Peer>> DatagramSocket::receive() {
    std::vector<std::pair<std::vector<std::byte>, Peer>> out;
    std::array<std::byte, 2048> buf;
    for (int i = 0; i < 4096; ++i) {
        Peer p;
        p.len = sizeof(p.addr);
        const auto n = ::recvfrom(fd_.get(), buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&p.addr), &p.len);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  // EAGAIN, or an ICMP error from an earlier send: nothing to read
        }
        out.emplace_back(std::vector<std::byte>(buf.begin(), buf.begin() + n), p);
    }
    return out;
}

}  // namespace i4::transport
