#pragma once
#include "../monitors.h"
#include <QImage>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
}
struct spa_buffer;

namespace quest {
struct Monitor {
    int id;
    QString name, output, serial;
    int node;
};
std::vector<Monitor> discoverMonitors(const QString &statePath);
QString defaultStatePath();

// Host-owned CPU BGRx pixels or an Intel NV12 surface. No compositor-owned
// memory is exposed to encoders or static repeats.
struct Frame {
    std::vector<uint8_t> pixels;
    std::shared_ptr<AVFrame> hardware;
    int stride = 0;
    int64_t ptsUs = 0;
};
using Sample = std::shared_ptr<const Frame>;

// Converts into host-owned GPU surfaces before returning the producer buffer.
// No encoder operation is permitted here. settle() is shutdown-only, off RT.
class DmaBufConverter {
public:
    virtual ~DmaBufConverter() = default;
    virtual std::shared_ptr<AVFrame> allocate() = 0;
    virtual std::vector<uint64_t> modifiers() const = 0;
    virtual void import(spa_buffer *, uint64_t modifier, bool alpha) = 0;
    virtual void convert(spa_buffer *, AVFrame *) = 0;
    virtual void remove(spa_buffer *) = 0;
    virtual void settle() = 0;
};

class MappedFrame {
public:
    explicit MappedFrame(Sample frame);
    const uint8_t *data() const;
    int stride() const;
    QImage copyImage() const;
    int64_t ptsUs;
    Sample owner() const { return frame; }
private:
    Sample frame;
};

class CaptureSession {
public:
    explicit CaptureSession(const Monitor &monitor, std::shared_ptr<DmaBufConverter> converter = {}, bool requireDma = false);
    ~CaptureSession();
    CaptureSession(const CaptureSession &) = delete;
    Sample next(int timeoutMs = 250);
    void checkError();
    uint64_t captured() const;
    uint64_t dropped() const;
    bool usingDmaBuf() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
