#pragma once
#include "encoder/nvenc.h"
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace quest {
// QSTV v1: fixed 32-byte network-order header + one Annex-B access unit.
std::vector<uint8_t> framePacket(const EncodedPacket &packet);
class TransportServer {
public:
    explicit TransportServer(uint16_t port);
    ~TransportServer();
    TransportServer(const TransportServer &) = delete;
    void publish(EncodedPacket packet);
    bool takeKeyframeRequest(int id) { return forceKeyframe.at(id).exchange(false); }
    uint16_t port() const { return boundPort; }
    uint64_t connections() const { return accepted.load(); }
    uint64_t droppedPackets() const { return dropped.load(); }
    uint64_t slowDisconnects() const { return slow.load(); }
private:
    void run();
    int listener = -1;
    uint16_t boundPort = 0;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::array<std::atomic<bool>, 3> forceKeyframe{};
    std::atomic<uint64_t> accepted{0}, dropped{0}, slow{0};
    std::mutex mutex;
    bool connected = false, disconnect = false;
    std::array<bool, 3> waitingForKeyframe{true, true, true};
    std::deque<std::vector<uint8_t>> queue;
    size_t queuedBytes = 0;
};
}
