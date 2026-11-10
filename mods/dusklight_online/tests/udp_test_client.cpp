#include "dusklight_online/net/udp_connection.hpp"
#include <array>
#include <cstring>
#include <memory>
using dusklight_online::net::UdpConnection;
struct Client { UdpConnection connection; UdpConnection::Id peer; uint16_t port; };
#if defined(_WIN32)
#define API extern "C" __declspec(dllexport)
#else
#define API extern "C" __attribute__((visibility("default")))
#endif
API void* udp_test_create(int port, const char* inviteSecret) {
    try {
        auto client = std::make_unique<Client>();
        if (!client->connection.open("127.0.0.1", 0, 1, false, false,
                                     inviteSecret ? inviteSecret : "")) return nullptr;
        client->connection.test_raw_tunnels(true);
        client->port = static_cast<uint16_t>(port);
        client->peer = client->connection.connect("127.0.0.1", static_cast<uint16_t>(port));
        if (client->peer == UdpConnection::invalid) return nullptr;
        return client.release();
    } catch (...) { return nullptr; }
}
API void udp_test_destroy(void* ptr) { delete static_cast<Client*>(ptr); }
API int udp_test_poll(void* ptr) {
    try {
        auto& c = *static_cast<Client*>(ptr); c.connection.poll();
        return c.connection.alive(c.peer) ? (c.connection.connected(c.peer) ? 1 : 0) : -1;
    } catch (...) { return -1; }
}
API int udp_test_send(void* ptr, const char* bytes, int size) {
    try { auto& c = *static_cast<Client*>(ptr); return c.connection.send(c.peer, bytes, size) ? size : -1; }
    catch (...) { return -1; }
}
API int udp_test_receive(void* ptr, char* bytes, int size) {
    try { auto& c = *static_cast<Client*>(ptr); return c.connection.receive(c.peer, bytes, size); }
    catch (...) { return 0; }
}
API int udp_test_send_realtime(void* ptr, const uint8_t* bytes, int size) {
    try {
        if (size < 0) return -1;
        auto& c = *static_cast<Client*>(ptr);
        UdpConnection::Address address{};
        const std::array<uint8_t, 4> loopback{127, 0, 0, 1};
        std::memcpy(&address.ipv4, loopback.data(), loopback.size());
        address.port = c.port;
        return c.connection.send_realtime(address, {bytes, static_cast<size_t>(size)}) ? size : -1;
    } catch (...) { return -1; }
}
API int udp_test_receive_realtime(void* ptr, uint8_t* bytes, int size) {
    try {
        if (size < 0) return -1;
        auto& c = *static_cast<Client*>(ptr);
        UdpConnection::Address address{};
        return c.connection.receive_realtime(address, {bytes, static_cast<size_t>(size)});
    } catch (...) { return -1; }
}
#include "dusklight_online/net/reliable_json.hpp"
API int udp_test_decode(const char* input, int size, char* output, int capacity) {
    try {
        auto text = dusklight_online::net::decode_reliable_json(std::string_view(input, size)).dump();
        if (text.size() > static_cast<size_t>(capacity)) return -1;
        std::copy(text.begin(), text.end(), output);
        return static_cast<int>(text.size());
    } catch (...) { return -1; }
}
