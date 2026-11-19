#pragma once
// src/server/encoder.hpp — client GPU buffers to video, on the GPU.
//
// For heads across a network. A client's dmabuf (any modifier this GPU can
// sample, NVIDIA's tiled ones included) is imported into GL, a shader splits
// it into G, B, R planes, CUDA hands those to NVENC, and out comes HEVC
// 4:4:4 RGB, LOSSLESS, one access unit per frame (no reordering, no
// lookahead). The pixels never leave video memory: no CPU copy on this
// side, and the head decodes in hardware on its side.
//
// One Stream per (head, window): a new head, a resize, or a decoder that
// lost its place gets a keyframe; everything else is a delta (a cursor
// blink in a terminal is a few hundred bytes).
//
// Everything is loaded at run time (EGL, GLES, libcuda through FFmpeg); on
// a machine without NVENC, available() is false and remote heads get tiles.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "i4/link/protocol.hpp"

namespace i4::server {

struct DmabufFrame {
    std::uint64_t buffer;  // stable per client buffer: its GL import is cached
    std::int32_t width, height;
    std::uint32_t fourcc;
    std::uint64_t modifier;
    struct Plane {
        int fd;
        std::uint32_t offset, stride;
    };
    std::vector<Plane> planes;
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    [[nodiscard]] bool available() const;
    /// Why not, when it is not (for the log).
    [[nodiscard]] const std::string& why() const;
    /// The client buffer layouts this GPU can import (for linux-dmabuf).
    [[nodiscard]] std::vector<link::up::Format> formats() const;

    class Stream;
    struct StreamFree {
        void operator()(Stream* s) const;
    };
    using StreamPtr = std::unique_ptr<Stream, StreamFree>;
    /// A new stream (its first frame is a keyframe).
    [[nodiscard]] StreamPtr stream();
    /// One frame of `s`; nullopt if it could not be imported or encoded.
    [[nodiscard]] std::optional<link::down::Video> encode(Stream& s, const DmabufFrame& f, bool keyframe = false);
    /// The client destroyed this buffer: drop its import.
    void forget(std::uint64_t buffer);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace i4::server
