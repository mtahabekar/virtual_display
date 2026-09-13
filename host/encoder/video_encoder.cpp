#include "video_encoder.h"
#include "nvenc.h"
#include "intel_vaapi.h"
#include "devices.h"
#include "annexb.h"
#include <iostream>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace quest {
static void check(int result, const char *operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
VideoEncoder::VideoEncoder(int monitorId, int bitrateMbps, PacketSink output, EncoderOptions options) : id(monitorId), sink(std::move(output))
{
    if (id < 0 || id >= MonitorCount || options.gop < 1 || options.gop > 600 || options.vbvMs < 10 || options.vbvMs > 1000)
        throw std::runtime_error("Invalid encoder stream/GOP/VBV configuration");
    std::string errors;
    for (const auto &candidate : encoderCandidates(options.backend, options.allowNvencFallback)) {
        selected = candidate;
        try { initialize(bitrateMbps, options); return; }
        catch (const std::exception &e) {
            errors += candidate + ": " + e.what() + "; ";
            std::cerr << "[encoder:" << id << "] initialization failed: " << candidate << ": " << e.what() << '\n';
            intel.reset();
        }
    }
    throw std::runtime_error(errors + "No software fallback is permitted");
}
void VideoEncoder::initialize(int bitrateMbps, const EncoderOptions &options) {
    const auto *codec = avcodec_find_encoder_by_name(selected == "nvenc" ? "h264_nvenc" : "h264_vaapi");
    if (!codec) throw std::runtime_error("Requested hardware codec is unavailable");
    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    try {
        if (!context || !frame || !packet) throw std::bad_alloc();
        context->width = 2560; context->height = 1440;
        // Feed the native BGRx capture to NVENC; its RGB mode converts to 4:2:0.
        // This prototype still copies through system memory and uploads frames.
        context->pix_fmt = selected == "nvenc" ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_VAAPI;
        context->time_base = AVRational{1, 1000000};
        context->framerate = AVRational{60, 1};
        context->bit_rate = int64_t(bitrateMbps) * 1000000;
        context->rc_max_rate = context->bit_rate;
        context->rc_buffer_size = bitrateMbps * 1000 * options.vbvMs;
        context->gop_size = options.gop;
        context->max_b_frames = 0;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_range = AVCOL_RANGE_MPEG;
        if (selected == "nvenc") {
        devicePath = "CUDA default device";
        configureNvenc(context);
        } else {
            devicePath = intelRenderNode(options.renderNode);
            intel = std::make_shared<IntelVaapi>(devicePath);
            context->hw_frames_ctx = av_buffer_ref(intel->frames());
            if (!context->hw_frames_ctx) throw std::bad_alloc();
            context->profile = FF_PROFILE_H264_HIGH;
            context->flags |= AV_CODEC_FLAG_LOW_DELAY;
            check(av_opt_set(context->priv_data, "rc_mode", "VBR", 0), "VAAPI rate control");
            check(av_opt_set(context->priv_data, "async_depth", "1", 0), "VAAPI pipeline depth");
            check(av_opt_set(context->priv_data, "idr_interval", "0", 0), "VAAPI IDR interval");
        }
        check(avcodec_open2(context, codec, nullptr), "Hardware H264 initialization");
        frame->format = context->pix_fmt; frame->width = context->width; frame->height = context->height;
        frame->color_primaries = context->color_primaries; frame->color_trc = context->color_trc;
        frame->colorspace = context->colorspace; frame->color_range = context->color_range;
    } catch (...) {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); throw;
    }
}
std::shared_ptr<DmaBufConverter> VideoEncoder::dmaConverter() const { return intel; }
VideoEncoder::~VideoEncoder() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); }
void VideoEncoder::encode(const MappedFrame &source, bool forceKeyframe, int64_t presentationUs)
{
    const auto started = Clock::now();
    av_frame_unref(frame);
    if (intel) {
        auto gpu = source.owner()->hardware ? source.owner()->hardware : intel->upload(source);
        check(av_frame_ref(frame, gpu.get()), "Reference Intel NV12 surface");
    } else {
        if (source.owner()->hardware) throw std::runtime_error("NVENC cannot consume Intel surfaces");
        // Keep the owned capture frame alive for as long as FFmpeg retains it.
        // The producer buffer has already been returned; no second frame memcpy.
        auto *owner = new Sample(source.owner());
        frame->buf[0] = av_buffer_create(const_cast<uint8_t *>(source.data()), size_t(source.stride()) * 1440,
            [](void *opaque, uint8_t *) { delete static_cast<Sample *>(opaque); }, owner, AV_BUFFER_FLAG_READONLY);
        if (!frame->buf[0]) { delete owner; throw std::bad_alloc(); }
        frame->format = AV_PIX_FMT_BGR0; frame->width = 2560; frame->height = 1440;
        frame->data[0] = const_cast<uint8_t *>(source.data()); frame->linesize[0] = source.stride();
    }
    frame->color_primaries = context->color_primaries; frame->color_trc = context->color_trc;
    frame->colorspace = context->colorspace; frame->color_range = context->color_range;
    frame->pts = presentationUs >= 0 ? presentationUs : source.ptsUs;
    // Keep the low-level encoder safe if a caller supplies a repeated PTS.
    if (frame->pts <= lastPts) { frame->pts = lastPts + 1; ++timestampAdjustments; }
    frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    submitted.emplace(frame->pts, started);
    check(avcodec_send_frame(context, frame), "Send frame to hardware encoder");
    lastPts = frame->pts;
    drain();
    av_frame_unref(frame);
}
void VideoEncoder::drain()
{
    while (true) {
        const auto result = avcodec_receive_packet(context, packet);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
        check(result, "Receive NVENC packet");
        auto it = submitted.find(packet->pts);
        if (it != submitted.end()) {
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - it->second).count();
            latencyTotalMs += ms; latencyMaxMs = std::max(latencyMaxMs, ms); submitted.erase(it);
        }
        ++frames;
        EncodedPacket encoded{id, packet->pts, bool(packet->flags & AV_PKT_FLAG_KEY),
            std::vector<uint8_t>(packet->data, packet->data + packet->size)};
        av_packet_unref(packet);
        encoded.keyframe = prepareAnnexB(encoded.bytes, sps, pps);
        bytes += encoded.bytes.size();
        sink(std::move(encoded));
    }
}
void VideoEncoder::flush() { check(avcodec_send_frame(context, nullptr), "Flush hardware encoder"); drain(); }
}
