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

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    std::string name_;
};

/// FFmpeg is linked in: this head can take video.
[[nodiscard]] bool video_available();

}  // namespace i4::remote
