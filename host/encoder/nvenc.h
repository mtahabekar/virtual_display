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
struct EncodedPacket {
    int monitorId;
    int64_t ptsUs;
    bool keyframe;
    std::vector<uint8_t> bytes;
};
using PacketSink = std::function<void(EncodedPacket)>;
class VideoEncoder {
public:
    VideoEncoder(int monitorId, int bitrateMbps, PacketSink sink);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder &) = delete;
    void encode(const MappedFrame &source, bool forceKeyframe = false, int64_t presentationUs = -1);
    void flush();
    uint64_t frames = 0, bytes = 0;
    uint64_t timestampAdjustments = 0;
    double latencyTotalMs = 0, latencyMaxMs = 0;
private:
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
