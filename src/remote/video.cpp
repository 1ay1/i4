// src/remote/video.cpp — FFmpeg decode, hardware first.
#include "video.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace i4::remote {

bool video_available() { return true; }

namespace {

/// The platform's hardware decoder device, opened once (it is the slow part:
/// ~100 ms to bring up CUDA / VideoToolbox) and shared by every window.
struct HwDevice {
    AVBufferRef* ref = nullptr;
    AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
};
std::once_flag g_hw_once;
HwDevice g_hw;

const HwDevice& hw_device() {
    std::call_once(g_hw_once, [] {
        if (std::getenv("I4_SW_DECODE") != nullptr) return;
        for (auto type : {AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA,
                          AV_HWDEVICE_TYPE_VAAPI})
            if (av_hwdevice_ctx_create(&g_hw.ref, type, nullptr, nullptr, 0) >= 0) {
                g_hw.type = type;
                return;
            }
    });
    return g_hw;
}

}  // namespace

void warm_up() {
    std::thread([] { (void)hw_device(); }).detach();
}

Changes diff(const Picture& a, const Picture& b) {
    Changes out;
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size() || b.rgba.empty()) {
        out.whole = true;
        return out;
    }
    constexpr int T = 64;
    const int w = b.width, h = b.height, pitch = w * 4;
    std::size_t changed_px = 0;
    for (int ty = 0; ty < h; ty += T) {
        const int th = std::min(T, h - ty);
        // Runs of changed tiles along this row of tiles become one rect.
        int run = -1;
        auto close = [&](int end_x) {
            if (run < 0) return;
            out.rects.push_back({run, ty, end_x - run, th});
            changed_px += static_cast<std::size_t>(end_x - run) * static_cast<std::size_t>(th);
            run = -1;
        };
        for (int tx = 0; tx < w; tx += T) {
            const int tw = std::min(T, w - tx);
            bool diff = false;
            for (int y = ty; y < ty + th && !diff; ++y) {
                const auto off = static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(tx) * 4;
                diff = std::memcmp(a.rgba.data() + off, b.rgba.data() + off, static_cast<std::size_t>(tw) * 4) != 0;
            }
            if (diff && run < 0) run = tx;
            if (!diff) close(tx);
        }
        close(w);
    }
    // Mostly changed (a scroll, a video): one upload of the lot is cheaper.
    if (changed_px * 2 > static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        out.rects.clear();
        out.whole = true;
        return out;
    }
    for (const auto& r : out.rects)
        for (int y = r[1]; y < r[1] + r[3]; ++y) {
            const auto* row = b.rgba.data() + static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(r[0]) * 4;
            out.packed.insert(out.packed.end(), row, row + static_cast<std::size_t>(r[2]) * 4);
        }
    return out;
}

struct Decoder::Impl {
    link::down::Codec codec{};
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw = nullptr;
    AVPixelFormat hw_format = AV_PIX_FMT_NONE;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw = av_frame_alloc();
    SwsContext* sws = nullptr;

    ~Impl() { close(); av_packet_free(&pkt); av_frame_free(&frame); av_frame_free(&sw); }
    void close() {
        avcodec_free_context(&ctx);
        av_buffer_unref(&hw);
        sws_freeContext(sws);
        sws = nullptr;
        hw_format = AV_PIX_FMT_NONE;
    }

    static AVPixelFormat pick(AVCodecContext* c, const AVPixelFormat* fmts) {
        const auto* self = static_cast<const Impl*>(c->opaque);
        for (const auto* f = fmts; *f != AV_PIX_FMT_NONE; ++f)
            if (*f == self->hw_format) return *f;
        return fmts[0];  // the hardware refused this stream: software
    }

    bool open(link::down::Codec c, std::string& name) {
        close();
        codec = c;
        const AVCodec* dec = avcodec_find_decoder(c == link::down::Codec::hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        if (dec == nullptr) return false;
        ctx = avcodec_alloc_context3(dec);
        ctx->opaque = this;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        const char* kind = "software";
        const auto& dev = hw_device();
        for (int i = 0; dev.ref != nullptr; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
            if (cfg == nullptr) break;
            if (cfg->device_type != dev.type || !(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
            hw = av_buffer_ref(dev.ref);
            hw_format = cfg->pix_fmt;
            ctx->hw_device_ctx = av_buffer_ref(hw);
            ctx->get_format = &Impl::pick;
            kind = av_hwdevice_get_type_name(dev.type);
            break;
        }
        // Software: slices in parallel, never frames (frame threads delay
        // output by one frame per thread).
        if (hw == nullptr) {
            ctx->thread_type = FF_THREAD_SLICE;
            ctx->thread_count = 0;
        }
        if (avcodec_open2(ctx, dec, nullptr) < 0) {
            close();
            return false;
        }
        name = std::string(dec->name) + " (" + kind + ")";
        return true;
    }
};

Decoder::Decoder() : p_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;

std::optional<Picture> Decoder::decode(const link::down::Video& v, std::int32_t width, std::int32_t height) {
    auto& p = *p_;
    if (p.ctx == nullptr || p.codec != v.codec) {
        if (!v.keyframe || !p.open(v.codec, name_)) return std::nullopt;
    }
    p.pkt->data = reinterpret_cast<std::uint8_t*>(const_cast<char*>(v.data.data()));
    p.pkt->size = static_cast<int>(v.data.size());
    if (v.keyframe) p.pkt->flags |= AV_PKT_FLAG_KEY;
    if (avcodec_send_packet(p.ctx, p.pkt) < 0) {
        av_packet_unref(p.pkt);
        return std::nullopt;
    }
    av_packet_unref(p.pkt);
    std::optional<Picture> out;
    static const bool trace = std::getenv("I4_HEAD_TRACE") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    while (avcodec_receive_frame(p.ctx, p.frame) == 0) {
        const auto t1 = std::chrono::steady_clock::now();
        AVFrame* f = p.frame;
        if (f->format == p.hw_format && p.hw_format != AV_PIX_FMT_NONE) {
            av_frame_unref(p.sw);
            if (av_hwframe_transfer_data(p.sw, f, 0) < 0) continue;
            f = p.sw;
        }
        if (trace)
            std::fprintf(stderr, "i4-head: stage decode %.3f ms  download %.3f ms\n", ms(t0, t1),
                         ms(t1, std::chrono::steady_clock::now()));
        const int w = std::min(width, f->width), h = std::min(height, f->height);
        if (w <= 0 || h <= 0) continue;
        // 4:4:4 coded as RGB (identity matrix): its three planes ARE G, B, R.
        auto format = static_cast<AVPixelFormat>(f->format);
        if (f->colorspace == AVCOL_SPC_RGB && format == AV_PIX_FMT_YUV444P) format = AV_PIX_FMT_GBRP;
        if (format == AV_PIX_FMT_GBRP) {
            // The common case (i4's own encoder): interleave the planes, no
            // colour maths, cropped as it goes.
            Picture pic;
            pic.width = w;
            pic.height = h;
            pic.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
            for (int y = 0; y < h; ++y) {
                const auto* g = f->data[0] + static_cast<std::ptrdiff_t>(y) * f->linesize[0];
                const auto* b = f->data[1] + static_cast<std::ptrdiff_t>(y) * f->linesize[1];
                const auto* r = f->data[2] + static_cast<std::ptrdiff_t>(y) * f->linesize[2];
                auto* o = pic.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4;
                for (int x = 0; x < w; ++x, o += 4) o[0] = r[x], o[1] = g[x], o[2] = b[x], o[3] = 255;
            }
            out = std::move(pic);
            av_frame_unref(p.frame);
            continue;
        }
        p.sws = sws_getCachedContext(p.sws, f->width, f->height, format, f->width,
                                     f->height, AV_PIX_FMT_RGBA, SWS_POINT, nullptr, nullptr, nullptr);
        if (p.sws == nullptr) continue;
        if (format != AV_PIX_FMT_GBRP)
            sws_setColorspaceDetails(p.sws, sws_getCoefficients(SWS_CS_ITU709), f->color_range == AVCOL_RANGE_JPEG,
                                     sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16, 1 << 16);
        Picture pic;
        pic.width = f->width;
        pic.height = f->height;
        pic.rgba.resize(static_cast<std::size_t>(f->width) * static_cast<std::size_t>(f->height) * 4);
        std::uint8_t* dst[4] = {pic.rgba.data(), nullptr, nullptr, nullptr};
        const int stride[4] = {f->width * 4, 0, 0, 0};
        sws_scale(p.sws, f->data, f->linesize, 0, f->height, dst, stride);
        if (pic.width != width || pic.height != height) {
            // Encoders pad to even sizes: crop back to the window.
            Picture crop;
            crop.width = w;
            crop.height = h;
            crop.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
            for (int y = 0; y < h; ++y)
                std::memcpy(crop.rgba.data() + static_cast<std::size_t>(y) * w * 4,
                            pic.rgba.data() + static_cast<std::size_t>(y) * pic.width * 4, static_cast<std::size_t>(w) * 4);
            pic = std::move(crop);
        }
        out = std::move(pic);
        av_frame_unref(p.frame);
    }
    return out;
}

}  // namespace i4::remote
