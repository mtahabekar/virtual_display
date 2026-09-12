#include "nvenc.h"
extern "C" {
#include <libavutil/opt.h>
}
#include <cstring>
#include <stdexcept>

namespace quest {
static void check(int result, const char *operation) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    throw std::runtime_error(std::string(operation) + ": " + error);
}
VideoEncoder::VideoEncoder(int monitorId, int bitrateMbps, PacketSink output) : id(monitorId), sink(std::move(output))
{
    const auto *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) throw std::runtime_error("h264_nvenc is unavailable; no software fallback is permitted");
    context = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    try {
        if (!context || !frame || !packet) throw std::bad_alloc();
        context->width = 2560; context->height = 1440;
        // Feed the native BGRx capture to NVENC; its RGB mode converts to 4:2:0.
        // This prototype still copies through system memory and uploads frames.
        context->pix_fmt = AV_PIX_FMT_BGR0;
        context->time_base = AVRational{1, 1000000};
        context->framerate = AVRational{60, 1};
        context->bit_rate = int64_t(bitrateMbps) * 1000000;
        context->rc_max_rate = context->bit_rate;
        context->rc_buffer_size = bitrateMbps * 1000000 / 20; // 50 ms VBV
        context->gop_size = 60;
        context->max_b_frames = 0;
        context->color_primaries = AVCOL_PRI_BT709;
        context->color_trc = AVCOL_TRC_BT709;
        context->colorspace = AVCOL_SPC_BT709;
        context->color_range = AVCOL_RANGE_MPEG;
        for (const auto &option : std::vector<std::pair<const char *, const char *>>{
                 {"preset", "p4"}, {"tune", "ll"}, {"profile", "high"}, {"rc", "vbr"},
                 {"rgb_mode", "yuv420"}, {"rc-lookahead", "0"}, {"zerolatency", "1"},
                 {"delay", "0"}, {"forced-idr", "1"}})
            check(av_opt_set(context->priv_data, option.first, option.second, 0), option.first);
        check(avcodec_open2(context, codec, nullptr), "NVENC initialization");
        frame->format = context->pix_fmt; frame->width = context->width; frame->height = context->height;
        frame->color_primaries = context->color_primaries; frame->color_trc = context->color_trc;
        frame->colorspace = context->colorspace; frame->color_range = context->color_range;
        check(av_frame_get_buffer(frame, 32), "Allocate encoder frame");
    } catch (...) {
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); throw;
    }
}
VideoEncoder::~VideoEncoder() { av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context); }
void VideoEncoder::encode(const MappedFrame &source, bool forceKeyframe, int64_t presentationUs)
{
    check(av_frame_make_writable(frame), "Writable encoder frame");
    const auto started = Clock::now();
    for (int y = 0; y < 1440; ++y)
        memcpy(frame->data[0] + y * frame->linesize[0], source.data() + y * source.stride(), 2560 * 4);
    frame->pts = presentationUs >= 0 ? presentationUs : source.ptsUs;
    // Keep the low-level encoder safe if a caller supplies a repeated PTS.
    if (frame->pts <= lastPts) { frame->pts = lastPts + 1; ++timestampAdjustments; }
    frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    submitted.emplace(frame->pts, started);
    check(avcodec_send_frame(context, frame), "Send frame to NVENC");
    lastPts = frame->pts;
    drain();
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
        ++frames; bytes += packet->size;
        EncodedPacket encoded{id, packet->pts, bool(packet->flags & AV_PKT_FLAG_KEY),
            std::vector<uint8_t>(packet->data, packet->data + packet->size)};
        av_packet_unref(packet);
        sink(std::move(encoded));
    }
}
void VideoEncoder::flush() { check(avcodec_send_frame(context, nullptr), "Flush NVENC"); drain(); }
}
