#include "tcp_server.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

namespace quest {
static constexpr size_t MaxPayload = 8 * 1024 * 1024;
std::vector<uint8_t> framePacket(const EncodedPacket &p)
{
    if (p.monitorId < 0 || p.monitorId >= MonitorCount || p.ptsUs < 0 || p.bytes.empty() || p.bytes.size() > MaxPayload)
        throw std::runtime_error("Invalid encoded packet");
    std::vector<uint8_t> result(32 + p.bytes.size());
    const uint8_t prefix[] = {'Q', 'S', 'T', 'V', 1, 1, uint8_t(p.monitorId), 1};
    std::copy(std::begin(prefix), std::end(prefix), result.begin());
    size_t position = 8;
    auto put = [&](uint64_t value, int bytes) {
        for (int i = bytes - 1; i >= 0; --i) result[position++] = uint8_t(value >> (8*i));
    };
    put(2560, 2); put(1440, 2); put(60, 2); put(1, 2);
    put(p.ptsUs, 8); put(p.bytes.size(), 4); put(p.keyframe ? 1 : 0, 4);
    std::copy(p.bytes.begin(), p.bytes.end(), result.begin() + 32);
    return result;
}
TransportServer::TransportServer(uint16_t port)
{
    listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) throw std::runtime_error("Cannot create TCP socket");
    const int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || listen(listener, 1) < 0) {
        const auto reason = std::string(strerror(errno)); close(listener); throw std::runtime_error("TCP listen: " + reason);
    }
    socklen_t size = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size);
    boundPort = ntohs(address.sin_port);
    try { worker = std::thread([this] { run(); }); }
    catch (...) { close(listener); throw; }
}
TransportServer::~TransportServer()
{
    stopping = true;
    if (worker.joinable()) worker.join();
    if (listener >= 0) close(listener);
}
void TransportServer::publish(EncodedPacket p)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!connected || disconnect) return;
        if (waitingForKeyframe.at(p.monitorId) && !p.keyframe) { ++dropped; return; }
    }
    // Framing allocates and copies the complete access unit. Keep that work out
    // of the shared queue lock so one high-rate stream cannot block the other
    // encoder workers long enough to starve their two-frame capture queues.
    auto framed = framePacket(p);
    std::lock_guard<std::mutex> lock(mutex);
    if (!connected || disconnect) return;
    if (waitingForKeyframe.at(p.monitorId)) {
        if (!p.keyframe) { ++dropped; return; }
        waitingForKeyframe[p.monitorId] = false;
    }
    // Disconnect instead of silently dropping predictive H.264 frames. On
    // reconnect all streams resume from a newly requested IDR with SPS/PPS.
    if (queue.size() >= 12 || queuedBytes + framed.size() > 16 * 1024 * 1024) {
        dropped += queue.size() + 1; queue.clear(); queuedBytes = 0;
        disconnect = true; ++slow; return;
    }
    queuedBytes += framed.size(); queue.push_back(std::move(framed));
}
void TransportServer::run()
{
    int client = -1;
    std::vector<uint8_t> pending;
    size_t offset = 0;
    auto progress = std::chrono::steady_clock::now();
    auto closeClient = [&] {
        if (client >= 0) close(client);
        client = -1; pending.clear(); offset = 0;
        std::lock_guard<std::mutex> lock(mutex);
        connected = false; disconnect = false; queue.clear(); queuedBytes = 0;
        waitingForKeyframe.fill(true);
    };
    while (!stopping) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (disconnect) { lock.unlock(); closeClient(); }
            else if (pending.empty() && !queue.empty()) {
                pending = std::move(queue.front()); queue.pop_front();
                queuedBytes -= pending.size(); offset = 0;
                progress = std::chrono::steady_clock::now();
            }
        }
        pollfd fds[2] = {{listener, POLLIN, 0}, {client, short(POLLIN | (pending.empty() ? 0 : POLLOUT)), 0}};
        if (poll(fds, 2, 10) < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & POLLIN) {
            int next = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (next >= 0) {
                if (client >= 0) close(next); // one client in the personal prototype
                else {
                    client = next;
                    const int yes = 1, bufferSize = 256 * 1024;
                    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    setsockopt(client, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
                    std::lock_guard<std::mutex> lock(mutex);
                    connected = true; waitingForKeyframe.fill(true);
                    for (auto &request : forceKeyframe) request = true;
                    ++accepted;
                }
            }
        }
        if (client < 0) continue;
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { closeClient(); continue; }
        if (fds[1].revents & POLLIN) {
            // No client-to-host messages, input, or control protocol exists.
            char byte;
            const auto count = recv(client, &byte, 1, MSG_PEEK);
            if (count == 0 || count > 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) { closeClient(); continue; }
        }
        if (!pending.empty() && (fds[1].revents & POLLOUT)) {
            const auto n = send(client, pending.data() + offset, pending.size() - offset, MSG_NOSIGNAL);
            if (n > 0) {
                offset += size_t(n); progress = std::chrono::steady_clock::now();
                if (offset == pending.size()) { pending.clear(); offset = 0; }
            } else if (n < 0 && errno != EAGAIN && errno != EINTR) { closeClient(); continue; }
        }
        if (!pending.empty() && std::chrono::steady_clock::now() - progress > std::chrono::seconds(2)) {
            ++slow; closeClient();
        }
    }
    closeClient();
}
}
