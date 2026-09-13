#pragma once
#include "capture/capture.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <chrono>
#include <functional>
#include <map>
#include <vector>

namespace quest {
struct EncoderOptions {
    std::string backend = "auto", renderNode;
    bool allowNvencFallback = false;
    int gop = 60, vbvMs = 50;
};
class IntelVaapi;
struct EncodedPacket {
    int monitorId;
    int64_t ptsUs;
    bool keyframe;
    std::vector<uint8_t> bytes;
};
using PacketSink = std::function<void(EncodedPacket)>;
class VideoEncoder {
public:
    VideoEncoder(int monitorId, int bitrateMbps, PacketSink sink, EncoderOptions options = {});
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder &) = delete;
    void encode(const MappedFrame &source, bool forceKeyframe = false, int64_t presentationUs = -1);
    void flush();
    const std::string &name() const { return selected; }
    const std::string &device() const { return devicePath; }
    std::shared_ptr<DmaBufConverter> dmaConverter() const;
    uint64_t frames = 0, bytes = 0;
    uint64_t timestampAdjustments = 0;
    double latencyTotalMs = 0, latencyMaxMs = 0;
private:
    void initialize(int bitrateMbps, const EncoderOptions &options);
    std::string selected, devicePath;
    std::shared_ptr<IntelVaapi> intel;
    std::vector<uint8_t> sps, pps;
    void drain();
    int id;
    AVCodecContext *context = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    PacketSink sink;
    using Clock = std::chrono::steady_clock;
    std::map<int64_t, Clock::time_point> submitted;
    int64_t lastPts = -1;
};
}
