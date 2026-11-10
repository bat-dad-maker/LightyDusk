#include "dusklight_online/net/udp_connection.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>

using dusklight_online::net::UdpConnection;

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
template<class Predicate>
bool wait_until(Predicate predicate, uint32_t milliseconds = 3000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(milliseconds);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}
}

int main() {
    constexpr auto key = "EzGfNsoU3e4V2sUdrS69Qv6jD8rNQ7gP";
    UdpConnection server, wrong, client;
    uint16_t port = 0;
    for (uint32_t candidate = 39000; candidate < 39100; ++candidate) {
        if (server.open("127.0.0.1", static_cast<uint16_t>(candidate), 4,
                        true, false, key)) {
            port = static_cast<uint16_t>(candidate);
            break;
        }
    }
    require(port != 0, "secure host opened");
    require(wrong.open("127.0.0.1", 0, 1, false, false,
                       "a different long secret key"), "wrong-key client opened");
    const auto wrongId = wrong.connect("127.0.0.1", port);
    require(wrongId != UdpConnection::invalid, "wrong-key UDP candidate created");
    UdpConnection::Address acceptedAddress{};
    require(!wait_until([&] { return wrong.connected(wrongId) ||
        server.accept(acceptedAddress) != UdpConnection::invalid; }, 700),
        "wrong invite key cannot be admitted");

    require(client.open("127.0.0.1", 0, 1, false, false, key), "client opened");
    const auto clientId = client.connect("127.0.0.1", port);
    require(clientId != UdpConnection::invalid, "client UDP candidate created");
    UdpConnection::Id serverId = UdpConnection::invalid;
    require(wait_until([&] {
        if (serverId == UdpConnection::invalid) serverId = server.accept(acceptedAddress);
        return client.connected(clientId) && serverId != UdpConnection::invalid &&
               server.connected(serverId);
    }), "matching invite key establishes both directions");

    constexpr std::string_view message = "{\"type\":\"save_snapshot\",\"sample\":42}\n";
    require(client.send(clientId, message.data(), message.size()), "reliable send queued");
    std::array<char, 256> received{};
    require(wait_until([&] {
        return server.receive(serverId, received.data(), received.size()) > 0;
    }), "encrypted reliable payload delivered");
    require(std::string_view(received.data()) == message, "reliable payload intact");

    std::array<uint8_t, 58> realtime{};
    realtime[0] = 'D'; realtime[1] = 'M'; realtime[2] = 'P'; realtime[3] = 'U';
    realtime[4] = 1;
    UdpConnection::Address address{acceptedAddress.ipv4, port};
    // The receiver address returned by accept is the client's endpoint; the
    // sender uses the local host's loopback address and listening port.
    const std::array<uint8_t, 4> loopback{127, 0, 0, 1};
    std::memcpy(&address.ipv4, loopback.data(), loopback.size());
    require(client.send_realtime(address, realtime), "realtime send queued");
    UdpConnection::Address source{};
    std::array<uint8_t, 256> visual{};
    UdpConnection::Id authenticatedPeer = UdpConnection::invalid;
    require(wait_until([&] {
        return server.receive_realtime(source, visual, &authenticatedPeer) == 58;
    }),
            "encrypted realtime payload delivered");
    require(authenticatedPeer == serverId, "realtime payload bound to secure peer");
    require(std::equal(realtime.begin(), realtime.end(), visual.begin()),
            "realtime payload intact");

    // A rebound visual endpoint may differ from the reliable endpoint. Its
    // packet classification must survive sealing so routing uses that port.
    const auto visualSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    require(visualSocket != INVALID_SOCKET, "visual socket opened");
#else
    require(visualSocket >= 0, "visual socket opened");
#endif
    sockaddr_in rebound{};
    rebound.sin_family = AF_INET;
    rebound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(visualSocket, reinterpret_cast<sockaddr*>(&rebound), sizeof(rebound)) == 0,
            "visual socket bound");
#if defined(_WIN32)
    int reboundSize = sizeof(rebound);
    DWORD timeout = 1000;
#else
    socklen_t reboundSize = sizeof(rebound);
    timeval timeout{1, 0};
#endif
    require(getsockname(visualSocket, reinterpret_cast<sockaddr*>(&rebound), &reboundSize) == 0,
            "visual socket port read");
    setsockopt(visualSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    UdpConnection::Address reboundAddress{rebound.sin_addr.s_addr, ntohs(rebound.sin_port)};
    require(server.bind_realtime(serverId, reboundAddress), "visual endpoint rebound");
    require(server.send_realtime(reboundAddress, realtime), "rebound visual queued");
    std::array<uint8_t, 256> sealed{};
    const int sealedSize = recvfrom(visualSocket, reinterpret_cast<char*>(sealed.data()),
                                    static_cast<int>(sealed.size()), 0, nullptr, nullptr);
    require(sealedSize == static_cast<int>(realtime.size() + 36) &&
            std::memcmp(sealed.data(), "DSE1", 4) == 0,
            "encrypted visual routed to rebound port");
#if defined(_WIN32)
    closesocket(visualSocket);
#else
    close(visualSocket);
#endif
    std::cout << "secure UDP connection passed\n";
}
