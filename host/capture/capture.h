#pragma once
#include "../monitors.h"
#include <QImage>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

namespace quest {
struct Monitor {
    int id;
    QString name, output, serial;
    int node;
};
std::vector<Monitor> discoverMonitors(const QString &statePath);
QString defaultStatePath();

// A host-owned copy of one 2560x1440 BGRx frame. PipeWire buffers are never
// exposed outside the capture callback, so holding a frame cannot starve KWin.
struct Frame {
    std::vector<uint8_t> pixels;
    int stride = 0;
    int64_t ptsUs = 0;
};
using Sample = std::shared_ptr<const Frame>;

class MappedFrame {
public:
    explicit MappedFrame(const Frame *frame);
    const uint8_t *data() const;
    int stride() const;
    QImage copyImage() const;
    int64_t ptsUs;
private:
    const Frame *frame;
};

class CaptureSession {
public:
    explicit CaptureSession(const Monitor &monitor);
    ~CaptureSession();
    CaptureSession(const CaptureSession &) = delete;
    Sample next(int timeoutMs = 250);
    void checkError();
    uint64_t captured() const;
    uint64_t dropped() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
