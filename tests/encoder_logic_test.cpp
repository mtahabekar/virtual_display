#include "encoder/devices.h"
#include "encoder/annexb.h"
#include "capture/latest.h"
#include <iostream>
#include <unistd.h>
void require(bool ok) { if (!ok) throw std::runtime_error("Assertion failed"); }
int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("quest-gpu-test-" + std::to_string(getpid()));
    try {
        require(quest::encoderCandidates("auto", false) == std::vector<std::string>{"intel-vaapi"});
        require(quest::encoderCandidates("auto", true) == std::vector<std::string>({"intel-vaapi", "nvenc"}));
        require(quest::encoderCandidates("intel-vaapi", true).size() == 1);
        bool rejected = false;
        try { quest::encoderCandidates("x264", true); } catch (...) { rejected = true; }
        require(rejected);
        for (auto spec : {std::pair{"renderD140", "0x10de"}, std::pair{"renderD151", "0x8086"}}) {
            fs::create_directories(root / "sys" / spec.first / "device");
            std::ofstream(root / "sys" / spec.first / "device/vendor") << spec.second;
        }
        require(quest::intelRenderNode({}, (root/"sys").string(), (root/"dev").string()) == (root/"dev/renderD151").string());
        rejected = false;
        try { quest::intelRenderNode((root/"dev/renderD140").string(), (root/"sys").string(), (root/"dev").string()); }
        catch (...) { rejected = true; }
        require(rejected);
        std::vector<uint8_t> sps, pps, first{0,0,0,1,0x67,42,0,0,1,0x68,42,0,0,1,0x65,42};
        require(quest::prepareAnnexB(first, sps, pps));
        std::vector<uint8_t> reconnect{0,0,1,0x65,43};
        require(quest::prepareAnnexB(reconnect, sps, pps));
        std::vector<uint8_t> freshSps, freshPps;
        require(quest::prepareAnnexB(reconnect, freshSps, freshPps));
        require(!freshSps.empty() && !freshPps.empty());
        std::vector<uint8_t> predictive{0,0,1,0x41,42};
        require(!quest::prepareAnnexB(predictive, sps, pps));
        std::deque<int> raw{1,2}; uint64_t dropped = 0;
        require(quest::takeLatest(raw, dropped) == 2 && dropped == 1 && raw.empty());
        require(quest::takeLatest(raw, dropped) == 0 && dropped == 1);
        fs::remove_all(root); return 0;
    } catch (const std::exception &e) { fs::remove_all(root); std::cerr << e.what() << '\n'; return 1; }
}
