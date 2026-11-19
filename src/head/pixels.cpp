// src/head/pixels.cpp — swizzle and un-premultiply.
#include "pixels.hpp"

namespace i4::head {

std::optional<mayag::ImageData> convert(std::span<const std::byte> memory, Layout l) {
    if (l.width <= 0 || l.height <= 0 || l.offset < 0 || l.stride < l.width * 4) return std::nullopt;
    const auto end = static_cast<std::uint64_t>(l.offset) +
                     static_cast<std::uint64_t>(l.stride) * static_cast<std::uint64_t>(l.height - 1) +
                     static_cast<std::uint64_t>(l.width) * 4;
    if (end > memory.size()) return std::nullopt;

    mayag::ImageData img;
    img.width = l.width;
    img.height = l.height;
    img.rgba.resize(static_cast<std::size_t>(l.width) * static_cast<std::size_t>(l.height) * 4);
    auto* out = img.rgba.data();
    for (std::int32_t y = 0; y < l.height; ++y) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(memory.data()) + l.offset +
                          static_cast<std::ptrdiff_t>(y) * l.stride;
        for (std::int32_t x = 0; x < l.width; ++x, out += 4) {
            const std::uint8_t b = row[x * 4 + 0], g = row[x * 4 + 1], r = row[x * 4 + 2];
            const std::uint8_t a = l.alpha ? row[x * 4 + 3] : 255;
            if (a == 255 || a == 0) {
                out[0] = r, out[1] = g, out[2] = b;
            } else {
                auto un = [a](std::uint8_t c) {
                    return static_cast<std::uint8_t>(std::min(255u, (c * 255u + a / 2u) / a));
                };
                out[0] = un(r), out[1] = un(g), out[2] = un(b);
            }
            out[3] = a;
        }
    }
    return img;
}

}  // namespace i4::head
