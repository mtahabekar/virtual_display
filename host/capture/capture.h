#pragma once
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <QImage>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace quest {
struct Monitor {
    int id;
    QString name, output, serial;
    int node;
};
std::vector<Monitor> discoverMonitors(const QString &statePath);
QString defaultStatePath();
struct SampleDelete { void operator()(GstSample *s) const { if (s) gst_sample_unref(s); } };
using Sample = std::unique_ptr<GstSample, SampleDelete>;

// Mapping is scoped to the sample's lifetime. Capture always negotiates BGRx
// in ordinary system memory; tiled DMA-BUF is never interpreted as pixels.
class MappedFrame {
public:
    explicit MappedFrame(GstSample *sample);
    ~MappedFrame();
    MappedFrame(const MappedFrame &) = delete;
    MappedFrame &operator=(const MappedFrame &) = delete;
    const uint8_t *data() const;
    int stride() const;
    QImage copyImage() const;
    int64_t ptsUs;
private:
    GstVideoFrame frame{};
};

class CaptureSession {
public:
    explicit CaptureSession(const Monitor &monitor, int keepaliveMs = 0);
    ~CaptureSession();
    CaptureSession(const CaptureSession &) = delete;
    Sample next(int timeoutMs = 250);
    void checkError();
    uint64_t captured() const { return arrivals.load(); }
    uint64_t dropped() const { return drops.load(); }
private:
    static GstFlowReturn onSample(GstAppSink *sink, gpointer data);
    GstElement *pipeline = nullptr;
    GstElement *sink = nullptr;
    GstBus *bus = nullptr;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Sample> queue;
    std::atomic<uint64_t> arrivals{0}, drops{0};
};
}
