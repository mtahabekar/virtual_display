#include "capture.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <iostream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 4) { std::cerr << "Usage: quest-capture-test MONITOR_ID SECONDS OUTPUT_DIRECTORY\n"; return 2; }
    try {
        bool idOk, secondsOk;
        const int id = args[1].toInt(&idOk), seconds = args[2].toInt(&secondsOk);
        if (!idOk || id < 0 || id > 2 || !secondsOk || seconds < 1 || seconds > 3600)
            throw std::runtime_error("Invalid monitor ID or duration");
        QDir dir(args[3]);
        if (!QDir().mkpath(dir.path())) throw std::runtime_error("Cannot create capture directory");
        auto monitors = quest::discoverMonitors(quest::defaultStatePath());
        quest::CaptureSession capture(monitors[id]);
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        uint64_t frames = 0;
        int64_t firstPts = -1, lastPts = -1;
        QImage first, last;
        auto nextLog = started + std::chrono::seconds(2);
        while (Clock::now() - started < std::chrono::seconds(seconds)) {
            auto sample = capture.next();
            if (!sample) continue;
            quest::MappedFrame frame(sample.get());
            if (firstPts < 0) { firstPts = frame.ptsUs; first = frame.copyImage(); }
            lastPts = frame.ptsUs;
            ++frames;
            // Copy just the most recent frame once per second, not every frame.
            if (frames == 1 || Clock::now() >= nextLog) {
                last = frame.copyImage();
                const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
                std::cerr << "[capture:" << id << "] " << frames / elapsed << " FPS, queue_dropped=" << capture.dropped() << '\n';
                nextLog = Clock::now() + std::chrono::seconds(1);
            }
        }
        if (!frames) throw std::runtime_error("No frames arrived from the selected output");
        // Freeze measurement before PNG compression; saving files is not part
        // of the capture benchmark and must not inflate its queue-drop count.
        const double elapsed = std::chrono::duration<double>(Clock::now() - started).count();
        const auto drops = capture.dropped();
        if (!first.save(dir.filePath("first.png")) || !last.save(dir.filePath("latest.png")))
            throw std::runtime_error("Could not save capture PNG");
        QJsonObject report{{"monitor_id", id}, {"output", monitors[id].output}, {"pipewire_serial", monitors[id].serial},
            {"width", 2560}, {"height", 1440}, {"format", "BGRx"}, {"frames", qint64(frames)},
            {"seconds", elapsed}, {"capture_fps", frames / elapsed},
            {"pts_fps", lastPts > firstPts ? (frames - 1) * 1e6 / (lastPts - firstPts) : 0.0},
            {"queue_dropped", qint64(drops)}, {"keepalive_ms", 0},
            {"note", "Damage-driven source: static desktops need not produce 60 FPS; upstream losses are not measurable here."}};
        QFile out(dir.filePath("capture.json"));
        if (!out.open(QIODevice::WriteOnly) || out.write(QJsonDocument(report).toJson()) < 0)
            throw std::runtime_error("Could not save capture report");
        std::cout << QJsonDocument(report).toJson().constData();
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
