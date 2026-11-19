// src/remote/video.cpp — FFmpeg decode, hardware first.
#include "video.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace i4::remote {

bool video_available() { return true; }

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
        for (auto type : {AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_CUDA,
                          AV_HWDEVICE_TYPE_VAAPI}) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(dec, i);
                if (cfg == nullptr) break;
                if (cfg->device_type != type || !(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) continue;
                if (av_hwdevice_ctx_create(&hw, type, nullptr, nullptr, 0) < 0) break;
                hw_format = cfg->pix_fmt;
                ctx->hw_device_ctx = av_buffer_ref(hw);
                ctx->get_format = &Impl::pick;
                kind = av_hwdevice_get_type_name(type);
                break;
            }
            if (hw != nullptr) break;
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
    while (avcodec_receive_frame(p.ctx, p.frame) == 0) {
        AVFrame* f = p.frame;
        if (f->format == p.hw_format && p.hw_format != AV_PIX_FMT_NONE) {
            av_frame_unref(p.sw);
            if (av_hwframe_transfer_data(p.sw, f, 0) < 0) continue;
            f = p.sw;
        }
        const int w = std::min(width, f->width), h = std::min(height, f->height);
        if (w <= 0 || h <= 0) continue;
        // 4:4:4 coded as RGB (identity matrix): its three planes ARE G, B, R.
        auto format = static_cast<AVPixelFormat>(f->format);
        if (f->colorspace == AVCOL_SPC_RGB && format == AV_PIX_FMT_YUV444P) format = AV_PIX_FMT_GBRP;
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
