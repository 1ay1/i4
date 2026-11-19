// src/server/encoder.cpp — dmabuf -> EGLImage -> GL planes -> CUDA -> NVENC.
#include "encoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>

#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

// The CUDA driver API, the few calls used, declared here and loaded from
// libcuda at run time: no CUDA SDK needed to build i4.
#define CUDA_VERSION 12000
using CUcontext = struct CUctx_st*;
using CUstream = struct CUstream_st*;
using CUarray = struct CUarray_st*;
using CUgraphicsResource = struct CUgraphicsResource_st*;
using CUdeviceptr = unsigned long long;
using CUresult = int;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

namespace i4::server {

namespace {

struct CUDA_MEMCPY2D {
    std::size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    std::size_t srcPitch;
    std::size_t dstXInBytes, dstY;
    int dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    std::size_t dstPitch;
    std::size_t WidthInBytes, Height;
};
constexpr int CU_MEMORYTYPE_DEVICE = 2, CU_MEMORYTYPE_ARRAY = 3;
constexpr unsigned CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY = 1;

struct Cuda {
    void* lib = nullptr;
    CUresult (*CtxPushCurrent)(CUcontext) = nullptr;
    CUresult (*CtxPopCurrent)(CUcontext*) = nullptr;
    CUresult (*GraphicsGLRegisterImage)(CUgraphicsResource*, GLuint, GLenum, unsigned) = nullptr;
    CUresult (*GraphicsUnregisterResource)(CUgraphicsResource) = nullptr;
    CUresult (*GraphicsMapResources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*GraphicsUnmapResources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*GraphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, unsigned, unsigned) = nullptr;
    CUresult (*Memcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream) = nullptr;
    CUresult (*StreamSynchronize)(CUstream) = nullptr;

    bool load() {
        lib = ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) return false;
        auto sym = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(::dlsym(lib, name));
            return f != nullptr;
        };
        return sym(CtxPushCurrent, "cuCtxPushCurrent_v2") && sym(CtxPopCurrent, "cuCtxPopCurrent_v2") &&
               sym(GraphicsGLRegisterImage, "cuGraphicsGLRegisterImage") &&
               sym(GraphicsUnregisterResource, "cuGraphicsUnregisterResource") &&
               sym(GraphicsMapResources, "cuGraphicsMapResources") &&
               sym(GraphicsUnmapResources, "cuGraphicsUnmapResources") &&
               sym(GraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray") &&
               sym(Memcpy2DAsync, "cuMemcpy2DAsync_v2") && sym(StreamSynchronize, "cuStreamSynchronize");
    }
};

constexpr const char* kVertex = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";

// The client's pixels as three stacked planes, G, B, R (FFmpeg's gbrp
// order), each `size.y` rows: what NVENC takes as 4:4:4 RGB. Row r of each
// plane is row r of the buffer in memory: no flip anywhere.
constexpr const char* kFragment = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES src;
uniform vec2 size;
out vec4 color;
void main() {
    float row = gl_FragCoord.y;
    float plane = floor(row / size.y);
    vec2 uv = vec2(gl_FragCoord.x, row - plane * size.y) / size;
    vec3 c = texture(src, uv).rgb;
    float v = plane < 0.5 ? c.g : (plane < 1.5 ? c.b : c.r);
    color = vec4(v, 0.0, 0.0, 1.0);
})";

// A straight copy, swizzled to the bytes Wayland's ARGB8888 has in memory
// (B, G, R, A), so glReadPixels(GL_RGBA) hands back exactly that.
constexpr const char* kCopy = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES src;
uniform vec2 size;
out vec4 color;
void main() { color = texture(src, gl_FragCoord.xy / size).bgra; }
)";

// One fragment per tile: does any pixel of it differ from the previous
// frame? The whole comparison stays on the GPU.
constexpr const char* kDiff = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
precision highp int;
uniform samplerExternalOES src;
uniform sampler2D prev;
uniform ivec2 size;
uniform int tile;
out vec4 color;
void main() {
    ivec2 o = ivec2(gl_FragCoord.xy) * tile;
    ivec2 e = min(o + tile, size);
    vec2 inv = 1.0 / vec2(size);
    for (int y = o.y; y < e.y; ++y)
        for (int x = o.x; x < e.x; ++x)
            if (any(greaterThan(abs(texture(src, (vec2(x, y) + 0.5) * inv) - texelFetch(prev, ivec2(x, y), 0)),
                                vec4(0.5 / 255.0)))) {
                color = vec4(1.0);
                return;
            }
    color = vec4(0.0);
}
)";

// The frame as it is, to compare the next one against.
constexpr const char* kKeep = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES src;
uniform vec2 size;
out vec4 color;
void main() { color = texture(src, gl_FragCoord.xy / size); }
)";

}  // namespace

// -- a stream: one NVENC session ------------------------------------------------------

class Encoder::Stream {
public:
    ~Stream() {
        avcodec_free_context(&ctx);
        av_buffer_unref(&frames);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        if (prev != 0) glDeleteTextures(1, &prev);
        if (prev_fbo != 0) glDeleteFramebuffers(1, &prev_fbo);
    }
    /// The last frame seen (RGBA8), for the GPU tile diff.
    GLuint prev = 0, prev_fbo = 0;
    std::int32_t prev_w = 0, prev_h = 0;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* frames = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    std::int32_t width = 0, height = 0;
    std::int64_t pts = 0;
};

struct Encoder::Impl {
    std::string why;
    Cuda cu;
    AVBufferRef* device = nullptr;
    AVCUDADeviceContext* cuda = nullptr;

    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext egl = EGL_NO_CONTEXT;
    PFNEGLCREATEIMAGEKHRPROC CreateImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC DestroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTexture = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC QueryFormats = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC QueryModifiers = nullptr;
    GLuint program = 0, fbo = 0, vao = 0;
    GLint u_size = -1;
    GLuint copy_program = 0, copy_fbo = 0, copy_tex = 0;
    GLint u_copy_size = -1;
    std::int32_t cw = 0, ch = 0;
    GLuint diff_program = 0, keep_program = 0, flags_fbo = 0, flags_tex = 0;
    GLint u_diff_size = -1, u_diff_tile = -1, u_keep_size = -1;
    std::int32_t fw = 0, fh = 0;

    /// The planes texture (W x 3H, R8), registered with CUDA, by size.
    GLuint planes = 0;
    CUgraphicsResource planes_cu = nullptr;
    std::int32_t pw = 0, ph = 0;

    /// Client buffers, imported once: most recently used first.
    struct Import {
        std::uint64_t buffer;
        EGLImageKHR image;
        GLuint texture;
    };
    std::list<Import> imports;
    static constexpr std::size_t kImports = 24;

    bool ok = false;

    Impl() { ok = init(); }
    ~Impl() {
        if (egl != EGL_NO_CONTEXT) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl);
        for (auto& i : imports) drop(i);
        if (planes_cu != nullptr && cuda != nullptr) {
            cu.CtxPushCurrent(cuda->cuda_ctx);
            cu.GraphicsUnregisterResource(planes_cu);
            CUcontext dummy;
            cu.CtxPopCurrent(&dummy);
        }
        if (egl != EGL_NO_CONTEXT) eglDestroyContext(dpy, egl);
        av_buffer_unref(&device);
    }

    bool fail(const char* what) {
        why = what;
        return false;
    }

    bool init() {
        if (!cu.load()) return fail("no libcuda (not an NVIDIA GPU)");
        if (avcodec_find_encoder_by_name("hevc_nvenc") == nullptr) return fail("FFmpeg has no hevc_nvenc");
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0)
            return fail("no CUDA device");
        cuda = static_cast<AVCUDADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(device->data)->hwctx);

        // EGL on the GPU itself, no window system: EGL_EXT_platform_device.
        auto QueryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
        auto GetPlatformDisplay =
            reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (QueryDevices == nullptr || GetPlatformDisplay == nullptr) return fail("EGL has no device platform");
        EGLDeviceEXT devs[8];
        EGLint n = 0;
        if (!QueryDevices(8, devs, &n) || n == 0) return fail("no EGL device");
        for (EGLint i = 0; i < n && dpy == EGL_NO_DISPLAY; ++i) {
            EGLDisplay d = GetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr);
            if (d != EGL_NO_DISPLAY && eglInitialize(d, nullptr, nullptr)) dpy = d;
        }
        if (dpy == EGL_NO_DISPLAY) return fail("EGL would not initialise");
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
        egl = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (egl == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl))
            return fail("no surfaceless GLES 3 context");
        CreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        DestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        ImageTargetTexture =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        QueryFormats = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        QueryModifiers =
            reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        if (!CreateImage || !DestroyImage || !ImageTargetTexture) return fail("EGL cannot import dmabufs");

        auto shader = [](GLenum kind, const char* src) {
            const GLuint s = glCreateShader(kind);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint good = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &good);
            if (!good) {
                char log[512];
                glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                std::fprintf(stderr, "i4d: encoder shader: %s\n", log);
            }
            return s;
        };
        program = glCreateProgram();
        glAttachShader(program, shader(GL_VERTEX_SHADER, kVertex));
        glAttachShader(program, shader(GL_FRAGMENT_SHADER, kFragment));
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) return fail("encoder shader did not link");
        glUseProgram(program);
        glUniform1i(glGetUniformLocation(program, "src"), 0);
        u_size = glGetUniformLocation(program, "size");
        glGenFramebuffers(1, &fbo);
        glGenVertexArrays(1, &vao);
        copy_program = glCreateProgram();
        glAttachShader(copy_program, shader(GL_VERTEX_SHADER, kVertex));
        glAttachShader(copy_program, shader(GL_FRAGMENT_SHADER, kCopy));
        glLinkProgram(copy_program);
        glGetProgramiv(copy_program, GL_LINK_STATUS, &linked);
        if (!linked) return fail("readback shader did not link");
        glUseProgram(copy_program);
        glUniform1i(glGetUniformLocation(copy_program, "src"), 0);
        u_copy_size = glGetUniformLocation(copy_program, "size");
        glGenFramebuffers(1, &copy_fbo);
        auto link_program = [&](const char* frag) -> GLuint {
            const GLuint p = glCreateProgram();
            glAttachShader(p, shader(GL_VERTEX_SHADER, kVertex));
            glAttachShader(p, shader(GL_FRAGMENT_SHADER, frag));
            glLinkProgram(p);
            GLint good = 0;
            glGetProgramiv(p, GL_LINK_STATUS, &good);
            return good ? p : 0;
        };
        diff_program = link_program(kDiff);
        keep_program = link_program(kKeep);
        if (diff_program == 0 || keep_program == 0) return fail("diff shaders did not link");
        glUseProgram(diff_program);
        glUniform1i(glGetUniformLocation(diff_program, "src"), 0);
        glUniform1i(glGetUniformLocation(diff_program, "prev"), 1);
        u_diff_size = glGetUniformLocation(diff_program, "size");
        u_diff_tile = glGetUniformLocation(diff_program, "tile");
        glUseProgram(keep_program);
        glUniform1i(glGetUniformLocation(keep_program, "src"), 0);
        u_keep_size = glGetUniformLocation(keep_program, "size");
        glGenFramebuffers(1, &flags_fbo);
        return true;
    }

    void drop(Import& i) {
        glDeleteTextures(1, &i.texture);
        DestroyImage(dpy, i.image);
    }

    GLuint import(const DmabufFrame& f) {
        if (auto it = std::ranges::find(imports, f.buffer, &Import::buffer); it != imports.end()) {
            imports.splice(imports.begin(), imports, it);
            return it->texture;
        }
        if (f.planes.empty() || f.planes.size() > 4) return 0;
        std::vector<EGLAttrib> a = {EGL_WIDTH, f.width, EGL_HEIGHT, f.height, EGL_LINUX_DRM_FOURCC_EXT,
                                    static_cast<EGLAttrib>(f.fourcc)};
        const EGLint keys[4][5] = {
            {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
             EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
             EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
             EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
            {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
             EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}};
        for (std::size_t i = 0; i < f.planes.size(); ++i) {
            a.insert(a.end(), {keys[i][0], f.planes[i].fd, keys[i][1], static_cast<EGLAttrib>(f.planes[i].offset),
                               keys[i][2], static_cast<EGLAttrib>(f.planes[i].stride)});
            if (f.modifier != 0x00ffffffffffffffULL)  // DRM_FORMAT_MOD_INVALID: implicit
                a.insert(a.end(), {keys[i][3], static_cast<EGLAttrib>(f.modifier & 0xffffffffu), keys[i][4],
                                   static_cast<EGLAttrib>(f.modifier >> 32)});
        }
        // eglCreateImageKHR takes EGLint attributes.
        std::vector<EGLint> ai(a.begin(), a.end());
        ai.push_back(EGL_NONE);
        EGLImageKHR image = CreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, ai.data());
        if (image == EGL_NO_IMAGE_KHR) return 0;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        ImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &tex);
            DestroyImage(dpy, image);
            return 0;
        }
        imports.push_front({f.buffer, image, tex});
        while (imports.size() > kImports) {
            drop(imports.back());
            imports.pop_back();
        }
        return tex;
    }

    bool size_planes(std::int32_t w, std::int32_t h) {
        if (w == pw && h == ph && planes != 0) return true;
        if (planes_cu != nullptr) {
            cu.GraphicsUnregisterResource(planes_cu);
            planes_cu = nullptr;
        }
        if (planes != 0) glDeleteTextures(1, &planes);
        glGenTextures(1, &planes);
        glBindTexture(GL_TEXTURE_2D, planes);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, w, h * 3);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, planes, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
        if (cu.GraphicsGLRegisterImage(&planes_cu, planes, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY) != 0)
            return false;
        pw = w;
        ph = h;
        return true;
    }

    bool open(Stream& s, std::int32_t w, std::int32_t h) {
        avcodec_free_context(&s.ctx);
        av_buffer_unref(&s.frames);
        s.frames = av_hwframe_ctx_alloc(device);
        auto* fc = reinterpret_cast<AVHWFramesContext*>(s.frames->data);
        fc->format = AV_PIX_FMT_CUDA;
        fc->sw_format = AV_PIX_FMT_YUV444P;  // G, B, R planes; the stream says RGB (identity matrix)
        fc->width = w;
        fc->height = h;
        fc->initial_pool_size = 2;
        if (av_hwframe_ctx_init(s.frames) < 0) return false;
        const AVCodec* codec = avcodec_find_encoder_by_name("hevc_nvenc");
        s.ctx = avcodec_alloc_context3(codec);
        s.ctx->width = w;
        s.ctx->height = h;
        s.ctx->pix_fmt = AV_PIX_FMT_CUDA;
        s.ctx->sw_pix_fmt = AV_PIX_FMT_YUV444P;
        s.ctx->hw_frames_ctx = av_buffer_ref(s.frames);
        s.ctx->time_base = {1, 1000};
        s.ctx->framerate = {240, 1};
        s.ctx->gop_size = 1 << 30;  // keyframes only when asked for
        s.ctx->max_b_frames = 0;
        s.ctx->colorspace = AVCOL_SPC_RGB;
        s.ctx->color_range = AVCOL_RANGE_JPEG;
        s.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        av_opt_set(s.ctx->priv_data, "preset", "p1", 0);
        av_opt_set(s.ctx->priv_data, "tune", "lossless", 0);
        av_opt_set_int(s.ctx->priv_data, "zerolatency", 1, 0);
        av_opt_set_int(s.ctx->priv_data, "delay", 0, 0);
        av_opt_set_int(s.ctx->priv_data, "forced-idr", 1, 0);
        av_opt_set(s.ctx->priv_data, "profile", "rext", 0);
        if (avcodec_open2(s.ctx, codec, nullptr) < 0) {
            avcodec_free_context(&s.ctx);
            return false;
        }
        s.width = w;
        s.height = h;
        return true;
    }

    std::optional<link::down::Video> encode(Stream& s, const DmabufFrame& f, bool keyframe) {
        if (!ok || f.width <= 0 || f.height <= 0 || f.width > 8192 || f.height > 8192) return std::nullopt;
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl);
        const GLuint src = import(f);
        if (src == 0) return std::nullopt;
        if (cu.CtxPushCurrent(cuda->cuda_ctx) != 0) return std::nullopt;
        struct Pop {
            Cuda& cu;
            ~Pop() {
                CUcontext c;
                cu.CtxPopCurrent(&c);
            }
        } pop{cu};
        if (!size_planes(f.width, f.height)) return std::nullopt;
        if (s.ctx == nullptr || s.width != f.width || s.height != f.height) {
            if (!open(s, f.width, f.height)) return std::nullopt;
            keyframe = true;
        }

        // The planes, on the GPU.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, f.width, f.height * 3);
        glUseProgram(program);
        glUniform2f(u_size, static_cast<float>(f.width), static_cast<float>(f.height));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();

        // GL -> CUDA (mapping waits for the draw) -> the encoder's frame.
        av_frame_unref(s.frame);
        if (av_hwframe_get_buffer(s.frames, s.frame, 0) < 0) return std::nullopt;
        if (cu.GraphicsMapResources(1, &planes_cu, cuda->stream) != 0) return std::nullopt;
        CUarray array = nullptr;
        bool copied = cu.GraphicsSubResourceGetMappedArray(&array, planes_cu, 0, 0) == 0;
        for (int plane = 0; copied && plane < 3; ++plane) {
            CUDA_MEMCPY2D c{};
            c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            c.srcArray = array;
            c.srcY = static_cast<std::size_t>(plane) * static_cast<std::size_t>(f.height);
            c.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            c.dstDevice = reinterpret_cast<CUdeviceptr>(s.frame->data[plane]);
            c.dstPitch = static_cast<std::size_t>(s.frame->linesize[plane]);
            c.WidthInBytes = static_cast<std::size_t>(f.width);
            c.Height = static_cast<std::size_t>(f.height);
            copied = cu.Memcpy2DAsync(&c, cuda->stream) == 0;
        }
        cu.GraphicsUnmapResources(1, &planes_cu, cuda->stream);
        if (!copied) return std::nullopt;

        s.frame->pts = s.pts++;
        s.frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (keyframe) s.frame->flags |= AV_FRAME_FLAG_KEY;
        else s.frame->flags &= ~AV_FRAME_FLAG_KEY;
        if (avcodec_send_frame(s.ctx, s.frame) < 0) return std::nullopt;
        link::down::Video out;
        out.codec = link::down::Codec::hevc;
        while (avcodec_receive_packet(s.ctx, s.pkt) == 0) {
            out.data.append(reinterpret_cast<const char*>(s.pkt->data), static_cast<std::size_t>(s.pkt->size));
            out.keyframe = out.keyframe || (s.pkt->flags & AV_PKT_FLAG_KEY) != 0;
            av_packet_unref(s.pkt);
        }
        if (out.data.empty()) return std::nullopt;
        return out;
    }

    std::optional<std::vector<std::byte>> read(const DmabufFrame& f, const std::vector<std::array<int, 4>>& rects) {
        if (!ok || f.width <= 0 || f.height <= 0) return std::nullopt;
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl);
        const GLuint src = import(f);
        if (src == 0) return std::nullopt;
        if (cw != f.width || ch != f.height) {
            if (copy_tex != 0) glDeleteTextures(1, &copy_tex);
            glGenTextures(1, &copy_tex);
            glBindTexture(GL_TEXTURE_2D, copy_tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, f.width, f.height);
            glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_tex, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return std::nullopt;
            cw = f.width;
            ch = f.height;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, copy_fbo);
        glViewport(0, 0, f.width, f.height);
        glUseProgram(copy_program);
        glUniform2f(u_copy_size, static_cast<float>(f.width), static_cast<float>(f.height));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
        glBindVertexArray(vao);
        // Only the damaged rectangles are drawn, then read.
        glEnable(GL_SCISSOR_TEST);
        for (const auto& r : rects) {
            glScissor(r[0], r[1], r[2], r[3]);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_SCISSOR_TEST);
        std::size_t total = 0;
        for (const auto& r : rects) total += static_cast<std::size_t>(r[2]) * static_cast<std::size_t>(r[3]) * 4;
        std::vector<std::byte> out(total);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        std::size_t at = 0;
        for (const auto& r : rects) {
            glReadPixels(r[0], r[1], r[2], r[3], GL_RGBA, GL_UNSIGNED_BYTE, out.data() + at);
            at += static_cast<std::size_t>(r[2]) * static_cast<std::size_t>(r[3]) * 4;
        }
        if (glGetError() != GL_NO_ERROR) return std::nullopt;
        return out;
    }

    std::optional<std::vector<std::uint8_t>> changes(Stream& s, const DmabufFrame& f) {
        constexpr int T = 64;
        if (!ok || f.width <= 0 || f.height <= 0) return std::nullopt;
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl);
        const GLuint src = import(f);
        if (src == 0) return std::nullopt;
        const int cols = (f.width + T - 1) / T, rows = (f.height + T - 1) / T;
        std::optional<std::vector<std::uint8_t>> out;
        glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
        if (s.prev != 0 && s.prev_w == f.width && s.prev_h == f.height) {
            if (fw != cols || fh != rows) {
                if (flags_tex != 0) glDeleteTextures(1, &flags_tex);
                glGenTextures(1, &flags_tex);
                glBindTexture(GL_TEXTURE_2D, flags_tex);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, cols, rows);
                glBindFramebuffer(GL_FRAMEBUFFER, flags_fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, flags_tex, 0);
                fw = cols;
                fh = rows;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, flags_fbo);
            glViewport(0, 0, cols, rows);
            glUseProgram(diff_program);
            glUniform2i(u_diff_size, f.width, f.height);
            glUniform1i(u_diff_tile, T);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, s.prev);
            glActiveTexture(GL_TEXTURE0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows) * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, cols, rows, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            std::vector<std::uint8_t> flags(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
            for (std::size_t i = 0; i < flags.size(); ++i) flags[i] = rgba[i * 4] != 0;
            out = std::move(flags);
        }
        // Remember this frame for the next comparison.
        if (s.prev == 0 || s.prev_w != f.width || s.prev_h != f.height) {
            if (s.prev != 0) glDeleteTextures(1, &s.prev);
            if (s.prev_fbo == 0) glGenFramebuffers(1, &s.prev_fbo);
            glGenTextures(1, &s.prev);
            glBindTexture(GL_TEXTURE_2D, s.prev);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, f.width, f.height);
            glBindFramebuffer(GL_FRAMEBUFFER, s.prev_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.prev, 0);
            s.prev_w = f.width;
            s.prev_h = f.height;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, s.prev_fbo);
        glViewport(0, 0, f.width, f.height);
        glUseProgram(keep_program);
        glUniform2f(u_keep_size, static_cast<float>(f.width), static_cast<float>(f.height));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, src);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFlush();
        return out;
    }

    std::vector<link::up::Format> formats() const {
        std::vector<link::up::Format> out;
        if (!ok || QueryFormats == nullptr || QueryModifiers == nullptr) return out;
        EGLint n = 0;
        QueryFormats(dpy, 0, nullptr, &n);
        std::vector<EGLint> fourccs(static_cast<std::size_t>(n));
        QueryFormats(dpy, n, fourccs.data(), &n);
        for (auto fourcc : fourccs) {
            // What clients render windows into: 8-bit RGB with or without alpha.
            constexpr EGLint ar24 = 0x34325241, xr24 = 0x34325258, ab24 = 0x34324241, xb24 = 0x34324258;
            if (fourcc != ar24 && fourcc != xr24 && fourcc != ab24 && fourcc != xb24) continue;
            EGLint m = 0;
            QueryModifiers(dpy, fourcc, 0, nullptr, nullptr, &m);
            std::vector<EGLuint64KHR> mods(static_cast<std::size_t>(m));
            std::vector<EGLBoolean> external(static_cast<std::size_t>(m));
            QueryModifiers(dpy, fourcc, m, mods.data(), external.data(), &m);
            for (auto mod : mods) out.push_back({static_cast<std::uint32_t>(fourcc), static_cast<std::uint64_t>(mod)});
        }
        return out;
    }
};

Encoder::Encoder() : p_(std::make_unique<Impl>()) {
    if (!p_->ok) std::fprintf(stderr, "i4d: GPU video encoding off: %s\n", p_->why.c_str());
}
Encoder::~Encoder() = default;
bool Encoder::available() const { return p_->ok; }
const std::string& Encoder::why() const { return p_->why; }
std::vector<link::up::Format> Encoder::formats() const { return p_->formats(); }
Encoder::StreamPtr Encoder::stream() { return StreamPtr{new Stream}; }
void Encoder::StreamFree::operator()(Stream* s) const { delete s; }
std::optional<link::down::Video> Encoder::encode(Stream& s, const DmabufFrame& f, bool keyframe) {
    return p_->encode(s, f, keyframe);
}
std::optional<std::vector<std::uint8_t>> Encoder::changes(Stream& s, const DmabufFrame& f) { return p_->changes(s, f); }
std::optional<std::vector<std::byte>> Encoder::read(const DmabufFrame& f, const std::vector<std::array<int, 4>>& rects) {
    return p_->read(f, rects);
}
void Encoder::forget(std::uint64_t buffer) {
    if (!p_->ok) return;
    auto it = std::ranges::find(p_->imports, buffer, &Impl::Import::buffer);
    if (it == p_->imports.end()) return;
    eglMakeCurrent(p_->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, p_->egl);
    p_->drop(*it);
    p_->imports.erase(it);
}

}  // namespace i4::server
