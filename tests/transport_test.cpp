#include "transport/tcp_server.h"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

static void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<typename F> static void waitUntil(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    require(predicate(), "Timed out waiting for transport state");
}
static int connectTo(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    require(fd >= 0, "socket failed");
    int small = 1024; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "connect failed");
    return fd;
}
int main() {
    try {
        quest::TransportServer server(0);
        int client = connectTo(server.port());
        waitUntil([&] { return server.connections() == 1; });
        require(server.takeKeyframeRequest(0) && server.takeKeyframeRequest(1) && server.takeKeyframeRequest(2), "Missing IDR requests");
        server.publish({0, 10, false, {0, 0, 1, 0x41}});
        pollfd fd{client, POLLIN, 0};
        require(poll(&fd, 1, 30) == 0, "Predictive frame sent before keyframe");
        server.publish({0, 20, true, {0, 0, 1, 0x65}});
        require(poll(&fd, 1, 1000) == 1, "No initial frame");
        uint8_t bytes[36]; size_t read = 0;
        while (read < sizeof(bytes)) {
            auto n = recv(client, bytes + read, sizeof(bytes) - read, 0);
            require(n > 0, "Short TCP frame"); read += n;
        }
        require(std::string(reinterpret_cast<char *>(bytes), 4) == "QSTV" && bytes[8] == 10 && bytes[9] == 0 && bytes[23] == 20 && bytes[27] == 4 && bytes[31] == 1,
                "Incorrect wire serialization");
        for (int i = 0; i < 40; ++i)
            server.publish({0, 21+i, false, std::vector<uint8_t>(1024*1024, 0x41)});
        waitUntil([&] { return server.slowDisconnects() > 0; });
        close(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client = connectTo(server.port());
        waitUntil([&] { return server.connections() == 2; });
        require(server.takeKeyframeRequest(0), "Reconnect did not request a fresh IDR");
        close(client);
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
