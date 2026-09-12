#include "capture/capture.h"
#include "encoder/nvenc.h"
#include "transport/tcp_server.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
volatile sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
using Clock = std::chrono::steady_clock;
std::mutex logs;
void log(const QString &line) { std::lock_guard<std::mutex> lock(logs); std::cerr << line.toStdString() << '\n'; }
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    av_log_set_level(AV_LOG_WARNING);
    QCommandLineParser parser;
    parser.setApplicationDescription("Independent KWin/PipeWire to NVENC H.264 streams");
    parser.addHelpOption();
    parser.addOption({"monitor", "Monitor ID 0/1/2, or all", "id", "all"});
    parser.addOption({"seconds", "Duration in seconds, 0 runs until stopped", "seconds", "0"});
    parser.addOption({"output", "Directory for independent H.264 files and statistics", "directory", "artifacts/streams"});
    parser.addOption({"bitrate", "Target Mbps per monitor (VBR, capped at this rate)", "mbps", "24"});
    parser.addOption({"listen", "TCP port on 127.0.0.1; 0 disables transport", "port", "0"});
    parser.addOption({"no-record", "Do not write video files; keep statistics and TCP output only"});
    parser.process(app);
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    try {
        bool secOk, rateOk, portOk;
        const int seconds = parser.value("seconds").toInt(&secOk), rate = parser.value("bitrate").toInt(&rateOk);
        const auto selection = parser.value("monitor");
        const int port = parser.value("listen").toInt(&portOk);
        if (!secOk || seconds < 0 || !rateOk || rate < 1 || rate > 150 || !portOk || port < 0 || port > 65535 ||
            (selection != "all" && selection != "0" && selection != "1" && selection != "2"))
            throw std::runtime_error("Invalid duration, bitrate or monitor selection");
        QDir directory(parser.value("output"));
        if (!QDir().mkpath(directory.path())) throw std::runtime_error("Cannot create output directory");
        QLockFile outputLock(directory.filePath("stream.lock"));
        outputLock.setStaleLockTime(0);
        if (!outputLock.tryLock()) throw std::runtime_error("Another stream process owns this output directory");
        const auto monitors = quest::discoverMonitors(quest::defaultStatePath());
        std::unique_ptr<quest::TransportServer> transport;
        if (port) {
            transport = std::make_unique<quest::TransportServer>(uint16_t(port));
            log(QString("[transport] listening on 127.0.0.1:%1").arg(port));
        }
        const bool record = !parser.isSet("no-record");
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        std::array<QJsonObject, 3> reports;
        const auto epoch = Clock::now();
        for (const auto &m : monitors) {
            if (selection != "all" && selection != QString::number(m.id)) continue;
            workers.emplace_back([&, m] {
                try {
                    QFile file(directory.filePath(QString("quest-%1.h264").arg(m.id)));
                    if (record && !file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open H.264 output");
                    quest::VideoEncoder encoder(m.id, rate, [&](quest::EncodedPacket packet) {
                        if (record && file.write(reinterpret_cast<const char *>(packet.bytes.data()), packet.bytes.size()) != qint64(packet.bytes.size()))
                            throw std::runtime_error("H.264 output write failed");
                        if (transport) transport->publish(std::move(packet));
                    });
                    log(QString("[encoder:%1] NVENC H264 initialized, 2560x1440, target 60 FPS, %2 Mbps").arg(m.id).arg(rate));
                    quest::CaptureSession capture(m, 0);
                    log(QString("[capture:%1] node=%2 serial=%3").arg(m.id).arg(m.node).arg(m.serial));
                    auto started = Clock::now(), lastLog = started;
                    uint64_t previousCapture = 0, previousEncoded = 0, previousBytes = 0;
                    uint64_t consumed = 0;
                    uint64_t repeated = 0, sourceTimestampRepeats = 0;
                    int64_t previousSourcePts = -1;
                    quest::Sample lastSample;
                    auto lastSubmit = started;
                    bool forcePending = false;
                    while (!interrupted && !failed && (seconds == 0 || Clock::now() - started < std::chrono::seconds(seconds))) {
                        auto sample = capture.next();
                        if (transport) forcePending |= transport->takeKeyframeRequest(m.id);
                        const bool repeat = !sample && lastSample && (forcePending || Clock::now() - lastSubmit >= std::chrono::seconds(1));
                        if (sample || repeat) {
                            quest::MappedFrame frame(sample ? sample.get() : lastSample.get());
                            // A common monotonic host timeline is used on the wire.
                            // It timestamps sample consumption, not GPU render time.
                            const auto pts = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - epoch).count();
                            encoder.encode(frame, forcePending, pts);
                            lastSubmit = Clock::now(); forcePending = false;
                            if (sample) {
                                if (frame.ptsUs <= previousSourcePts) ++sourceTimestampRepeats;
                                previousSourcePts = frame.ptsUs;
                                ++consumed; lastSample = std::move(sample);
                            } else ++repeated;
                        }
                        if (!consumed && Clock::now() - started > std::chrono::seconds(10))
                            throw std::runtime_error("No captured frames after 10 seconds");
                        const auto now = Clock::now();
                        const double interval = std::chrono::duration<double>(now - lastLog).count();
                        if (interval >= 2) {
                            log(QString("[stream:%1] capture=%2 FPS encode=%3 FPS %4 Mbps queue_dropped=%5 encode_latency=%6 ms")
                                .arg(m.id).arg((capture.captured()-previousCapture)/interval, 0, 'f', 1)
                                .arg((encoder.frames-previousEncoded)/interval, 0, 'f', 1)
                                .arg((encoder.bytes-previousBytes)*8e-6/interval, 0, 'f', 2)
                                .arg(capture.dropped()).arg(encoder.frames ? encoder.latencyTotalMs/encoder.frames : 0, 0, 'f', 2));
                            previousCapture=capture.captured(); previousEncoded=encoder.frames; previousBytes=encoder.bytes; lastLog=now;
                        }
                    }
                    const auto captured = capture.captured(), dropped = capture.dropped();
                    const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                    encoder.flush();
                    if (record && !file.flush()) throw std::runtime_error("Could not flush output file");
                    if (!consumed) throw std::runtime_error("No video frames captured");
                    reports[m.id] = {{"monitor_id", m.id}, {"output", m.output}, {"pipewire_serial", m.serial},
                        {"width", 2560}, {"height", 1440}, {"target_fps", 60}, {"codec", "h264"}, {"encoder", "h264_nvenc"},
                        {"seconds", elapsed}, {"capture_frames", qint64(captured)}, {"encoded_frames", qint64(encoder.frames)},
                        {"capture_fps", captured/elapsed}, {"encoded_fps", encoder.frames/elapsed},
                        {"queue_dropped", qint64(dropped)}, {"capture_pending_at_stop", qint64(captured-consumed-dropped)},
                        {"timestamp_adjustments", qint64(encoder.timestampAdjustments)},
                        {"source_timestamp_nonmonotonic", qint64(sourceTimestampRepeats)}, {"static_repeat_frames", qint64(repeated)},
                        {"bitrate_mbps", encoder.bytes*8e-6/elapsed}, {"bytes", qint64(encoder.bytes)},
                        {"encode_latency_mean_ms", encoder.latencyTotalMs/encoder.frames}, {"encode_latency_max_ms", encoder.latencyMaxMs},
                        {"pipewire_keepalive_ms", 0}, {"host_static_repeat_ms", 1000},
                        {"note", "Encode latency includes CPU copy/upload to packet availability, not display-to-display latency. Capture FPS excludes host static repeats. Wire PTS timestamps host sample consumption. Upstream losses unknown."}};
                } catch (const std::exception &e) {
                    failed = true;
                    reports[m.id] = {{"monitor_id", m.id}, {"error", e.what()}};
                    log(QString("[stream:%1] ERROR %2").arg(m.id).arg(e.what()));
                }
            });
        }
        for (auto &worker : workers) worker.join();
        QJsonArray results;
        for (const auto &r : reports) if (!r.isEmpty()) results.append(r);
        QJsonObject report{{"success", !failed.load()}, {"streams", results}};
        if (transport) report.insert("transport", QJsonObject{{"connections", qint64(transport->connections())},
            {"packets_skipped_or_discarded", qint64(transport->droppedPackets())},
            {"slow_client_disconnects", qint64(transport->slowDisconnects())}});
        const auto json = QJsonDocument(report).toJson();
        QSaveFile stats(directory.filePath("stats.json"));
        if (!stats.open(QIODevice::WriteOnly) || stats.write(json) != json.size() || !stats.commit())
            throw std::runtime_error("Cannot save stream statistics");
        std::cout << json.constData();
        return failed ? 1 : 0;
    } catch (const std::exception &e) { log(e.what()); return 1; }
}
