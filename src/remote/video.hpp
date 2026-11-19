#pragma once
// src/remote/video.hpp — decoding down::Video on a head, on any OS.
//
// FFmpeg's decoders with the platform's hardware behind them, tried in
// order: VideoToolbox (macOS), D3D11VA (Windows), CUDA/NVDEC and VAAPI
// (Linux); software if none opens. Low-delay: one packet in, its frame out,
// no reordering queue.
//
// The decoded picture comes back as straight RGBA, the layout a renderer
// uploads as it is. (Next: hand the hardware surface to the renderer
// directly, without the trip through CPU memory.)

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "i4/link/protocol.hpp"

namespace i4::remote {

struct Picture {
    std::int32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

/// What changed between two same-size RGBA pictures, as rectangles (64 px
/// tiles merged along rows), with their pixels packed rect after rect. A
/// head uploads only these: a keystroke on a 4K window is a few KB, not
/// 40 MB across PCIe. Empty `rects` with `whole`: nothing changed.
struct Changes {
    std::vector<std::array<int, 4>> rects;
    std::vector<std::uint8_t> packed;
    bool whole = false;  // a different size, or most of it changed: upload it all
};
[[nodiscard]] Changes diff(const Picture& before, const Picture& after);

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// One frame's packet; its picture (cropped to width x height), or
    /// nullopt if it did not decode (a delta without its keyframe).
    [[nodiscard]] std::optional<Picture> decode(const link::down::Video& v, std::int32_t width, std::int32_t height);
    /// Which decoder is in use ("hevc (videotoolbox)", "h264 (software)"), once open.
    [[nodiscard]] const std::string& name() const { return name_; }
    /// Frames have come out, and none from the hardware: the platform's
    /// decoder refused this stream (an M1 cannot do HEVC 4:4:4) and FFmpeg
    /// fell back to software, whatever name() says it opened.
    [[nodiscard]] bool software() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::string name_;
};

/// FFmpeg is linked in: this head can take video.
[[nodiscard]] bool video_available();
/// Bring the hardware decoder up now, off the loop, so the first frame
/// does not wait ~100 ms for it.
void warm_up();

}  // namespace i4::remote
