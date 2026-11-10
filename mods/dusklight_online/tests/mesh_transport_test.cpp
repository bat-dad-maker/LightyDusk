#include "dusklight_online/net/udp_connection.hpp"
#include "dusklight_online/net/peer_tunnel.hpp"
#include "dusklight_online/net/udp_codec.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string_view>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif
using namespace dusklight_online::net;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* text) {
    if (!ok) { std::cerr << text << '\n'; std::exit(1); }
}
uint16_t port() {
    auto fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "reserve port");
#if defined(_WIN32)
    int size = sizeof(address);
#else
    socklen_t size = sizeof(address);
#endif
    require(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "read port");
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
    return ntohs(address.sin_port);
}
struct Network {
    static constexpr size_t count = 8;
    static constexpr std::string_view inviteSecret = "manual-relay-test-endpoint-key-2026";
    std::array<UdpConnection, count> clients;
    UdpConnection relay;
    std::array<UdpConnection::Id, count> relayClients{}, clientRelay{};
    std::array<UdpConnection::Address, count> addresses{};
    std::array<std::array<UdpConnection::Id, count>, count> links{};
    struct Pending { Clock::time_point due; size_t target; std::vector<uint8_t> bytes; };
    std::deque<Pending> pending;
    size_t forwarded = 0;
    size_t reflexiveCandidates = 0;
    uint32_t delay = 0;
    bool loss = false;
    static std::string name(size_t i) { return "client_" + std::to_string(i + 1); }
    void pump(bool signals) {
        std::array<uint8_t, 2048> bytes{};
        UdpConnection::Address from;
        for (int n = 0; n < 512; ++n) {
            int size = relay.receive_realtime(from, bytes);
            if (size < 0) break;
            auto packet = std::span<const uint8_t>(bytes).first(size);
            require(is_peer_tunnel(packet), "unexpected fallback wire");
            const size_t source = tunnel_read(packet.subspan(4, 8)) - 1;
            const size_t target = tunnel_read(packet.subspan(12, 8)) - 1;
            require(source < count && target < count && source != target, "fallback recipient bounds");
            require(addresses[source].ipv4 == from.ipv4 && addresses[source].port == from.port, "fallback source binding");
            ++forwarded;
            if (loss && forwarded % 50 == 0) continue;
            Pending p{Clock::now() + std::chrono::milliseconds(delay + (forwarded % 7)), target, {packet.begin(), packet.end()}};
            pending.push_back(p);
            if (loss && forwarded % 17 == 0) pending.push_back(p);
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->due > Clock::now()) { ++it; continue; }
            require(relay.send_realtime(addresses[it->target], it->bytes), "fallback budget enqueue");
            it = pending.erase(it);
        }
        if (signals) for (size_t i = 0; i < count; ++i) {
            IceAgent::Signal signal; std::string target;
            while (clients[i].mesh_pop_signal(target, signal)) {
                if (signal.kind == IceAgent::Signal::Candidate && signal.text.find("typ srflx") != std::string::npos)
                    ++reflexiveCandidates;
                require(clients[peer_number(target) - 1].mesh_signal(name(i), signal), "ICE signaling rejected");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    template<class F> void until(F predicate, bool signals, int timeout = 20000) {
        auto end = Clock::now() + std::chrono::milliseconds(timeout);
        do { pump(signals); if (predicate()) return; } while (Clock::now() < end);
        require(false, "mesh deadline exceeded");
    }
    Network() {
        const uint16_t p = port();
        require(relay.open("127.0.0.1", p, 16, true, true, inviteSecret), "relay open");
        for (size_t i = 0; i < count; ++i) {
            require(clients[i].open("0.0.0.0", 0, 1, false, false, inviteSecret), "client open");
            clientRelay[i] = clients[i].connect("127.0.0.1", p);
            const auto end = Clock::now() + std::chrono::seconds(4);
            UdpConnection::Id accepted = UdpConnection::invalid;
            while (Clock::now() < end && accepted == UdpConnection::invalid) {
                accepted = relay.accept(addresses[i]); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(accepted != UdpConnection::invalid, "relay accept"); relayClients[i] = accepted;
            while (!clients[i].connected(clientRelay[i]) && Clock::now() < end)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            require(clients[i].connected(clientRelay[i]), "client connect");
            // One peer's STUN endpoint is deliberately unreachable. Host
            // candidates and relay fallback must still remain usable.
            require(clients[i].mesh_open(name(i), clientRelay[i], "127.0.0.1", i == 0 ? port() : p), "mesh open");
        }
        for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < count; ++j) if (i != j) {
            links[i][j] = clients[i].mesh_admit(name(j), true);
            require(links[i][j] != UdpConnection::invalid, "admit eight-player mesh");
        }
    }
    void transfer(bool signals, bool fanout = false) {
        std::string payload(43 * 1024, 'x');
        uint32_t random = 123;
        for (auto& c : payload) { random = random * 1664525U + 1013904223U; c = char(random >> 24); }
        payload += "first-event-after-sync";
        std::array<std::string, count> received;
        if (fanout) {
            for (size_t i=1;i<count;++i) require(clients[0].send(links[0][i],payload.data(),payload.size()),"queue seven-recipient catchup");
        } else for (size_t i = 0; i < count; ++i)
            require(clients[i].send(links[i][(i + 1) % count], payload.data(), payload.size()), "queue reliable catchup");
        until([&] {
            bool done = true;
            std::array<char, 4096> bytes{};
            for (size_t i = 0; i < count; ++i) {
                int n;
                if (fanout && i==0) continue;
                while ((n = clients[i].receive(links[i][fanout ? 0 : (i + count - 1) % count], bytes.data(), bytes.size())) > 0)
                    received[i].append(bytes.data(), n);
                require(received[i].size() <= payload.size(), "duplicate reliable delivery");
                done = done && received[i] == payload;
            }
            return done;
        }, signals, 45000);
    }
};
int main() {
#if defined(_WIN32)
    WSADATA data{}; require(WSAStartup(MAKEWORD(2,2), &data) == 0, "Winsock");
#endif
    require(peer_number("client_1junk") == 0 && peer_number("client_0") == 0, "identity parser");
    Network net;
    net.delay = 140; net.loss = true;
    net.transfer(false); // all fallback: 280 ms RTT, loss, reorder and duplication
    std::cout << "eight-player relayed catchups: exact bytes, no duplicates\n";
    net.delay = 280;
    net.transfer(false); // two 140 ms one-way relay legs: 560 ms peer RTT
    std::cout << "560 ms peer RTT catchups: exact bytes, no duplicates\n";
    const auto fanoutStart = Clock::now();
    net.transfer(false,true);
    std::cout << "seven-recipient 43 KiB fallback fanout at 560 ms peer RTT: exact bytes; ms="
              << std::chrono::duration<double,std::milli>(Clock::now()-fanoutStart).count() << "\n";
    net.until([&] {
        for (size_t i = 0; i < net.count; ++i) for (size_t j = 0; j < net.count; ++j)
            if (i != j && !net.clients[i].mesh_direct(net.name(j))) return false;
        return true;
    }, true);
    net.transfer(true);
    require(net.reflexiveCandidates > 0, "libjuice must accept shared-port STUN discovery");
    std::cout << "56 directed ICE links established; direct catchups passed\n";
    require(net.clients[0].mesh_retry(net.name(1)), "restart first ICE pair");
    net.transfer(false); // one broken direct pair, others remain direct
    require(!net.clients[0].mesh_direct(net.name(1)), "restarted pair must use fallback");
    net.until([&] { return net.clients[0].mesh_direct(net.name(1)) && net.clients[1].mesh_direct(net.name(0)); }, true);
    net.transfer(true);
    // Realtime and reliable packets share the same carrier without decoder confusion.
    auto pose = udp::encode_message({{"type", "pose"}, {"sequence", 1}, {"x", 42}}, net.name(0));
    require(!pose.empty(), "encode pose");
    for (const auto& p : pose) require(net.clients[0].mesh_send(net.name(1), p.bytes), "send pose");
    udp::Decoder decoder;
    net.until([&] {
        std::array<uint8_t, 1200> bytes{}; std::string source;
        int n = net.clients[1].mesh_receive(source, bytes);
        if (n < 0) return false;
        auto result = decoder.accept(std::span<const uint8_t>(bytes).first(n));
        return source == net.name(0) && result.kind == udp::DecodeKind::Message && result.message.value("x", 0) == 42;
    }, true);
    net.clients[1].mesh_remove(net.name(0));
    require(!net.clients[1].mesh_signal(net.name(0), {IceAgent::Signal::Done, {}}), "departed peer signaling rejected");
    require(!net.clients[1].mesh_send(net.name(0), pose.front().bytes), "departed peer send rejected");
    std::cout << "ICE restart/fallback/recovery, realtime demux and departure passed\n";
}
