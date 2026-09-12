#include "capture.h"
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace quest {
namespace {
constexpr int Width = 2560, Height = 1440;
// Queue (2) + frame being encoded + last frame kept for static repeats, plus one spare.
constexpr int PoolSize = 5;
std::once_flag pipewireInit;
}

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
        if (monitor.id < 0 || monitor.id >= MonitorCount || monitor.name != QString("QUEST-%1").arg(monitor.id + 1) ||
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
    if (result.size() != size_t(MonitorCount)) throw std::runtime_error("Unexpected number of monitor streams");
    std::sort(result.begin(), result.end(), [](const Monitor &a, const Monitor &b) { return a.id < b.id; });
    return result;
}

MappedFrame::MappedFrame(const Frame *source) : ptsUs(source->ptsUs), frame(source) {}
const uint8_t *MappedFrame::data() const { return frame->pixels.data(); }
int MappedFrame::stride() const { return frame->stride; }
QImage MappedFrame::copyImage() const { return QImage(data(), Width, Height, stride(), QImage::Format_RGB32).copy(); }

struct CaptureSession::Impl {
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_stream *stream = nullptr;
    spa_hook listener{};
    bool streamed = false;
    std::vector<std::shared_ptr<Frame>> pool;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Sample> queue;
    std::string error;
    std::atomic<uint64_t> arrivals{0}, drops{0};

    ~Impl()
    {
        if (loop) pw_thread_loop_lock(loop);
        if (stream) pw_stream_destroy(stream);
        if (core) pw_core_disconnect(core);
        if (loop) { pw_thread_loop_unlock(loop); pw_thread_loop_stop(loop); }
        if (context) pw_context_destroy(context);
        if (loop) pw_thread_loop_destroy(loop);
    }
    void fail(std::string message)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (error.empty()) error = std::move(message);
        }
        available.notify_all();
    }
    static void onStateChanged(void *data, pw_stream_state, pw_stream_state state, const char *message)
    {
        auto &self = *static_cast<Impl *>(data);
        if (state == PW_STREAM_STATE_STREAMING) self.streamed = true;
        else if (state == PW_STREAM_STATE_ERROR) self.fail(message ? message : "PipeWire stream error");
        else if (state == PW_STREAM_STATE_UNCONNECTED && self.streamed) self.fail("PipeWire stream ended");
    }
    static void onParamChanged(void *data, uint32_t id, const spa_pod *param)
    {
        auto &self = *static_cast<Impl *>(data);
        if (!param || id != SPA_PARAM_Format) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0 || info.size.width != Width || info.size.height != Height ||
            (info.format != SPA_VIDEO_FORMAT_BGRx && info.format != SPA_VIDEO_FORMAT_BGRA)) {
            self.fail("Unexpected capture format (requires 2560x1440 BGRx)");
            return;
        }
        uint8_t buffer[512];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod *params[] = {
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
                SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
                SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_header)))),
        };
        pw_stream_update_params(self.stream, params, 2);
    }
    // Runs on the PipeWire data thread (RT_PROCESS). KWin 5.27 drives the stream
    // without pw_stream_trigger_process(), and a graph cycle returns at most one
    // buffer that the consumer has already queued. Copying and requeueing inside
    // this callback returns every buffer in the same cycle; a late requeue leaks
    // one of KWin's 16 buffers until KWin can no longer record frames at all.
    static void onProcess(void *data)
    {
        auto &self = *static_cast<Impl *>(data);
        pw_buffer *latest = nullptr;
        while (pw_buffer *next = pw_stream_dequeue_buffer(self.stream)) {
            if (latest) { pw_stream_queue_buffer(self.stream, latest); ++self.drops; }
            latest = next;
        }
        if (!latest) return;
        self.consume(latest->buffer);
        pw_stream_queue_buffer(self.stream, latest);
    }
    void consume(spa_buffer *buffer)
    {
        const spa_data &d = buffer->datas[0];
        if (!d.data || !d.chunk || d.chunk->size == 0 || (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) return;
        const uint32_t stride = d.chunk->stride > 0 ? uint32_t(d.chunk->stride) : Width * 4;
        if (stride < Width * 4 || uint64_t(d.chunk->offset) + uint64_t(stride) * (Height - 1) + Width * 4 > d.maxsize) {
            fail("Captured buffer is smaller than 2560x1440 BGRx");
            return;
        }
        ++arrivals;
        std::shared_ptr<Frame> frame;
        for (const auto &candidate : pool)
            if (candidate.use_count() == 1) { frame = candidate; break; }
        if (!frame) { ++drops; return; }
        const auto *source = static_cast<const uint8_t *>(d.data) + d.chunk->offset;
        for (int y = 0; y < Height; ++y)
            memcpy(frame->pixels.data() + y * Width * 4, source + y * stride, Width * 4);
        const auto *header = static_cast<const spa_meta_header *>(
            spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));
        frame->ptsUs = header && header->pts >= 0 ? header->pts / 1000
            : std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.size() == 2) { queue.pop_front(); ++drops; }
            queue.push_back(std::move(frame));
        }
        available.notify_one();
    }
};

CaptureSession::CaptureSession(const Monitor &m) : impl(std::make_unique<Impl>())
{
    bool ok = false;
    m.serial.toULongLong(&ok);
    if (!ok) throw std::runtime_error("Invalid capture target");
    std::call_once(pipewireInit, [] { pw_init(nullptr, nullptr); });
    auto &s = *impl;
    for (int i = 0; i < PoolSize; ++i) {
        auto frame = std::make_shared<Frame>();
        frame->pixels.resize(size_t(Width) * 4 * Height);
        frame->stride = Width * 4;
        s.pool.push_back(std::move(frame));
    }
    const auto name = QString("quest-capture-%1").arg(m.id).toUtf8();
    s.loop = pw_thread_loop_new(name.constData(), nullptr);
    if (!s.loop) throw std::runtime_error("Could not create PipeWire loop");
    s.context = pw_context_new(pw_thread_loop_get_loop(s.loop), nullptr, 0);
    if (!s.context || pw_thread_loop_start(s.loop) < 0) throw std::runtime_error("Could not start PipeWire loop");

    static const pw_stream_events events = [] {
        pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = &Impl::onStateChanged;
        e.param_changed = &Impl::onParamChanged;
        e.process = &Impl::onProcess;
        return e;
    }();
    pw_thread_loop_lock(s.loop);
    int result = -1;
    s.core = pw_context_connect(s.context, nullptr, 0);
    if (s.core) {
        const auto serial = m.serial.toUtf8();
        s.stream = pw_stream_new(s.core, name.constData(), pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_TARGET_OBJECT, serial.constData(), nullptr));
    }
    if (s.stream) {
        pw_stream_add_listener(s.stream, &s.listener, &events, &s);
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_rectangle size{Width, Height};
        spa_fraction variable{0, 1}, minimum{1, 1}, maximum{60, 1};
        const spa_pod *params[] = {static_cast<const spa_pod *>(spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable),
            SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&maximum, &minimum, &maximum)))};
        // No modifier is offered, so KWin negotiates CPU-mappable memfd buffers, never DMA-BUF.
        result = pw_stream_connect(s.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    }
    pw_thread_loop_unlock(s.loop);
    if (!s.core) throw std::runtime_error("Cannot connect to PipeWire");
    if (!s.stream || result < 0) throw std::runtime_error("Could not start PipeWire capture");
}
CaptureSession::~CaptureSession() = default;
uint64_t CaptureSession::captured() const { return impl->arrivals.load(); }
uint64_t CaptureSession::dropped() const { return impl->drops.load(); }
Sample CaptureSession::next(int timeoutMs)
{
    checkError();
    std::unique_lock<std::mutex> lock(impl->mutex);
    impl->available.wait_for(lock, std::chrono::milliseconds(timeoutMs),
        [this] { return !impl->queue.empty() || !impl->error.empty(); });
    if (impl->queue.empty()) return {};
    auto sample = std::move(impl->queue.front());
    impl->queue.pop_front();
    return sample;
}
void CaptureSession::checkError()
{
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->error.empty()) throw std::runtime_error(impl->error);
}
}
