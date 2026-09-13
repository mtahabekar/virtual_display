#include "nvenc.h"
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
}
namespace quest {
static void check(int result, const char *operation) {
    if (result < 0) { char error[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, error, sizeof(error));
        throw std::runtime_error(std::string(operation) + ": " + error); }
}
// Without a device, every h264_nvenc session creates its own CUDA context
// (about 280 MB of VRAM each). All encoders share this one instead.
static AVBufferRef *sharedCudaDevice()
{
    static std::mutex mutex;
    static AVBufferRef *device = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (!device) check(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0), "Create shared CUDA device");
    return device;
}
void configureNvenc(AVCodecContext *context) {
        context->hw_device_ctx = av_buffer_ref(sharedCudaDevice());
        if (!context->hw_device_ctx) throw std::bad_alloc();
        // The USB link carries far more than these streams need, so spend bits
        // instead of encoder effort: fastest preset, ultra-low-latency tuning.
        for (const auto &option : std::vector<std::pair<const char *, const char *>>{
                 {"preset", "p1"}, {"tune", "ull"}, {"profile", "high"}, {"rc", "vbr"},
                 {"rgb_mode", "yuv420"}, {"rc-lookahead", "0"}, {"zerolatency", "1"},
                 {"delay", "0"}, {"forced-idr", "1"}})
            check(av_opt_set(context->priv_data, option.first, option.second, 0), option.first);

}
}
