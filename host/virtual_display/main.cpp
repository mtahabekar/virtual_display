// Milestone 1: KWin 5.27 native virtual-output lifetime and discovery.
#include "screencast-client.h"
#include <wayland-client.h>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <poll.h>
#include <stdexcept>

namespace {
volatile sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
using Clock = std::chrono::steady_clock;
constexpr int Width = 2560, Height = 1440;

bool kwinRunning()
{
    auto *bus = QDBusConnection::sessionBus().interface();
    if (!bus) return false;
    const QDBusReply<bool> reply = bus->isServiceRegistered("org.kde.KWin");
    return reply.isValid() && reply.value();
}

void checkExistingKScreenOutputs()
{
    // wl_output only describes enabled outputs. Check KScreen too so that a
    // disabled virtual output owned by another application cannot be duplicated.
    QProcess process;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("QT_QPA_PLATFORM", "wayland");
    // The installed NVIDIA EGL driver crashes this Qt5 diagnostic. Keep its
    // real Wayland backend, but avoid GPU initialization in this child only.
    if (QFile::exists("/usr/share/glvnd/egl_vendor.d/50_mesa.json")) {
        env.insert("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json");
        env.insert("LIBGL_ALWAYS_SOFTWARE", "1");
        env.insert("QT_QUICK_BACKEND", "software");
    }
    process.setProcessEnvironment(env);
    process.start("kscreen-doctor", {"-j"});
    if (!process.waitForStarted(3000) || !process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        throw std::runtime_error("KScreen inspection failed or timed out; refusing to create potentially duplicate outputs");
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(process.readAllStandardOutput(), &error);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        error.error != QJsonParseError::NoError || !json.object().value("outputs").isArray())
        throw std::runtime_error("KScreen returned no valid output inventory; run kscreen-doctor -j in Plasma Wayland");
    for (const auto &entry : json.object().value("outputs").toArray()) {
        auto name = entry.toObject().value("name").toString();
        if (name.startsWith("Virtual-")) name.remove(0, 8);
        if (name.startsWith("QUEST-"))
            throw std::runtime_error("KScreen already has " + name.toStdString() + "; stop its owner before starting this host");
    }
}

struct Output {
    wl_output *proxy = nullptr;
    uint32_t global = 0;
    std::string name, description;
    int width = 0, height = 0, refresh = 0, scale = 1;
    int x = 0, y = 0;
    bool done = false;
    ~Output() { if (proxy) wl_output_release(proxy); }
};
struct Stream {
    int id = 0;
    std::string name;
    zkde_screencast_stream_unstable_v1 *proxy = nullptr;
    uint32_t node = 0;
    bool ready = false;
    std::string error;
};

class VirtualDisplayManager {
public:
    VirtualDisplayManager()
    {
        for (int i = 0; i < 3; ++i) {
            streams[i].id = i;
            streams[i].name = "QUEST-" + std::to_string(i + 1);
        }
    }
    ~VirtualDisplayManager()
    {
        for (auto &s : streams)
            if (s.proxy) zkde_screencast_stream_unstable_v1_close(s.proxy);
        outputs.clear();
        if (manager) zkde_screencast_unstable_v1_destroy(manager);
        if (registry) wl_registry_destroy(registry);
        if (display) {
            wl_display_flush(display);
            wl_display_disconnect(display);
        }
    }
    void connectDisplay()
    {
        display = wl_display_connect(nullptr);
        if (!display) throw std::runtime_error(std::string("Cannot connect to Wayland: ") + strerror(errno));
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {globalAdded, globalRemoved};
        wl_registry_add_listener(registry, &listener, this);
        sync(); // registry globals
        sync(); // initial wl_output events
    }
    void create()
    {
        if (!manager || advertisedVersion < 2)
            throw std::runtime_error("KWin virtual-output protocol unavailable. Use Plasma Wayland and run tools/register-desktop.py for this exact executable.");
        for (const auto &s : streams)
            if (findOutput(s.name))
                throw std::runtime_error("Output already exists: " + s.name + ". Stop its owning process before starting this host.");
        static const zkde_screencast_stream_unstable_v1_listener listener = {
            streamClosed, streamCreated, streamFailed
        };
        // Always use the v2 request (supported by the installed 5.27 server).
        // Scale 1 means logical and physical dimensions are both 2560x1440.
        for (auto &s : streams) {
            s.proxy = zkde_screencast_unstable_v1_stream_virtual_output(
                manager, s.name.c_str(), Width, Height, wl_fixed_from_int(1),
                ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
            if (!s.proxy) throw std::runtime_error("Could not allocate screencast stream");
            zkde_screencast_stream_unstable_v1_add_listener(s.proxy, &listener, &s);
        }
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!stopping && Clock::now() < deadline) {
            pump(100);
            checkStreamErrors();
            if (complete()) {
                for (const auto &s : streams) {
                    const auto *o = findOutput(s.name);
                    std::cerr << "[virtual-display] " << s.name << " created: " << o->width << 'x' << o->height
                              << " output=" << o->name << " global=" << o->global << '\n';
                    std::cerr << "[capture:" << s.id << "] PipeWire node " << s.node << " (feed allocated; frames not yet validated)\n";
                }
                return;
            }
        }
        throw std::runtime_error(stopping ? "Startup interrupted" : "Timed out waiting for three named 2560x1440 outputs and PipeWire nodes; rolling back");
    }
    void checkStreamErrors() const
    {
        for (const auto &s : streams)
            if (!s.error.empty()) throw std::runtime_error(s.name + ": " + s.error);
    }
    bool complete() const
    {
        for (const auto &s : streams) {
            const auto *o = findOutput(s.name);
            if (!s.ready || !o || !o->done || o->width != Width || o->height != Height || o->scale != 1
                || o->refresh < 59000 || o->refresh > 61000)
                return false;
        }
        return true;
    }
    QJsonObject snapshot(bool ready) const
    {
        QJsonArray screens;
        for (const auto &entry : outputs) {
            const auto &o = *entry.second;
            screens.append(QJsonObject{{"name", QString::fromStdString(o.name)},
                {"description", QString::fromStdString(o.description)}, {"wayland_global", int(o.global)},
                {"width", o.width}, {"height", o.height}, {"refresh_millihz", o.refresh},
                {"scale", o.scale}, {"x", o.x}, {"y", o.y}});
        }
        QJsonArray feeds;
        for (const auto &s : streams) {
            if (!s.ready) continue;
            const auto *o = findOutput(s.name);
            feeds.append(QJsonObject{{"id", s.id}, {"name", QString::fromStdString(s.name)},
                {"output_name", o ? QString::fromStdString(o->name) : QString()},
                {"wayland_global", o ? int(o->global) : 0}, {"pipewire_node", qint64(s.node)},
                {"width", Width}, {"height", Height}, {"target_fps", 60}});
        }
        return {{"schema_version", 1}, {"pid", QCoreApplication::applicationPid()},
            {"wayland_display", qEnvironmentVariable("WAYLAND_DISPLAY")},
            {"session_type", qEnvironmentVariable("XDG_SESSION_TYPE")},
            {"kwin_running", kwinRunning()}, {"screencast_advertised_version", int(advertisedVersion)},
            {"screencast_bound_version", int(boundVersion)}, {"ready", ready},
            {"outputs", screens}, {"monitors", feeds}};
    }
    void pump(int timeout)
    {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) failConnection();
        }
        int flushed = wl_display_flush(display);
        if (flushed < 0 && errno != EAGAIN) {
            wl_display_cancel_read(display);
            failConnection();
        }
        pollfd fd{wl_display_get_fd(display), short(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
        const int n = poll(&fd, 1, timeout);
        if (n < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) return;
            throw std::runtime_error("Wayland poll failed");
        }
        if (n && (fd.revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) failConnection();
        } else {
            wl_display_cancel_read(display);
        }
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) failConnection();
        if (wl_display_dispatch_pending(display) < 0) failConnection();
    }

private:
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    zkde_screencast_unstable_v1 *manager = nullptr;
    uint32_t managerGlobal = 0, advertisedVersion = 0, boundVersion = 0;
    std::map<uint32_t, std::unique_ptr<Output>> outputs;
    std::array<Stream, 3> streams;
    [[noreturn]] void failConnection() const { throw std::runtime_error("Wayland connection lost or protocol error"); }
    const Output *findOutput(const std::string &name) const
    {
        const Output *found = nullptr;
        for (const auto &entry : outputs) {
            const auto &o = *entry.second;
            if (o.name == name || o.name == "Virtual-" + name) {
                if (found) throw std::runtime_error("Ambiguous duplicate output name: " + name);
                found = &o;
            }
        }
        return found;
    }
    void sync()
    {
        bool done = false;
        wl_callback *callback = wl_display_sync(display);
        static const wl_callback_listener listener = {
            [](void *data, wl_callback *, uint32_t) { *static_cast<bool *>(data) = true; }
        };
        wl_callback_add_listener(callback, &listener, &done);
        try {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!done && !stopping && Clock::now() < deadline) pump(100);
            if (!done) throw std::runtime_error("Wayland registry timed out");
        } catch (...) { wl_callback_destroy(callback); throw; }
        wl_callback_destroy(callback);
    }
    static void globalAdded(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
    {
        auto &self = *static_cast<VirtualDisplayManager *>(data);
        if (strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
            self.advertisedVersion = version;
            self.boundVersion = std::min(version, 3u);
            self.managerGlobal = name;
            self.manager = static_cast<zkde_screencast_unstable_v1 *>(
                wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, self.boundVersion));
        } else if (strcmp(interface, "wl_output") == 0 && version >= 4) {
            auto o = std::make_unique<Output>();
            o->global = name;
            o->proxy = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 4));
            static const wl_output_listener listener = {
                [](void *d, wl_output *, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {
                    auto &o = *static_cast<Output *>(d); o.x = x; o.y = y;
                },
                [](void *d, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
                    if (flags & WL_OUTPUT_MODE_CURRENT) {
                        auto &o = *static_cast<Output *>(d); o.width = w; o.height = h; o.refresh = refresh;
                    }
                },
                [](void *d, wl_output *) { static_cast<Output *>(d)->done = true; },
                [](void *d, wl_output *, int32_t scale) { static_cast<Output *>(d)->scale = scale; },
                [](void *d, wl_output *, const char *name) { static_cast<Output *>(d)->name = name; },
                [](void *d, wl_output *, const char *description) { static_cast<Output *>(d)->description = description; }
            };
            wl_output_add_listener(o->proxy, &listener, o.get());
            self.outputs.emplace(name, std::move(o));
        }
    }
    static void globalRemoved(void *data, wl_registry *, uint32_t name)
    {
        auto &self = *static_cast<VirtualDisplayManager *>(data);
        self.outputs.erase(name);
        if (name == self.managerGlobal)
            for (auto &s : self.streams) s.error = "Screencast global removed";
    }
    static void streamClosed(void *d, zkde_screencast_stream_unstable_v1 *) { static_cast<Stream *>(d)->error = "KWin closed the stream"; }
    static void streamCreated(void *d, zkde_screencast_stream_unstable_v1 *, uint32_t node)
    {
        auto &s = *static_cast<Stream *>(d); s.node = node; s.ready = true;
    }
    static void streamFailed(void *d, zkde_screencast_stream_unstable_v1 *, const char *error) { static_cast<Stream *>(d)->error = error; }
};

void saveState(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error("Could not atomically write runtime state");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 2 && args[1] == "--help") {
        std::cout << "Usage: quest-displays --probe | --run\n"
                     "--probe  Read Wayland outputs and accessible KWin protocol as JSON; creates nothing.\n"
                     "--run    Own exactly three 2560x1440 KWin virtual outputs until Ctrl-C.\n"
                     "State: $XDG_RUNTIME_DIR/quest-displays/state.json\n";
        return 0;
    }
    if (args.size() != 2 || (args[1] != "--probe" && args[1] != "--run")) {
        std::cerr << "Use --help, --probe, or --run\n"; return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    QString statePath;
    std::unique_ptr<QLockFile> lock;
    try {
        const bool run = args[1] == "--run";
        if (run) {
            if (qEnvironmentVariable("XDG_SESSION_TYPE") != "wayland" || !kwinRunning())
                throw std::runtime_error("A live Plasma/KWin Wayland session is required. Save work and select Plasma (Wayland) at login; no displays were changed.");
            const auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
            if (runtime.isEmpty()) throw std::runtime_error("XDG_RUNTIME_DIR is missing");
            const auto dir = runtime + "/quest-displays";
            if (!QDir().mkpath(dir)) throw std::runtime_error("Cannot create runtime directory");
            QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            lock = std::make_unique<QLockFile>(dir + "/host.lock");
            lock->setStaleLockTime(0);
            if (!lock->tryLock()) throw std::runtime_error("Another quest-displays instance holds the runtime lock");
            statePath = dir + "/state.json";
            QFile::remove(statePath);
        }
        VirtualDisplayManager displays;
        displays.connectDisplay();
        if (!run) {
            std::cout << QJsonDocument(displays.snapshot(false)).toJson().constData();
            return 0;
        }
        checkExistingKScreenOutputs();
        displays.create();
        QByteArray last;
        while (!stopping) {
            displays.checkStreamErrors();
            if (!displays.complete()) throw std::runtime_error("An owned output disappeared or changed dimensions/scale; stopping all three");
            const auto state = QJsonDocument(displays.snapshot(true)).toJson();
            if (state != last) { saveState(statePath, state); last = state; }
            displays.pump(250);
        }
        QFile::remove(statePath);
        std::cerr << "[virtual-display] Stopping; releasing the three owned outputs\n";
        return 0;
    } catch (const std::exception &error) {
        if (!statePath.isEmpty()) QFile::remove(statePath);
        std::cerr << "[virtual-display] " << error.what() << '\n';
        return 1;
    }
}
