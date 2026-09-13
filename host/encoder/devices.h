#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

namespace quest {
inline std::string intelRenderNode(const std::string &requested = {},
                                   const std::string &sysroot = "/sys/class/drm",
                                   const std::string &devroot = "/dev/dri") {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    for (const auto &node : fs::directory_iterator(sysroot)) {
        const auto name = node.path().filename().string();
        if (name.rfind("renderD", 0) != 0) continue;
        std::string vendor;
        std::ifstream(node.path() / "device/vendor") >> vendor;
        if (vendor != "0x8086") continue;
        const auto path = fs::path(devroot) / name;
        const auto pci = fs::canonical(node.path() / "device").filename().string();
        auto stable = fs::path(devroot) / "by-path" / ("pci-" + pci + "-render");
        if (!requested.empty() && fs::weakly_canonical(requested) != fs::weakly_canonical(path)) continue;
        candidates.push_back(fs::exists(stable) ? stable.string() : path.string());
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.empty()) throw std::runtime_error("No matching Intel render node (vendor 0x8086); check GPU enablement/device access");
    return candidates.front();
}
inline std::vector<std::string> encoderCandidates(const std::string &backend, bool fallback) {
    if (backend == "auto") return fallback ? std::vector<std::string>{"intel-vaapi", "nvenc"}
                                          : std::vector<std::string>{"intel-vaapi"};
    if (backend == "intel-vaapi" || backend == "nvenc") return {backend};
    if (backend == "intel-qsv") throw std::runtime_error("intel-qsv is not implemented in this build; use intel-vaapi (no silent substitution)");
    throw std::runtime_error("Unknown encoder backend");
}
}
