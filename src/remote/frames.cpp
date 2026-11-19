// src/remote/frames.cpp — fragments, XOR parity, rebuild.
#include "frames.hpp"

#include <algorithm>
#include <cstring>

namespace i4::remote {

namespace {

void put16(std::byte* p, std::uint16_t v) {
    p[0] = static_cast<std::byte>(v), p[1] = static_cast<std::byte>(v >> 8);
}
void put32(std::byte* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>(v >> (8 * i));
}
std::uint16_t get16(const std::byte* p) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(p[0]) | (std::to_integer<unsigned>(p[1]) << 8));
}
std::uint32_t get32(const std::byte* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::to_integer<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

void write_header(std::byte* p, const FragmentHeader& h) {
    put32(p, h.frame);
    put32(p + 4, h.window);
    put16(p + 8, h.index);
    put16(p + 10, h.data_count);
    put32(p + 12, h.total);
    put16(p + 16, h.group);
    put16(p + 18, h.chunk);
}

std::optional<FragmentHeader> read_header(std::span<const std::byte> f) {
    if (f.size() < FragmentHeader::size) return std::nullopt;
    FragmentHeader h;
    h.frame = get32(f.data());
    h.window = get32(f.data() + 4);
    h.index = get16(f.data() + 8);
    h.data_count = get16(f.data() + 10);
    h.total = get32(f.data() + 12);
    h.group = get16(f.data() + 16);
    h.chunk = get16(f.data() + 18);
    if (h.data_count == 0 || h.group == 0 || h.chunk == 0) return std::nullopt;
    if (static_cast<std::uint64_t>(h.data_count) * h.chunk < h.total) return std::nullopt;
    if (static_cast<std::uint64_t>(h.data_count - 1) * h.chunk >= std::max<std::uint32_t>(h.total, 1)) return std::nullopt;
    const std::uint32_t parity = (h.data_count + h.group - 1u) / h.group;
    if (h.index >= h.data_count + parity) return std::nullopt;
    return h;
}

/// Bytes of data fragment `i`.
std::size_t length(const FragmentHeader& h, std::uint32_t i) {
    return i + 1 < h.data_count ? h.chunk : h.total - static_cast<std::size_t>(h.data_count - 1) * h.chunk;
}

}  // namespace

std::vector<std::vector<std::byte>> fragment(std::uint32_t frame, std::uint32_t window, std::span<const std::byte> bytes,
                                             std::size_t chunk, std::size_t group) {
    FragmentHeader h;
    h.frame = frame;
    h.window = window;
    h.total = static_cast<std::uint32_t>(bytes.size());
    h.chunk = static_cast<std::uint16_t>(std::max<std::size_t>(1, std::min(chunk, bytes.size())));
    h.data_count = static_cast<std::uint16_t>(std::max<std::size_t>(1, (bytes.size() + h.chunk - 1) / h.chunk));
    h.group = static_cast<std::uint16_t>(group);
    std::vector<std::vector<std::byte>> out;
    const std::size_t groups = (h.data_count + group - 1) / group;
    std::vector<std::vector<std::byte>> parity(groups, std::vector<std::byte>(h.chunk, std::byte{0}));
    for (std::uint32_t i = 0; i < h.data_count; ++i) {
        const auto off = static_cast<std::size_t>(i) * h.chunk;
        const auto n = length(h, i);
        std::vector<std::byte> f(FragmentHeader::size + n);
        h.index = static_cast<std::uint16_t>(i);
        write_header(f.data(), h);
        std::memcpy(f.data() + FragmentHeader::size, bytes.data() + off, n);
        auto& p = parity[i / group];
        for (std::size_t k = 0; k < n; ++k) p[k] ^= bytes[off + k];
        out.push_back(std::move(f));
    }
    for (std::size_t g = 0; g < groups; ++g) {
        std::vector<std::byte> f(FragmentHeader::size + h.chunk);
        h.index = static_cast<std::uint16_t>(h.data_count + g);
        write_header(f.data(), h);
        std::memcpy(f.data() + FragmentHeader::size, parity[g].data(), h.chunk);
        // A one-fragment frame (a keystroke, a few hundred bytes) goes three
        // times: losing all three copies is rare even on a bad link.
        if (h.data_count == 1) out.push_back(f);
        out.push_back(std::move(f));
    }
    return out;
}

std::optional<Reassembly::Frame> Reassembly::add(std::span<const std::byte> fragment) {
    const auto h = read_header(fragment);
    if (!h || h->frame < done_below_ || finished_.contains(h->frame)) return std::nullopt;
    if (!newest_ || h->frame > *newest_) newest_ = h->frame;
    auto& p = partial_[h->frame];
    if (p.pieces.empty()) p.h = *h;
    // Every fragment of a frame must agree on its shape.
    if (p.h.data_count != h->data_count || p.h.total != h->total || p.h.chunk != h->chunk || p.h.group != h->group ||
        p.h.window != h->window)
        return std::nullopt;
    const auto payload = fragment.subspan(FragmentHeader::size);
    const auto want = h->index < h->data_count ? length(*h, h->index) : h->chunk;
    if (payload.size() != want) return std::nullopt;
    p.pieces.emplace(h->index, std::vector<std::byte>(payload.begin(), payload.end()));
    p.last = std::chrono::steady_clock::now();
    return try_complete(p);
}

std::optional<Reassembly::Frame> Reassembly::try_complete(Partial& p) {
    const auto& h = p.h;
    // Rebuild what parity can: one missing data fragment per group.
    for (std::uint32_t g = 0; g * h.group < h.data_count; ++g) {
        const std::uint32_t first = g * h.group, last = std::min<std::uint32_t>(h.data_count, first + h.group);
        std::uint32_t missing = 0, which = 0;
        for (auto i = first; i < last; ++i)
            if (!p.pieces.contains(static_cast<std::uint16_t>(i))) ++missing, which = i;
        if (missing != 1) continue;
        auto par = p.pieces.find(static_cast<std::uint16_t>(h.data_count + g));
        if (par == p.pieces.end()) continue;
        std::vector<std::byte> rebuilt = par->second;
        for (auto i = first; i < last; ++i) {
            if (i == which) continue;
            const auto& d = p.pieces[static_cast<std::uint16_t>(i)];
            for (std::size_t k = 0; k < d.size(); ++k) rebuilt[k] ^= d[k];
        }
        rebuilt.resize(length(h, which));
        p.pieces.emplace(static_cast<std::uint16_t>(which), std::move(rebuilt));
    }
    for (std::uint32_t i = 0; i < h.data_count; ++i)
        if (!p.pieces.contains(static_cast<std::uint16_t>(i))) return std::nullopt;
    Frame f{h.frame, h.window, {}};
    f.bytes.reserve(h.total);
    for (std::uint32_t i = 0; i < h.data_count; ++i) {
        const auto& d = p.pieces[static_cast<std::uint16_t>(i)];
        f.bytes.insert(f.bytes.end(), d.begin(), d.end());
    }
    finished_[h.frame] = true;
    partial_.erase(h.frame);
    return f;
}

Reassembly::Lost Reassembly::expire_quiet(std::chrono::steady_clock::time_point now, std::chrono::microseconds quiet) {
    Lost out;
    for (auto it = partial_.begin(); it != partial_.end();) {
        if (now - it->second.last < quiet) {
            ++it;
            continue;
        }
        if (std::ranges::find(out.windows, it->second.h.window) == out.windows.end())
            out.windows.push_back(it->second.h.window);
        finished_[it->first] = true;  // reported: never again
        it = partial_.erase(it);
    }
    return out;
}

Reassembly::Lost Reassembly::expire(std::uint32_t slack) {
    Lost out;
    if (!newest_ || *newest_ < slack) return out;
    const std::uint32_t horizon = *newest_ - slack;  // frames below this are given up on
    for (; done_below_ < horizon; ++done_below_) {
        if (finished_.erase(done_below_) != 0) continue;
        if (auto it = partial_.find(done_below_); it != partial_.end()) {
            if (std::ranges::find(out.windows, it->second.h.window) == out.windows.end())
                out.windows.push_back(it->second.h.window);
            partial_.erase(it);
        } else {
            ++out.unknown;
        }
    }
    return out;
}

}  // namespace i4::remote
