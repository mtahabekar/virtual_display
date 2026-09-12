// A protocol fixture, not a compositor: no rendering, windows, GPU, or PipeWire.
// Exercise asynchronous discovery, request arguments, stream errors and rollback.
#define main quest_host_main
#include "../host/virtual_display/main.cpp"
#undef main
#include "screencast-server.h"
#include <wayland-server.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct Fake;
struct FakeOutput {
    Fake *owner = nullptr;
    int id = 0;
    wl_global *global = nullptr;
};
struct Fake {
    wl_display *display = nullptr;
    std::array<FakeOutput, 3> outputs;
    int requested = 0, closed = 0;
    bool reject = false, invalid = false;
};

void bindOutput(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    auto &o = *static_cast<FakeOutput *>(data);
    auto *resource = wl_resource_create(client, &wl_output_interface, std::min(version, 4u), id);
    static const struct wl_output_interface impl = {[](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
    wl_resource_set_implementation(resource, &impl, data, nullptr);
    const auto name = "Virtual-QUEST-" + std::to_string(o.id + 1);
    wl_output_send_geometry(resource, o.id * Width, 0, 600, 340, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "fixture", "virtual", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, Width, Height, 60000);
    wl_output_send_scale(resource, 1);
    wl_output_send_name(resource, name.c_str());
    wl_output_send_description(resource, name.c_str());
    wl_output_send_done(resource);
}
void virtualRequest(wl_client *client, wl_resource *resource, uint32_t id, const char *name,
                    int32_t width, int32_t height, wl_fixed_t scale, uint32_t pointer)
{
    auto &f = *static_cast<Fake *>(wl_resource_get_user_data(resource));
    if (f.requested >= 3) { f.invalid = true; return; }
    auto &o = f.outputs[f.requested];
    o.owner = &f;
    o.id = f.requested++;
    if (std::string(name) != "QUEST-" + std::to_string(o.id + 1) || width != Width || height != Height ||
        scale != wl_fixed_from_int(1) || pointer != ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED)
        f.invalid = true;
    auto *stream = wl_resource_create(client, &zkde_screencast_stream_unstable_v1_interface, 3, id);
    static const struct zkde_screencast_stream_unstable_v1_interface impl = {
        [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }
    };
    wl_resource_set_implementation(stream, &impl, &o, [](wl_resource *r) {
        auto &o = *static_cast<FakeOutput *>(wl_resource_get_user_data(r));
        if (o.global) { wl_global_destroy(o.global); o.global = nullptr; }
        if (++o.owner->closed == 3) wl_display_terminate(o.owner->display);
    });
    o.global = wl_global_create(f.display, &wl_output_interface, 4, &o, bindOutput);
    if (f.reject && o.id == 1)
        zkde_screencast_stream_unstable_v1_send_failed(stream, "injected PipeWire initialization failure");
    else
        zkde_screencast_stream_unstable_v1_send_created(stream, 100 + o.id);
}
void bindManager(wl_client *client, void *data, uint32_t version, uint32_t id)
{
    auto *r = wl_resource_create(client, &zkde_screencast_unstable_v1_interface, std::min(version, 3u), id);
    static const struct zkde_screencast_unstable_v1_interface impl = {
        nullptr, nullptr, [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }, virtualRequest, nullptr
    };
    wl_resource_set_implementation(r, &impl, data, nullptr);
}

bool scenario(bool reject)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("socketpair failed");
    const auto child = fork();
    if (child < 0) throw std::runtime_error("fork failed");
    if (child == 0) {
        close(sockets[0]);
        alarm(10);
        Fake f;
        f.reject = reject;
        f.display = wl_display_create();
        wl_global_create(f.display, &zkde_screencast_unstable_v1_interface, 3, &f, bindManager);
        if (!wl_client_create(f.display, sockets[1])) {
            std::cerr << "[test] Cannot create private Wayland client: " << strerror(errno) << '\n';
            _exit(1);
        }
        wl_display_run(f.display);
        wl_display_destroy_clients(f.display);
        wl_display_destroy(f.display);
        _exit(f.requested == 3 && f.closed == 3 && !f.invalid ? 0 : 1);
    }
    close(sockets[1]);
    qputenv("WAYLAND_SOCKET", QByteArray::number(sockets[0]));
    bool passed = false;
    try {
        VirtualDisplayManager manager;
        manager.connectDisplay();
        manager.create();
        passed = !reject && manager.complete();
    } catch (const std::exception &e) {
        passed = reject && std::string(e.what()).find("injected PipeWire") != std::string::npos;
        std::cerr << "[test] " << e.what() << '\n';
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) return false;
    return passed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    return scenario(false) && scenario(true) ? 0 : 1;
}
