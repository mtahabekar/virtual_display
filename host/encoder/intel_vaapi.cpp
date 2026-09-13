#include "intel_vaapi.h"
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <spa/buffer/buffer.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
namespace quest {
namespace {
void check(int r, const char *name) { if (r < 0) throw std::runtime_error(std::string(name) + ": " + std::to_string(r)); }
void vaCheck(VAStatus r, const char *name) { if (r != VA_STATUS_SUCCESS) throw std::runtime_error(std::string(name) + ": " + vaErrorStr(r)); }
std::shared_ptr<AVFrame> frame() {
    auto f = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame *p) { av_frame_free(&p); });
    if (!f) throw std::bad_alloc();
    return f;
}
VASurfaceID surface(const AVFrame *f) { return VASurfaceID(uintptr_t(f->data[3])); }
}
struct IntelVaapi::Impl {
    AVBufferRef *device = nullptr, *nv12 = nullptr, *rgb = nullptr;
    VADisplay display = nullptr;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VABufferID params = VA_INVALID_ID;
    VASurfaceID pending = VA_INVALID_SURFACE;
    struct Import { VASurfaceID surface; int64_t fd; uint32_t offset; int32_t stride; uint64_t modifier; bool alpha; };
    std::map<spa_buffer *, Import> imports;
    std::vector<uint64_t> modifiers{0}; // DRM_FORMAT_MOD_LINEAR, never an implicit modifier
    std::mutex mutex;
    ~Impl() {
        if (display) {
            if (pending != VA_INVALID_SURFACE) vaSyncSurface(display, pending);
            for (auto &item : imports) vaDestroySurfaces(display, &item.second.surface, 1);
            if (params != VA_INVALID_ID) vaDestroyBuffer(display, params);
            if (context != VA_INVALID_ID) vaDestroyContext(display, context);
            if (config != VA_INVALID_ID) vaDestroyConfig(display, config);
        }
        av_buffer_unref(&rgb); av_buffer_unref(&nv12); av_buffer_unref(&device);
    }
    AVBufferRef *pool(AVPixelFormat format) {
        auto *ref = av_hwframe_ctx_alloc(device);
        if (!ref) throw std::bad_alloc();
        auto *ctx = reinterpret_cast<AVHWFramesContext *>(ref->data);
        ctx->format = AV_PIX_FMT_VAAPI; ctx->sw_format = format;
        ctx->width = 2560; ctx->height = 1440;
        int r = av_hwframe_ctx_init(ref);
        if (r < 0) { av_buffer_unref(&ref); check(r, "VAAPI frame pool"); }
        return ref;
    }
    void process(VASurfaceID input, AVFrame *output, uint64_t timeout) {
        VAProcPipelineParameterBuffer p{};
        p.surface = input;
        p.surface_color_standard = VAProcColorStandardBT709;
        p.output_color_standard = VAProcColorStandardBT709;
        p.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
        p.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;
        void *mapped = nullptr;
        vaCheck(vaMapBuffer(display, params, &mapped), "Map VPP parameters");
        *static_cast<VAProcPipelineParameterBuffer *>(mapped) = p;
        vaCheck(vaUnmapBuffer(display, params), "Unmap VPP parameters");
        pending = surface(output);
        vaCheck(vaBeginPicture(display, context, pending), "Begin Intel VPP");
        const auto rendered = vaRenderPicture(display, context, &params, 1);
        const auto ended = vaEndPicture(display, context);
        vaCheck(rendered, "Intel VPP conversion"); vaCheck(ended, "End Intel VPP");
        // Capture waits only for the GPU-local conversion, never H.264 encoding.
        // Timeout quarantines the input/output; shutdown settles them off RT.
        vaCheck(vaSyncSurface2(display, pending, timeout), "Intel VPP completion (use capture_memory=cpu if DMA-BUF times out)");
        pending = VA_INVALID_SURFACE;
    }
};
IntelVaapi::IntelVaapi(const std::string &path) : impl(std::make_unique<Impl>()) {
    auto &s = *impl;
    check(av_hwdevice_ctx_create(&s.device, AV_HWDEVICE_TYPE_VAAPI, path.c_str(), nullptr, 0), "Intel VAAPI device");
    auto *device = reinterpret_cast<AVHWDeviceContext *>(s.device->data);
    s.display = static_cast<AVVAAPIDeviceContext *>(device->hwctx)->display;
    s.nv12 = s.pool(AV_PIX_FMT_NV12); s.rgb = s.pool(AV_PIX_FMT_BGR0);
    vaCheck(vaCreateConfig(s.display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &s.config), "Intel VPP config");
    vaCheck(vaCreateContext(s.display, s.config, 2560, 1440, VA_PROGRESSIVE, nullptr, 0, &s.context), "Intel VPP context");
    vaCheck(vaCreateBuffer(s.display, s.context, VAProcPipelineParameterBufferType,
                          sizeof(VAProcPipelineParameterBuffer), 1, nullptr, &s.params), "VPP parameters");
    auto probe = frame();
    check(av_hwframe_get_buffer(s.rgb, probe.get(), 0), "RGB allocation");
    VADRMPRIMESurfaceDescriptor descriptor{};
    if (vaExportSurfaceHandle(s.display, surface(probe.get()), VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                              VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &descriptor) == VA_STATUS_SUCCESS) {
        if (descriptor.num_objects == 1 && descriptor.num_layers == 1 && descriptor.layers[0].num_planes == 1 &&
            descriptor.objects[0].drm_format_modifier != 0)
            s.modifiers.insert(s.modifiers.begin(), descriptor.objects[0].drm_format_modifier);
        for (uint32_t i = 0; i < descriptor.num_objects; ++i) close(descriptor.objects[i].fd);
    }
}
IntelVaapi::~IntelVaapi() = default;
AVBufferRef *IntelVaapi::frames() const { return impl->nv12; }
std::shared_ptr<AVFrame> IntelVaapi::allocate() {
    auto f = frame(); check(av_hwframe_get_buffer(impl->nv12, f.get(), 0), "NV12 allocation"); return f;
}
std::vector<uint64_t> IntelVaapi::modifiers() const { return impl->modifiers; }
void IntelVaapi::import(spa_buffer *buffer, uint64_t modifier, bool alpha) {
    auto &s = *impl; std::lock_guard<std::mutex> lock(s.mutex);
    if (buffer->n_datas != 1 || buffer->datas[0].type != SPA_DATA_DmaBuf)
        throw std::runtime_error("Intel RGB DMA-BUF requires one packed plane/object");
    const auto &d = buffer->datas[0];
    if (!d.chunk || d.fd < 0 || d.chunk->stride < 2560 * 4)
        throw std::runtime_error("Invalid DMA-BUF plane metadata");
    if (auto found = s.imports.find(buffer); found != s.imports.end()) {
        const auto &i = found->second;
        if (i.fd != d.fd || i.offset != d.chunk->offset || i.stride != d.chunk->stride ||
            i.modifier != modifier || i.alpha != alpha)
            throw std::runtime_error("DMA-BUF layout changed without buffer replacement");
        return;
    }
    const auto size = lseek(d.fd, 0, SEEK_END); // DMA-BUF size, not SPA maxsize (often zero)
    lseek(d.fd, 0, SEEK_SET);
    if (size <= 0 || uint64_t(size) > UINT32_MAX) throw std::runtime_error("Cannot determine DMA-BUF object size");
    if (d.chunk->offset >= uint64_t(size) || (modifier == 0 &&
        uint64_t(d.chunk->offset) + uint64_t(d.chunk->stride) * 1439 + 2560 * 4 > uint64_t(size)))
        throw std::runtime_error("DMA-BUF plane exceeds object size");
    VADRMPRIMESurfaceDescriptor desc{};
    desc.fourcc = alpha ? VA_FOURCC_BGRA : VA_FOURCC_BGRX;
    desc.width = 2560; desc.height = 1440; desc.num_objects = 1;
    desc.objects[0].fd = int(d.fd); desc.objects[0].size = uint32_t(size); desc.objects[0].drm_format_modifier = modifier;
    desc.num_layers = 1;
    // DRM ARGB8888 / XRGB8888. VA fourcc and DRM fourcc are different conventions.
    desc.layers[0].drm_format = alpha ? 0x34325241 : 0x34325258;
    desc.layers[0].num_planes = 1; desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = d.chunk->offset; desc.layers[0].pitch[0] = d.chunk->stride;
    VASurfaceAttrib attrs[2]{};
    attrs[0].type = VASurfaceAttribMemoryType; attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger; attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor; attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer; attrs[1].value.value.p = &desc;
    VASurfaceID id = VA_INVALID_SURFACE;
    vaCheck(vaCreateSurfaces(s.display, VA_RT_FORMAT_RGB32, 2560, 1440, &id, 1, attrs, 2), "Import RGB DMA-BUF into Intel");
    s.imports.emplace(buffer, Impl::Import{id, d.fd, d.chunk->offset, d.chunk->stride, modifier, alpha});
}
void IntelVaapi::convert(spa_buffer *buffer, AVFrame *output) {
    auto &s = *impl; std::lock_guard<std::mutex> lock(s.mutex);
    s.process(s.imports.at(buffer).surface, output, 2000000); // at most 2 ms explicit GPU wait on capture thread
}
void IntelVaapi::settle() {
    auto &s = *impl; std::lock_guard<std::mutex> lock(s.mutex);
    if (s.pending != VA_INVALID_SURFACE) {
        vaSyncSurface(s.display, s.pending);
        s.pending = VA_INVALID_SURFACE;
    }
}
void IntelVaapi::remove(spa_buffer *buffer) {
    auto &s = *impl; std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.imports.find(buffer);
    // Removal runs on PipeWire's control loop, never its RT data callback.
    if (s.pending != VA_INVALID_SURFACE) { vaSyncSurface(s.display, s.pending); s.pending = VA_INVALID_SURFACE; }
    if (it != s.imports.end()) { vaDestroySurfaces(s.display, &it->second.surface, 1); s.imports.erase(it); }
}
std::shared_ptr<AVFrame> IntelVaapi::upload(const MappedFrame &source) {
    auto &s = *impl; std::lock_guard<std::mutex> lock(s.mutex);
    auto rgb = frame(), output = allocate();
    check(av_hwframe_get_buffer(s.rgb, rgb.get(), 0), "Intel RGB upload surface");
    auto cpu = frame(); cpu->format = AV_PIX_FMT_BGR0; cpu->width = 2560; cpu->height = 1440;
    cpu->data[0] = const_cast<uint8_t *>(source.data()); cpu->linesize[0] = source.stride();
    check(av_hwframe_transfer_data(rgb.get(), cpu.get(), 0), "Intel RGB upload");
    try { s.process(surface(rgb.get()), output.get(), 1000000000); }
    catch (...) { if (s.pending != VA_INVALID_SURFACE) vaSyncSurface(s.display, s.pending); s.pending = VA_INVALID_SURFACE; throw; }
    return output;
}
}
