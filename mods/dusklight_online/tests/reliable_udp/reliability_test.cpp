#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
using Socket = int;
#endif
#include "dusklight_online/net/reliable_udp.hpp"
#include "dusklight_online/net/datagram_scheduler.hpp"
#include "dusklight_online/net/udp_connection.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <random>
using namespace dusklight_online::net;
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
using Bytes = std::vector<uint8_t>;
Bytes payload(size_t size);
struct Simulated : DatagramTransport {
    std::deque<Bytes> packets;
    unsigned sends = 0;
    bool faults = true;
    bool blackhole = false;
    size_t chargedBytes = 0;
    bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) override {
        require(peer == 7, "carrier received an address instead of logical peer");
        require(bytes.size() + kSealedDatagramOverhead <= kIceDatagramBytes,
                "sealed reliable packet exceeds ICE limit");
        ++sends;
        chargedBytes += bytes.size() + 64;
        if (blackhole || (faults && sends % 7 == 0)) return false;
        Bytes packet(bytes.begin(), bytes.end());
        if (faults && sends % 3 == 0) packets.push_front(packet);
        else packets.push_back(packet);
        if (faults && sends % 5 == 0) packets.push_back(packet);
        return true;
    }
    void deliver(ReliableUdp& destination) {
        while (!packets.empty()) {
            require(destination.input(7, packets.front()), "valid packet rejected");
            packets.pop_front();
        }
    }
};
void coexistence() {
    Simulated wireA, wireB;
    DatagramScheduler::Limits limits;
    limits.perPeerBytesPerSecond = limits.totalBytesPerSecond = 64 * 1024;
    DatagramScheduler budgetA(wireA, limits), budgetB(wireB, limits);
    require(budgetA.add_peer(7) && budgetB.add_peer(7), "scheduler admission failed");
    ReliableUdp a(budgetA), b(budgetB);
    require(a.add_peer(7, 200) && b.add_peer(7, 200), "coexistence admission failed");
    auto save = payload(512 * 1024);
    require(a.send(7, save), "coexistence save queue failed");
    Bytes pose(400, 0);
    std::copy_n("DMPU", 4, pose.begin());
    require(!a.input(7, pose), "realtime entered reliable decoder");
    unsigned poses = 0;
    uint32_t lastPose = 0, maxGap = 0;
    bool complete = false;
    for (uint32_t now = 0; now < 180000; now += 10) {
        if (now % 50 == 0) require(budgetA.send(7, pose), "pose queue rejected");
        a.update(now); b.update(now);
        budgetA.update(now); budgetB.update(now);
        auto realtime = [&](LogicalPeerId id, std::span<const uint8_t> bytes) {
            require(id == 7 && bytes.size() == 400, "misrouted realtime");
            maxGap = std::max(maxGap, now - lastPose); lastPose = now; ++poses;
            return true;
        };
        while (!wireA.packets.empty()) {
            require(dispatch_datagram(7, wireA.packets.front(), b, realtime), "dispatch rejected valid packet");
            wireA.packets.pop_front();
        }
        while (!wireB.packets.empty()) {
            require(dispatch_datagram(7, wireB.packets.front(), a, realtime), "ACK misrouted");
            wireB.packets.pop_front();
        }
        require(wireA.chargedBytes <= size_t(now) * limits.totalBytesPerSecond / 1000 + 4224,
            "aggregate outbound budget exceeded");
        ReliableUdp::Message message;
        if (b.pop(message)) {
            require(!complete && message.bytes == save, "save changed or duplicated under mixed traffic");
            complete = true;
        }
        require(!a.failed(7) && !b.failed(7), "mixed traffic stalled reliable stream");
        if (complete && a.drained(7)) break;
    }
    require(complete && a.drained(7), "bulk traffic starved");
    require(poses > 20 && maxGap < 1000, "realtime traffic starved");
    require(!dispatch_datagram(7, Bytes{1,2,3}, b,
        [](auto, auto) { throw std::runtime_error("invalid datagram dispatched"); return true; }),
        "invalid datagram accepted");
    budgetA.remove_peer(7);
    require(!budgetA.send(7, pose), "removed peer retained queue");
    std::cout << "Mixed traffic passed; maximum pose arrival gap=" << maxGap << " ms\n";
}
struct NativeUdp : DatagramTransport {
    Socket socket;
    size_t sent = 0, received = 0, errors = 0;
    sockaddr_in remote{};
    NativeUdp() {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(socket != static_cast<Socket>(-1), "UDP socket failed");
        int bufferSize = 1024 * 1024;
        setsockopt(socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferSize), sizeof(bufferSize));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "UDP bind failed");
#if defined(_WIN32)
        u_long nonblocking = 1;
        require(ioctlsocket(socket, FIONBIO, &nonblocking) == 0, "nonblocking failed");
#else
        require(fcntl(socket, F_SETFL, O_NONBLOCK) == 0, "nonblocking failed");
#endif
    }
    ~NativeUdp() {
        std::cout << "UDP sent=" << sent << " received=" << received << " errors=" << errors << std::endl;
#if defined(_WIN32)
        closesocket(socket);
#else
        close(socket);
#endif
    }
    void pair(NativeUdp& other) {
#if defined(_WIN32)
        int length = sizeof(remote);
#else
        socklen_t length = sizeof(remote);
#endif
        require(getsockname(other.socket, reinterpret_cast<sockaddr*>(&remote), &length) == 0, "getsockname failed");
    }
    bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) override {
        require(peer == 7, "unknown logical peer");
        bool ok = sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == static_cast<int>(bytes.size());
        if (ok) ++sent; else ++errors;
        return ok;
    }
    void deliver(ReliableUdp& destination) {
        std::array<uint8_t, 2048> bytes{};
        for (;;) {
            sockaddr_in from{};
#if defined(_WIN32)
            int length = sizeof(from);
#else
            socklen_t length = sizeof(from);
#endif
            int count = recvfrom(socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                reinterpret_cast<sockaddr*>(&from), &length);
            if (count < 0) return;
            ++received;
            require(from.sin_addr.s_addr == remote.sin_addr.s_addr && from.sin_port == remote.sin_port,
                "unadmitted UDP source");
            require(destination.input(7, std::span(bytes).first(count)), "UDP input rejected");
        }
    }
};
Bytes payload(size_t size) {
    Bytes bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(i * 31 + i / 17);
    return bytes;
}
template<class Carrier>
void transfer(Carrier& ab, Carrier& ba, size_t size, bool realtime) {
    ReliableUdp a(ab), b(ba);
    require(a.add_peer(7, 123) && b.add_peer(7, 123), "admit failed");
    auto snapshot = payload(size);
    Bytes after{3,2,1}, before{9,8,7};
    require(a.send(7, before) && a.send(7, snapshot) && a.send(7, after), "queue failed");
    std::vector<Bytes> received;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t now = 0; now < (realtime ? 10000U : 180000U);) {
        a.update(now); b.update(now);
        // Simulation queues hold outbound packets; sockets hold inbound packets.
        if (realtime) { ab.deliver(a); ba.deliver(b); }
        else { ab.deliver(b); ba.deliver(a); }
        ReliableUdp::Message message;
        while (b.pop(message)) received.push_back(std::move(message.bytes));
        require(!a.failed(7) && !b.failed(7), "transfer failed");
        if (received.size() == 3 && a.drained(7)) break;
        if (realtime) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            now = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        } else now += 10;
    }
    require(received == std::vector<Bytes>{before, snapshot, after}, "loss/duplicate/order/large-frame failure");
    require(a.drained(7), "ACKs did not drain send queue");
}

void overseas_recovery() {
    struct Delayed : DatagramTransport {
        struct Packet { uint32_t due; Bytes bytes; };
        std::deque<Packet> packets;
        std::mt19937 random;
        uint32_t now = 0;
        explicit Delayed(unsigned seed) : random(seed) {}
        bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) override {
            require(peer == 7 && bytes.size() <= 2048, "invalid delayed datagram");
            if (random() % 100 < 10) return true; // independent data/ACK/pose loss
            packets.push_back({now + 300, {bytes.begin(), bytes.end()}});
            return true;
        }
        void deliver(ReliableUdp& target) {
            while (!packets.empty() && packets.front().due <= now) {
                require(dispatch_datagram(7, packets.front().bytes, target,
                    [](auto, auto) { return true; }), "delayed packet rejected");
                packets.pop_front();
            }
        }
    } ab(1), ba(1001);
    DatagramScheduler budgetA(ab), budgetB(ba);
    ReliableUdp sender(budgetA), receiver(budgetB);
    require(budgetA.add_peer(7) && budgetB.add_peer(7) &&
        sender.add_peer(7, 777) && receiver.add_peer(7, 777), "overseas admission");
    Bytes pose(400);
    std::copy_n("DMPU", 4, pose.begin());
    std::deque<std::pair<uint32_t, Bytes>> expected;
    std::vector<uint32_t> latencies;
    for (uint32_t now = 0; now < 60000 && latencies.size() < 40; now += 5) {
        ab.now = ba.now = now;
        ab.deliver(receiver); ba.deliver(sender);
        if (now % 50 == 0) {
            Bytes event(256, 0xA5);
            for (unsigned i = 0; i < 4; ++i) event[i] = uint8_t(now >> (8*i));
            require(sender.send(7, event), "overseas event rejected");
            expected.emplace_back(now, event);
            require(budgetA.send(7, pose) && budgetB.send(7, pose), "overseas pose rejected");
        }
        sender.update(now); receiver.update(now);
        budgetA.update(now); budgetB.update(now);
        ReliableUdp::Message message;
        while (receiver.pop(message)) {
            require(!expected.empty() && message.bytes == expected.front().second,
                "overseas event missing, duplicated or reordered");
            auto sent = expected.front().first;
            if (sent >= 5000 && sent < 7000) latencies.push_back(now - sent);
            expected.pop_front();
        }
        require(!sender.failed(7) && !receiver.failed(7), "overseas connection failed");
    }
    require(latencies.size() == 40, "overseas measured events did not all arrive");
    std::sort(latencies.begin(), latencies.end());
    std::cout << "600 ms RTT / 10% independent loss: event p95=" << latencies[37] << " ms" << std::endl;
    require(latencies[37] <= 3000, "small-event recovery window stalled overseas gameplay");
}

void late_realtime_arrival() {
    uint16_t port;
    { NativeUdp reserve; reserve.pair(reserve); port = ntohs(reserve.remote.sin_port); }
    UdpConnection receiver;
    require(receiver.open("127.0.0.1", port, 1, true), "late arrival receiver open");
    NativeUdp sender;
    sender.remote.sin_family = AF_INET;
    sender.remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sender.remote.sin_port = htons(port);
    Bytes pose(100);
    std::copy_n("DMPU", 4, pose.begin());
    std::array<uint8_t, 2048> bytes{};
    UdpConnection::Address from;
    for (int i = 0; i < 3000; ++i) {
        receiver.poll();
        // Arrival between the game update's initial poll and pose consumption.
        // sendto success does not guarantee even loopback delivery is already
        // readable. Consume without another application poll/game tick, allowing
        // asynchronous arrival but requiring service within one 30 Hz frame.
        require(sender.send(7, pose), "late arrival send");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        int count;
        do {
            count = receiver.receive_realtime(from, bytes);
        } while (count < 0 && std::chrono::steady_clock::now() < deadline);
        require(count == static_cast<int>(pose.size()),
                "late pose not serviced within the frame budget");
        require(std::equal(pose.begin(), pose.end(), bytes.begin()), "late pose corrupted");
    }
    std::cout << "Late realtime arrival: 3000 within-frame reads passed\n";
}
void sessions() {
    uint16_t port;
    { NativeUdp reserve; reserve.pair(reserve); port = ntohs(reserve.remote.sin_port); }
    UdpConnection server, client;
    require(server.open("127.0.0.1", port, 7, true), "server open failed");
    require(client.open("127.0.0.1", 0, 1, false), "client open failed");
    auto outbound = client.connect("127.0.0.1", port);
    require(outbound != UdpConnection::invalid, "session connect failed");
    UdpConnection::Id inbound = UdpConnection::invalid;
    UdpConnection::Address address;
    auto data = payload(128 * 1024);
    bool sent = false;
    Bytes received;
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(8)) {
        server.poll(); client.poll();
        if (inbound == UdpConnection::invalid) inbound = server.accept(address);
        if (!sent && client.connected(outbound)) {
            require(client.send(outbound, reinterpret_cast<char*>(data.data()), data.size()), "session send failed");
            sent = true;
        }
        if (inbound != UdpConnection::invalid) {
            std::array<char, 4096> bytes;
            int n;
            while ((n = server.receive(inbound, bytes.data(), bytes.size())) > 0)
                received.insert(received.end(), bytes.data(), bytes.data() + n);
        }
        if (received.size() == data.size() && client.drained(outbound)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(sent && received == data && client.drained(outbound), "session transfer failed");
    Bytes pose(100); std::copy_n("DMPU", 4, pose.begin());
    require(server.send_realtime(address, pose), "shared realtime send failed");
    bool gotPose = false;
    for (int i = 0; i < 100; ++i) {
        server.poll(); client.poll();
        std::array<uint8_t, 2048> bytes;
        UdpConnection::Address from;
        if (client.receive_realtime(from, bytes) == 100) { gotPose = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(gotPose, "shared realtime receive failed");
    require(client.send(outbound, "loading", 7), "loading marker send failed");
    // Neither application calls poll while the game would be loading.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    require(client.drained(outbound), "ACK servicing stopped with game updates");
    std::array<char, 8> loading{};
    require(server.receive(inbound, loading.data(), loading.size()) == 7 &&
        std::string(loading.data(), 7) == "loading", "background servicing lost message");
    client.disconnect(outbound);
    for (int i = 0; i < 100 && server.alive(inbound); ++i) {
        server.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!server.alive(inbound), "disconnect not observed");
    server.disconnect(inbound);
    std::cout << "Native session and shared realtime path passed\n";
}
int main() {
    try {
#if defined(_WIN32)
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2,2), &data) == 0, "Winsock failed");
#endif
        Simulated ab, ba;
        std::cout << "Starting lossy transfer" << std::endl;
        transfer(ab, ba, ReliableUdp::maxMessageBytes, false);
        std::cout << "Starting native UDP transfer" << std::endl;
        {
            NativeUdp udpA, udpB;
            udpA.pair(udpB); udpB.pair(udpA);
            transfer(udpA, udpB, 256 * 1024, true);
        }
        Simulated output;
        std::cout << "Starting boundary tests" << std::endl;
        output.faults = false;
        ReliableUdp sender(output), receiver(output);
        require(sender.add_peer(7, 10) && receiver.add_peer(7, 10), "admission failed");
        require(sender.send(7, payload(65536)), "snapshot queue failed");
        sender.update(0); sender.update(100);
        require(!output.packets.empty(), "no initial packet");
        Bytes old = output.packets.front();
        require(!receiver.input(8, old), "unknown peer accepted");
        Bytes malformed = old; malformed.push_back(0);
        require(!receiver.input(7, malformed), "malformed compound accepted");
        require(receiver.input(7, old), "first fragment rejected");
        receiver.update(100);
        ReliableUdp::Message message;
        require(!receiver.pop(message), "partial snapshot escaped");
        receiver.remove_peer(7);
        require(receiver.add_peer(7, 11), "new generation failed");
        require(!receiver.input(7, old), "old generation replay accepted");
        require(!sender.send(7, payload(ReliableUdp::maxMessageBytes + 1)), "oversized frame accepted");
        size_t admitted = 0;
        while (sender.send(7, payload(1024 * 1024))) require(++admitted < 9, "unbounded send queue");
        require(admitted > 0, "queue unexpectedly full");
        Simulated lost; lost.blackhole = true;
        ReliableUdp doomed(lost);
        require(doomed.add_peer(7, 1) && doomed.send(7, Bytes{1}), "blackhole setup failed");
        for (uint32_t now = 0; now < 4000000 && !doomed.failed(7); now += 100) doomed.update(now);
        require(doomed.failed(7) && !doomed.drained(7), "retry exhaustion silently succeeded");
        coexistence();
        overseas_recovery();
        late_realtime_arrival();
        sessions();
        std::cout << "Reliability: lossy ordered 2 MiB transfer, native UDP, framing, bounds, replay and failure passed\n";
#if defined(_WIN32)
        WSACleanup();
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
