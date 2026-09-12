#include "capture.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <set>
#include <stdexcept>

namespace quest {
QString defaultStatePath() { return qEnvironmentVariable("XDG_RUNTIME_DIR") + "/quest-displays/state.json"; }
std::vector<Monitor> discoverMonitors(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Display host state missing; start quest-displays first");
    const auto state = QJsonDocument::fromJson(file.readAll()).object();
    const auto pid = state.value("pid").toVariant().toLongLong();
    if (!state.value("ready").toBool() || state.value("schema_version").toInt() != 1 || pid <= 0 ||
        QFileInfo(QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget()).fileName() != "quest-displays")
        throw std::runtime_error("Display host state is stale or invalid");
    if (state.value("wayland_display").toString() != qEnvironmentVariable("WAYLAND_DISPLAY"))
        throw std::runtime_error("Display host belongs to a different Wayland session");
    QProcess pw;
    pw.start("pw-dump", QStringList{});
    if (!pw.waitForStarted(2000) || !pw.waitForFinished(5000)) {
        pw.kill(); pw.waitForFinished(1000);
        throw std::runtime_error("PipeWire discovery timed out");
    }
    const auto nodes = QJsonDocument::fromJson(pw.readAllStandardOutput());
    if (pw.exitCode() != 0 || !nodes.isArray()) throw std::runtime_error("PipeWire discovery failed");
    std::vector<Monitor> result;
    std::set<int> ids;
    std::set<QString> serials;
    for (const auto &value : state.value("monitors").toArray()) {
        const auto m = value.toObject();
        Monitor monitor{m.value("id").toInt(-1), m.value("name").toString(),
                        m.value("output_name").toString(), {}, m.value("pipewire_node").toInt(-1)};
        if (monitor.id < 0 || monitor.id > 2 || monitor.name != QString("QUEST-%1").arg(monitor.id + 1) ||
            (monitor.output != monitor.name && monitor.output != "Virtual-" + monitor.name) ||
            m.value("width").toInt() != 2560 || m.value("height").toInt() != 1440 || !ids.insert(monitor.id).second)
            throw std::runtime_error("Invalid fixed monitor mapping");
        for (const auto &n : nodes.array()) {
            const auto node = n.toObject();
            if (node.value("id").toInt() != monitor.node || node.value("type").toString() != "PipeWire:Interface:Node") continue;
            const auto props = node.value("info").toObject().value("props").toObject();
            // Check both the numeric ID and compositor-provided media name
            // before resolving object.serial to avoid stale ID reuse.
            if (props.value("media.name").toString() != "kwin-screencast-" + monitor.output)
                throw std::runtime_error("PipeWire node no longer belongs to the expected output");
            monitor.serial = props.value("object.serial").toVariant().toString();
        }
        if (monitor.serial.isEmpty() || !serials.insert(monitor.serial).second)
            throw std::runtime_error("Missing or duplicate PipeWire object serial");
        result.push_back(monitor);
    }
    if (result.size() != 3) throw std::runtime_error("Expected exactly three monitor streams");
    std::sort(result.begin(), result.end(), [](const Monitor &a, const Monitor &b) { return a.id < b.id; });
    return result;
}

MappedFrame::MappedFrame(GstSample *sample)
{
    GstVideoInfo info;
    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_WIDTH(&info) != 2560 || GST_VIDEO_INFO_HEIGHT(&info) != 1440 ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRx)
        throw std::runtime_error("Unexpected capture format (requires 2560x1440 BGRx)");
    auto *buffer = gst_sample_get_buffer(sample);
    if (!GST_BUFFER_PTS_IS_VALID(buffer)) throw std::runtime_error("Capture frame has no timestamp");
    ptsUs = GST_BUFFER_PTS(buffer) / 1000;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
        throw std::runtime_error("Could not map system-memory capture frame");
}
MappedFrame::~MappedFrame() { gst_video_frame_unmap(&frame); }
const uint8_t *MappedFrame::data() const { return static_cast<const uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)); }
int MappedFrame::stride() const { return GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0); }
QImage MappedFrame::copyImage() const { return QImage(data(), 2560, 1440, stride(), QImage::Format_RGB32).copy(); }

CaptureSession::CaptureSession(const Monitor &m, int keepaliveMs)
{
    bool ok = false;
    m.serial.toULongLong(&ok);
    if (!ok || keepaliveMs < 0) throw std::runtime_error("Invalid capture target");
    GError *error = nullptr;
    const auto spec = QString("pipewiresrc target-object=%1 always-copy=true keepalive-time=%2 ! "
        "video/x-raw,format=BGRx,width=2560,height=1440 ! "
        "appsink name=frames sync=false emit-signals=true max-buffers=1 drop=false").arg(m.serial).arg(keepaliveMs);
    pipeline = gst_parse_launch(spec.toUtf8().constData(), &error);
    if (error || !pipeline) {
        const std::string message = error ? error->message : "Could not create capture pipeline";
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        throw std::runtime_error(message);
    }
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");
    bus = gst_element_get_bus(pipeline);
    g_signal_connect(sink, "new-sample", G_CALLBACK(onSample), this);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus); gst_object_unref(sink); gst_object_unref(pipeline);
        throw std::runtime_error("Could not start PipeWire capture");
    }
}
CaptureSession::~CaptureSession()
{
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus); gst_object_unref(sink); gst_object_unref(pipeline);
}
GstFlowReturn CaptureSession::onSample(GstAppSink *sink, gpointer data)
{
    auto &self = *static_cast<CaptureSession *>(data);
    Sample sample(gst_app_sink_pull_sample(sink));
    if (!sample) return GST_FLOW_EOS;
    ++self.arrivals;
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        if (self.queue.size() == 2) { self.queue.pop_front(); ++self.drops; }
        self.queue.push_back(std::move(sample));
    }
    self.available.notify_one();
    return GST_FLOW_OK;
}
Sample CaptureSession::next(int timeoutMs)
{
    checkError();
    std::unique_lock<std::mutex> lock(mutex);
    available.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !queue.empty(); });
    if (queue.empty()) return {};
    auto sample = std::move(queue.front()); queue.pop_front(); return sample;
}
void CaptureSession::checkError()
{
    auto *message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!message) return;
    std::string text = "PipeWire stream ended";
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *error = nullptr; gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        text = error->message; g_error_free(error); g_free(debug);
    }
    gst_message_unref(message);
    throw std::runtime_error(text);
}
}
