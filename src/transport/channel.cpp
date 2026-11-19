// src/transport/channel.cpp — SOCK_SEQPACKET with SCM_RIGHTS, or TCP.
#include "channel.hpp"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include <fcntl.h>
#include <poll.h>
#include <sodium.h>
#if defined(__linux__)
#include <linux/sockios.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Linux has these as flags; elsewhere (macOS) they are a fcntl / setsockopt.
#ifndef SOCK_NONBLOCK
#define I4_NO_SOCK_FLAGS 1
#define SOCK_NONBLOCK 0
#define SOCK_CLOEXEC 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0  // macOS: received fds are set close-on-exec below
#endif

namespace i4::transport {

namespace {

/// What SOCK_NONBLOCK / SOCK_CLOEXEC do at creation, after it (no-op on Linux).
int flags(int fd, bool nonblock = true) {
#ifdef I4_NO_SOCK_FLAGS
    if (fd < 0) return fd;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonblock) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#else
    (void)nonblock;
#endif
    return fd;
}

int accept_nb(int listener) {
#ifdef I4_NO_SOCK_FLAGS
    return flags(::accept(listener, nullptr, nullptr));
#else
    return ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
#endif
}

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

/// Interactive traffic: every packet leaves at once (no Nagle); the kernel
/// keeps at most a little UNSENT data (TCP_NOTSENT_LOWAT), so a frame never
/// queues behind seconds of stale ones and the sender's skip-when-behind
/// sees the real backlog; buffers autotune for throughput; packets are
/// marked low-delay (DSCP EF) for networks that honour it; and a dead peer
/// is noticed (keepalive) rather than waited on forever.
void tune(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    const int idle = 10, interval = 5, count = 3;
#if defined(TCP_KEEPIDLE)
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#elif defined(TCP_KEEPALIVE)
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));  // macOS's name
#endif
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#if defined(TCP_NOTSENT_LOWAT)
    const int lowat = 64 << 10;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
    const int ef = 0xb8;  // DSCP 46 (expedited forwarding) << 2
    ::setsockopt(fd, IPPROTO_IP, IP_TOS, &ef, sizeof(ef));
#if defined(IPV6_TCLASS)
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &ef, sizeof(ef));
#endif
#if defined(TCP_QUICKACK)
    ::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
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

// -- the secure link -------------------------------------------------------------
//
// Every TCP link is encrypted and both ends authenticated, with nothing to
// set up but one shared pairing key (i4d makes it; you give it to a head):
//
//   head -> i4d    "i4s1" + an ephemeral X25519 public key
//   i4d  -> head   "i4s1" + its ephemeral public key + an empty sealed box
//
// Both derive the session keys from the X25519 exchange (forward secrecy:
// a later leak of the pairing key does not open recorded sessions) MIXED
// with the pairing key (only someone who has it derives the same keys, so a
// man in the middle gets nothing). The empty box proves the session to the
// head before connect() returns; the head's first packet (Hello) proves
// itself to i4d, and nothing it says is believed before that. Then every
// packet is ChaCha20-Poly1305 with a per-direction counter as nonce: TCP is
// in order, so a replayed, dropped or reordered packet fails to decrypt and
// the link closes. ~16 bytes and microseconds per packet.

constexpr std::size_t kHello = 4 + crypto_kx_PUBLICKEYBYTES;
constexpr std::size_t kTag = crypto_aead_chacha20poly1305_ietf_ABYTES;

bool sodium() {
    static const bool ok = ::sodium_init() >= 0;
    return ok;
}

}  // namespace

struct Channel::Secure {
    bool server = false;
    bool ready = false;
    Key psk{};
    std::array<unsigned char, crypto_kx_PUBLICKEYBYTES> pk{};
    std::array<unsigned char, crypto_kx_SECRETKEYBYTES> sk{};
    std::array<unsigned char, 32> tx{}, rx{};
    std::uint64_t tx_n = 0, rx_n = 0;
    ~Secure() {
        sodium_memzero(sk.data(), sk.size());
        sodium_memzero(tx.data(), tx.size());
        sodium_memzero(rx.data(), rx.size());
        sodium_memzero(psk.data(), psk.size());
    }

    /// Session keys from the exchange, mixed with the pairing key and the
    /// transcript (both public keys).
    bool derive(const unsigned char* client_pk, const unsigned char* server_pk, const unsigned char* peer_pk) {
        std::array<unsigned char, 32> r{}, t{};
        const int rc = server ? crypto_kx_server_session_keys(r.data(), t.data(), pk.data(), sk.data(), peer_pk)
                              : crypto_kx_client_session_keys(r.data(), t.data(), pk.data(), sk.data(), peer_pk);
        if (rc != 0) return false;
        auto mix = [&](const std::array<unsigned char, 32>& k, std::array<unsigned char, 32>& out) {
            crypto_generichash_state st;
            crypto_generichash_init(&st, psk.data(), psk.size(), out.size());
            crypto_generichash_update(&st, k.data(), k.size());
            crypto_generichash_update(&st, client_pk, crypto_kx_PUBLICKEYBYTES);
            crypto_generichash_update(&st, server_pk, crypto_kx_PUBLICKEYBYTES);
            crypto_generichash_final(&st, out.data(), out.size());
        };
        mix(r, rx);
        mix(t, tx);
        sodium_memzero(r.data(), r.size());
        sodium_memzero(t.data(), t.size());
        sodium_memzero(sk.data(), sk.size());
        return true;
    }

    static void nonce(std::uint64_t n, unsigned char* out) {
        std::memset(out, 0, crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
        for (int i = 0; i < 8; ++i) out[4 + i] = static_cast<unsigned char>(n >> (8 * i));
    }
    std::vector<std::byte> seal(const std::vector<std::byte>& m) {
        std::vector<std::byte> c(m.size() + kTag);
        unsigned char nn[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
        nonce(tx_n++, nn);
        unsigned long long len = 0;
        crypto_aead_chacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(c.data()), &len,
                                                  reinterpret_cast<const unsigned char*>(m.data()), m.size(), nullptr,
                                                  0, nullptr, nn, tx.data());
        c.resize(static_cast<std::size_t>(len));
        return c;
    }
    std::optional<std::vector<std::byte>> open(const std::byte* c, std::size_t n) {
        if (n < kTag) return std::nullopt;
        std::vector<std::byte> m(n - kTag);
        unsigned char nn[crypto_aead_chacha20poly1305_ietf_NPUBBYTES];
        nonce(rx_n, nn);
        unsigned long long len = 0;
        if (crypto_aead_chacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(m.data()), &len, nullptr,
                                                      reinterpret_cast<const unsigned char*>(c), n, nullptr, 0, nn,
                                                      rx.data()) != 0)
            return std::nullopt;
        ++rx_n;
        m.resize(static_cast<std::size_t>(len));
        return m;
    }
};

std::optional<Key> parse_key(const std::string& hex) {
    std::string s;
    for (char ch : hex)
        if (!std::isspace(static_cast<unsigned char>(ch))) s += ch;
    Key k{};
    std::size_t len = 0;
    if (s.size() != 64 || ::sodium_hex2bin(k.data(), k.size(), s.c_str(), s.size(), nullptr, &len, nullptr) != 0 ||
        len != k.size())
        return std::nullopt;
    return k;
}

std::string key_hex(const Key& k) {
    std::array<char, 65> out{};
    ::sodium_bin2hex(out.data(), out.size(), k.data(), k.size());
    return out.data();
}

std::string default_key_path() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/i4/remote.key";
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/i4/remote.key";
}

std::optional<Key> load_key(const std::string& path, bool create) {
    if (!sodium()) return std::nullopt;
    if (std::FILE* f = std::fopen(path.c_str(), "r")) {
        char buf[256] = {};
        const auto n = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        return parse_key(std::string(buf, n));
    }
    if (!create) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    Key k{};
    ::randombytes_buf(k.data(), k.size());
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return std::nullopt;
    const auto text = key_hex(k) + "\n";
    const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ::close(fd);
    if (!ok) return std::nullopt;
    return k;
}

namespace {

/// Read exactly `n` bytes from a blocking socket, giving up after `ms`.
bool read_exact(int fd, std::byte* p, std::size_t n, int ms) {
    while (n > 0) {
        pollfd pf{fd, POLLIN, 0};
        if (::poll(&pf, 1, ms) <= 0) return false;
        const ssize_t r = ::recv(fd, p, n, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool write_all(int fd, const std::byte* p, std::size_t n) {
    while (n > 0) {
        const ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

Channel::Channel(Fd socket) : socket_(std::move(socket)) {
    stream_ = socket_ && is_stream(socket_.get());
    if (stream_) tune(socket_.get());
}

bool Channel::secure() const { return secure_ && secure_->ready; }

std::optional<std::pair<Key, Key>> Channel::datagram_keys() const {
    if (!secure()) return std::nullopt;
    // Separate keys for datagrams (their nonces are explicit packet numbers,
    // not the stream's counters), derived from this session's: (send, receive).
    auto derive = [](const std::array<unsigned char, 32>& k) {
        Key out{};
        static constexpr unsigned char label[] = "i4 datagrams v1";
        crypto_generichash(out.data(), out.size(), label, sizeof(label) - 1, k.data(), k.size());
        return out;
    };
    return std::pair{derive(secure_->tx), derive(secure_->rx)};
}

std::expected<std::pair<Channel, Channel>, ChannelError> Channel::pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) != 0)
        return std::unexpected(ChannelError::io);
    flags(fds[0]);
    flags(fds[1]);
    return std::pair{Channel{Fd{fds[0]}}, Channel{Fd{fds[1]}}};
}

std::expected<Fd, ChannelError> Channel::listen(const std::string& path) {
    if (is_tcp(path)) {
        const auto hp = split_tcp(path);
        if (!hp) return std::unexpected(ChannelError::io);
        const auto ai = resolve(hp->first, hp->second, true);
        if (!ai) return std::unexpected(ChannelError::io);
        for (const addrinfo* a = ai.get(); a != nullptr; a = a->ai_next) {
            Fd s{flags(::socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))};
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
    Fd s{flags(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0))};
    if (!s) return std::unexpected(ChannelError::io);
    ::unlink(path.c_str());
    if (::bind(s.get(), reinterpret_cast<const sockaddr*>(&*a), sizeof(*a)) != 0 || ::listen(s.get(), 8) != 0)
        return std::unexpected(ChannelError::io);
    return s;
}

std::expected<Channel, ChannelError> Channel::accept(const Fd& listener, const Key* key) {
    const int c = accept_nb(listener.get());
    if (c < 0) return std::unexpected(ChannelError::io);
    Channel ch{Fd{c}};
    if (ch.stream_ && key) {
        if (!sodium()) return std::unexpected(ChannelError::io);
        ch.secure_ = std::make_shared<Secure>();
        ch.secure_->server = true;
        ch.secure_->psk = *key;
    }
    return ch;
}

std::expected<Channel, ChannelError> Channel::connect(const std::string& path, const Key* key) {
    if (is_tcp(path)) {
        const auto hp = split_tcp(path);
        if (!hp) return std::unexpected(ChannelError::io);
        const auto ai = resolve(hp->first, hp->second, false);
        if (!ai) return std::unexpected(ChannelError::closed);
        for (const addrinfo* a = ai.get(); a != nullptr; a = a->ai_next) {
            Fd s{flags(::socket(a->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0), false)};
            if (!s) continue;
            // Blocking connect (a head has nothing to do until it is
            // connected), then non-blocking like every other channel.
            if (::connect(s.get(), a->ai_addr, a->ai_addrlen) != 0) continue;
            std::shared_ptr<Secure> sec;
            if (key) {
                if (!sodium()) return std::unexpected(ChannelError::io);
                sec = std::make_shared<Secure>();
                sec->psk = *key;
                crypto_kx_keypair(sec->pk.data(), sec->sk.data());
                std::array<std::byte, 4 + kHello> hello{};
                put_u32(hello.data(), kHello);
                std::memcpy(hello.data() + 4, "i4s1", 4);
                std::memcpy(hello.data() + 8, sec->pk.data(), sec->pk.size());
                std::array<std::byte, 4 + kHello + kTag> reply{};
                if (!write_all(s.get(), hello.data(), hello.size()) ||
                    !read_exact(s.get(), reply.data(), reply.size(), 5000) ||
                    get_u32(reply.data()) != kHello + kTag || std::memcmp(reply.data() + 4, "i4s1", 4) != 0)
                    return std::unexpected(ChannelError::closed);
                const auto* server_pk = reinterpret_cast<const unsigned char*>(reply.data() + 8);
                // A wrong key derives different keys: the session's sealed
                // empty box will not open.
                if (!sec->derive(sec->pk.data(), server_pk, server_pk) ||
                    !sec->open(reply.data() + 4 + kHello, kTag))
                    return std::unexpected(ChannelError::io);
                sec->ready = true;
            }
            ::fcntl(s.get(), F_SETFL, ::fcntl(s.get(), F_GETFL) | O_NONBLOCK);
            Channel ch{std::move(s)};
            ch.secure_ = std::move(sec);
            return ch;
        }
        return std::unexpected(ChannelError::closed);
    }
    auto a = address(path);
    if (!a) return std::unexpected(a.error());
    Fd s{flags(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), false)};
    if (!s) return std::unexpected(ChannelError::io);
    // Blocking connect (a local socket connects at once), then non-blocking.
    if (::connect(s.get(), reinterpret_cast<const sockaddr*>(&*a), sizeof(*a)) != 0)
        return std::unexpected(ChannelError::closed);
    ::fcntl(s.get(), F_SETFL, ::fcntl(s.get(), F_GETFL) | O_NONBLOCK);
    return Channel{std::move(s)};
}

std::expected<void, ChannelError> Channel::queue_frame(const std::vector<std::byte>& payload) {
    if (payload.size() > max_stream_packet + kTag) return std::unexpected(ChannelError::too_big);
    std::vector<std::byte> framed(4 + payload.size());
    put_u32(framed.data(), static_cast<std::uint32_t>(payload.size()));
    std::memcpy(framed.data() + 4, payload.data(), payload.size());
    out_.push_back({std::move(framed), {}});
    return flush();
}

std::expected<bool, ChannelError> Channel::handshake_server(const std::vector<std::byte>& hello) {
    auto& s = *secure_;
    if (hello.size() != kHello || std::memcmp(hello.data(), "i4s1", 4) != 0) return std::unexpected(ChannelError::io);
    const auto* client_pk = reinterpret_cast<const unsigned char*>(hello.data() + 4);
    crypto_kx_keypair(s.pk.data(), s.sk.data());
    const auto server_pk = s.pk;
    if (!s.derive(client_pk, server_pk.data(), client_pk)) return std::unexpected(ChannelError::io);
    std::vector<std::byte> reply(kHello);
    std::memcpy(reply.data(), "i4s1", 4);
    std::memcpy(reply.data() + 4, server_pk.data(), server_pk.size());
    const auto proof = s.seal({});
    reply.insert(reply.end(), proof.begin(), proof.end());
    if (auto r = queue_frame(reply); !r) return std::unexpected(r.error());
    s.ready = true;
    for (auto& early : std::exchange(early_, {}))
        if (auto r = queue_frame(s.seal(early)); !r) return std::unexpected(r.error());
    return true;
}

std::expected<void, ChannelError> Channel::send(std::vector<std::byte> bytes, std::vector<Fd> fds) {
    if (stream_) {
        // No fds cross a network; the owner never offers any to a remote
        // channel (it sends pixels instead). Framed: u32 length, then bytes.
        if (bytes.size() > max_stream_packet || !fds.empty()) return std::unexpected(ChannelError::too_big);
        if (secure_) {
            if (!secure_->ready) {
                early_.push_back(std::move(bytes));
                return {};
            }
            return queue_frame(secure_->seal(bytes));
        }
        return queue_frame(bytes);
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
#if defined(__linux__)
    if (socket_ && ::ioctl(socket_.get(), SIOCOUTQ, &queued) == 0 && queued > 0) n += static_cast<std::size_t>(queued);
#elif defined(SO_NWRITE)
    socklen_t len = sizeof(queued);
    if (socket_ && ::getsockopt(socket_.get(), SOL_SOCKET, SO_NWRITE, &queued, &len) == 0 && queued > 0)
        n += static_cast<std::size_t>(queued);
#endif
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
            if (len > max_stream_packet + kTag) return std::unexpected(ChannelError::too_big);
            // Before the handshake only a hello may arrive: refuse anything
            // bigger at once (a stranger cannot make us buffer megabytes).
            if (secure_ && !secure_->ready && len != kHello) return std::unexpected(ChannelError::io);
            if (in_.size() - at - 4 < len) break;
            const auto* body = in_.data() + at + 4;
            if (!secure_) {
                out.push_back({std::vector<std::byte>(body, body + len), {}});
            } else if (!secure_->ready) {
                if (auto r = handshake_server(std::vector<std::byte>(body, body + len)); !r)
                    return std::unexpected(r.error());
            } else {
                auto m = secure_->open(body, len);
                if (!m) return std::unexpected(ChannelError::io);  // forged, replayed, or the wrong key
                out.push_back({std::move(*m), {}});
            }
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
#ifdef I4_NO_SOCK_FLAGS
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
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
