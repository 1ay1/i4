// wire.hpp — the Wayland wire format as a fold over Sig<...>.
//
// There is exactly one encoder and one decoder in this library. Neither knows
// the name of a single Wayland message: both are generic over the algebra, so
// adding a protocol adds *data*, never code.
//
// Wire format (little-endian on every platform Wayland runs on):
//   header: u32 object_id, u32 (size << 16 | opcode)   // size includes header
//   int/uint/fixed/object/new_id : 1 word
//   string : u32 len (incl. NUL), bytes, padded to 4   // len==0 means null
//   array  : u32 len, bytes, padded to 4
//   fd     : nothing in-band; sent via SCM_RIGHTS
#pragma once

#include <cstring>
#include <vector>

#include "algebra.hpp"

namespace i4::wl::wire {

using word = std::uint32_t;
using id_t = std::uint32_t;

constexpr std::size_t pad4(std::size_t n) { return (n + 3u) & ~std::size_t{3}; }

// ---------------------------------------------------------------------------
// Encoder. Appends words to a caller-owned buffer, collects fds on the side.
// ---------------------------------------------------------------------------
struct Encoder {
    std::vector<word>& buf;
    std::vector<int>& fds;
    std::size_t header = 0;

    void begin(id_t target, std::uint32_t opcode) {
        header = buf.size();
        buf.push_back(target);
        buf.push_back(opcode);  // size patched in end()
    }

    void end() {
        const std::size_t bytes = (buf.size() - header) * sizeof(word);
        buf[header + 1] |= static_cast<word>(bytes) << 16;
    }

    void put_u32(word w) { buf.push_back(w); }
    void put_i32(std::int32_t v) { buf.push_back(static_cast<word>(v)); }

    void put_bytes(const void* p, std::size_t n, bool nul_terminated) {
        const std::size_t len = n + (nul_terminated ? 1u : 0u);
        buf.push_back(static_cast<word>(len));
        const std::size_t words = pad4(len) / 4;
        const std::size_t start = buf.size();
        buf.resize(start + words, 0u);
        if (n) std::memcpy(buf.data() + start, p, n);
    }

    void put_string(std::string_view s) { put_bytes(s.data(), s.size(), true); }
    void put_null_string() { buf.push_back(0u); }
    void put_array(std::span<const std::byte> a) { put_bytes(a.data(), a.size(), false); }
    void put_fd(int fd) { fds.push_back(fd); }
};

// ---------------------------------------------------------------------------
// Decoder. Borrows one message body; strings/arrays alias the buffer, so a
// handler must copy anything it keeps. Malformed input sets `bad`, never
// reads out of bounds, and never throws.
// ---------------------------------------------------------------------------
struct Decoder {
    std::span<const word> body;
    std::span<const int> fds;
    std::size_t pos = 0;
    std::size_t fd_pos = 0;
    bool bad = false;

    bool avail(std::size_t n) const { return pos + n <= body.size(); }

    word get_u32() {
        if (!avail(1)) { bad = true; return 0; }
        return body[pos++];
    }
    std::int32_t get_i32() { return static_cast<std::int32_t>(get_u32()); }

    std::span<const std::byte> get_bytes(bool strip_nul) {
        const word len = get_u32();
        if (bad) return {};
        if (len == 0) return {};
        const std::size_t words = pad4(len) / 4;
        if (!avail(words)) { bad = true; return {}; }
        const auto* p = reinterpret_cast<const std::byte*>(body.data() + pos);
        pos += words;
        const std::size_t n = strip_nul ? (len - 1) : len;
        return {p, n};
    }

    std::string_view get_string() {
        auto b = get_bytes(true);
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    std::span<const std::byte> get_array() { return get_bytes(false); }

    int get_fd() {
        if (fd_pos >= fds.size()) { bad = true; return -1; }
        return fds[fd_pos++];
    }
};

// ---------------------------------------------------------------------------
// Per-atom codec. The *only* place a wire representation is written down.
// ---------------------------------------------------------------------------
template <class A>
struct codec;

template <>
struct codec<Int> {
    static void put(Encoder& e, std::int32_t v) { e.put_i32(v); }
    static std::int32_t get(Decoder& d) { return d.get_i32(); }
};
template <>
struct codec<Uint> {
    static void put(Encoder& e, std::uint32_t v) { e.put_u32(v); }
    static std::uint32_t get(Decoder& d) { return d.get_u32(); }
};
template <>
struct codec<Fixed> {
    static void put(Encoder& e, fixed v) { e.put_i32(v.raw); }
    static fixed get(Decoder& d) { return fixed::from_raw(d.get_i32()); }
};
template <>
struct codec<String> {
    static void put(Encoder& e, std::string_view v) { e.put_string(v); }
    static std::string_view get(Decoder& d) { return d.get_string(); }
};
template <>
struct codec<Array> {
    static void put(Encoder& e, std::span<const std::byte> v) { e.put_array(v); }
    static std::span<const std::byte> get(Decoder& d) { return d.get_array(); }
};
template <>
struct codec<Fd> {
    // Sending LENDS the descriptor: the caller keeps ownership of theirs and
    // must still close it. The message holds its own duplicate until it's
    // flushed (flush closes that copy once the kernel has it), so the caller
    // may close theirs right away, even before the flush. Without the dup
    // both sides would close the same number, and in a threaded program the
    // second close can hit a descriptor someone else just opened.
    static void put(Encoder& e, BorrowedFd fd) {
        if (!fd.valid()) {
            e.put_fd(-1);
            return;
        }
        // jaal's duplicate_handle: F_DUPFD_CLOEXEC, said once, in the one
        // place that is allowed to know what that means. A descriptor that
        // leaked across an exec would hand a client's buffer to whatever the
        // compositor spawned next.
        auto copy = jaal::platform::duplicate_handle(fd.get());
        e.put_fd(copy ? *copy : -1);
    }
    // Receiving TRANSFERS one: it arrives via SCM_RIGHTS as a new descriptor
    // in this process, and someone has to close it.
    static BorrowedFd get(Decoder& d) { return BorrowedFd(d.get_fd()); }
};
template <class E, class B>
struct codec<Enum<E, B>> {
    using U = std::underlying_type_t<E>;
    static void put(Encoder& e, E v) { e.put_u32(static_cast<word>(static_cast<U>(v))); }
    static E get(Decoder& d) { return static_cast<E>(static_cast<U>(d.get_u32())); }
};
template <class A>
struct codec<Nullable<A>> {
    static void put(Encoder& e, const std::optional<value_t<A>>& v) {
        if (v) codec<A>::put(e, *v);
        else if constexpr (std::is_same_v<A, String>) e.put_null_string();
        else e.put_u32(0);
    }
    static std::optional<decode_value_t<A>> get(Decoder& d) {
        auto v = codec<A>::get(d);
        return std::optional<decode_value_t<A>>(v);
    }
};

}  // namespace i4::wl::wire
