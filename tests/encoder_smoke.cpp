#include "encoder/video_encoder.h"
#include "encoder/annexb.h"
#include <iostream>
#include <QCoreApplication>
// Deliberately not a CTest: explicit, two-frame hardware smoke check only.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        quest::EncoderOptions options; options.backend = argc > 1 ? argv[1] : "intel-vaapi";
        auto input = std::make_shared<quest::Frame>();
        input->stride = 2560 * 4; input->pixels.resize(size_t(input->stride) * 1440, 96);
        unsigned packets = 0;
        quest::VideoEncoder encoder(0, 40, [&](quest::EncodedPacket p) {
            if (!p.keyframe) throw std::runtime_error("Forced IDR was not an IDR");
            std::vector<uint8_t> sps, pps;
            quest::prepareAnnexB(p.bytes, sps, pps);
            ++packets;
        }, options);
        std::unique_ptr<quest::CaptureSession> capture;
        quest::Sample sample = input;
        if (argc > 2 && std::string(argv[2]) == "dmabuf") {
            const auto monitors = quest::discoverMonitors(quest::defaultStatePath());
            capture = std::make_unique<quest::CaptureSession>(monitors.front(), encoder.dmaConverter(), true);
            sample = capture->next(2000);
            capture->checkError();
            if (!sample || !capture->usingDmaBuf()) throw std::runtime_error("No DMA-BUF frame within 2 seconds");
        }
        quest::MappedFrame mapped(sample);
        encoder.encode(mapped, true, 0);
        encoder.encode(mapped, true, 16667);
        encoder.flush();
        if (packets != 2) throw std::runtime_error("Expected two complete AUs");
        std::cout << encoder.name() << " " << encoder.device() << ": two IDR AUs with SPS/PPS passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
