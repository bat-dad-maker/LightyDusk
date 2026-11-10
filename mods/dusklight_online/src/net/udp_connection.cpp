#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
using NativeSocket = int;
#endif
#include "dusklight_online/net/udp_connection.hpp"
#include "dusklight_online/net/datagram_scheduler.hpp"
#include "dusklight_online/net/peer_tunnel.hpp"
#include "dusklight_online/net/secure_datagram.hpp"
#include "dusklight_online/net/stun_binding.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace dusklight_online::net {
namespace {
constexpr NativeSocket badSocket = static_cast<NativeSocket>(-1);
uint32_t clock_ms() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool resolve(std::string_view name, uint16_t port, sockaddr_in& address) {
    address = {}; address.sin_family = AF_INET; address.sin_port = htons(port);
    std::string host(name);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) return true;
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) || !result) return false;
    address.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result); return true;
}
uint64_t read64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}
void write64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
bool same(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
}
struct UdpConnection::Impl : DatagramTransport {
    struct Peer {
        sockaddr_in address{};
        sockaddr_in realtimeAddress{};
        uint64_t nonce = 0, session = 0;
        uint32_t started = 0, lastReceive = 0, lastControl = 0;
        bool ready = false, closed = false, accepted = false;
        uint64_t logical = 0;
        bool meshReliable = false;
        std::unique_ptr<IceAgent> ice;
        uint32_t lastProbe = 0, lastEcho = 0, probeRtt = 250;
        uint32_t lastIcePacket = 0, lastPayloadPacket = 0;
        uint64_t probeSends = 0, probeSendFailures = 0;
        uint64_t probeRequests = 0, probeReplies = 0, payloadPackets = 0;
        bool hasEcho = false;
        std::optional<SecureDatagram> secure;
        uint32_t lastSecure = 0;
        uint32_t iceGeneration = 0, retryAt = 0, retryDelay = 30000;
        uint32_t lastIceChange = 0;
        std::array<uint32_t, 3> iceSent{}, iceReceived{}, iceRejected{};
        std::string rx;
    };
    struct Raw {
        Address address;
        std::vector<uint8_t> bytes;
        Id authenticatedPeer = invalid;
    };
    mutable std::mutex mutex;
    std::condition_variable activity;
    uint64_t activityGeneration = 0;
    std::thread worker;
    std::atomic<bool> workerStop{false};
    NativeSocket socket = badSocket;
    bool stack = false, server = false;
    std::string inviteSecret;
    std::string meshSecret;
#ifdef DUSKLIGHT_TRANSPORT_TESTING
    bool rawTestTunnels = false;
#endif
    bool stun = false;
    uint32_t stunWindow = 0, stunCount = 0;
    struct StunRate { uint32_t window = 0, count = 0; };
    std::array<StunRate, 256> stunRates{};
    size_t capacity = 0;
    Id nextId = 1;
    std::map<Id, Peer> peers;
    std::deque<Id> accepted;
    std::deque<Raw> realtime;
    struct MeshRaw { std::string peer; std::vector<uint8_t> bytes; };
    std::deque<MeshRaw> meshRealtime;
    uint64_t localLogical = 0;
    Id relayId = invalid;
    std::string stunHost;
    uint16_t stunPort = 0;
    DatagramScheduler budget{*this};
    ReliableUdp reliable{budget, 4096};
    std::random_device random;
    uint64_t nonce() { uint64_t n = (uint64_t(random()) << 32) ^ random(); return n ? n : 1; }
    Id find(const sockaddr_in& address) const {
        for (const auto& [id, peer] : peers) if (!peer.logical && !peer.closed && same(peer.address, address)) return id;
        return invalid;
    }
    Id logical_peer(uint64_t logical) const {
        if (logical) for (const auto& [id, peer] : peers)
            if (!peer.closed && peer.logical == logical) return id;
        return invalid;
    }
    bool direct(const Peer& peer, uint32_t now) const {
        return peer.ice && peer.ice->connected() && peer.hasEcho &&
            (!peer.secure || peer.secure->established()) &&
            uint32_t(now - peer.lastEcho) < std::max(750U, std::min(3000U, peer.probeRtt * 4));
    }
    bool send(LogicalPeerId id, std::span<const uint8_t> bytes) override {
        auto it = peers.find(static_cast<Id>(id));
        if (it == peers.end() || it->second.closed || socket == badSocket) return false;
        if (it->second.logical) {
            auto& peer = it->second;
            if (direct(peer, clock_ms())) {
                std::vector<uint8_t> protectedBytes;
                if (peer.secure) {
                    protectedBytes = peer.secure->seal(bytes);
                    if (protectedBytes.empty()) return false;
                }
                if (peer.ice->send(peer.secure ? std::span<const uint8_t>(protectedBytes) : bytes))
                    return true;
            }
            const auto relay = peers.find(relayId);
            if (relay == peers.end() || relay->second.closed || !relay->second.ready) return false;
            auto wrapped = peer_tunnel(localLogical, peer.logical, bytes);
            if (wrapped.empty()) return false;
            return send(relayId, wrapped);
        }
        const bool isRealtime = datagram_kind(bytes) == DatagramKind::Realtime;
        std::vector<uint8_t> encrypted;
        const bool handshake = bytes.size() >= 4 &&
            (std::memcmp(bytes.data(), "DUC1", 4) == 0 ||
             std::memcmp(bytes.data(), "DSH1", 4) == 0);
        if (it->second.secure && !handshake) {
            encrypted = it->second.secure->seal(bytes);
            if (encrypted.empty()) return false;
            bytes = encrypted;
        } else if (it->second.secure && handshake &&
                   std::memcmp(bytes.data(), "DUC1", 4) == 0 &&
                   it->second.secure->established()) {
            encrypted = it->second.secure->seal(bytes);
            if (encrypted.empty()) return false;
            bytes = encrypted;
        }
        const auto& destination = isRealtime && it->second.realtimeAddress.sin_port ?
            it->second.realtimeAddress : it->second.address;
        return sendto(socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(sockaddr_in)) == static_cast<int>(bytes.size());
    }
    // Fixed-size, rate-limited session control outside reliability. Challenge
    // echoes establish return routability; game/room authentication remains in
    // the existing hello handler. They do not claim cryptographic identity.
    void control(Id id, uint8_t type) {
        auto& peer = peers.at(id);
        if (peer.logical) return;
        std::array<uint8_t, 21> bytes{'D','U','C','1',type};
        write64(bytes.data() + 5, peer.nonce); write64(bytes.data() + 13, peer.session);
        send(id, bytes);
        peer.lastControl = clock_ms();
    }
    bool ready(Id id) {
        auto& peer = peers.at(id);
        if (peer.ready) return true;
        if (!budget.add_peer(id)) return false;
        if (peer.logical && !peer.meshReliable) { peer.ready = true; return true; }
        if (!reliable.add_peer(id, peer.session)) { budget.remove_peer(id); return false; }
        peer.ready = true;
        return true;
    }
    void close_peer(Id id, bool notify) {
        auto it = peers.find(id);
        if (it == peers.end()) return;
        if (notify && it->second.ready && !it->second.closed) control(id, 6);
        it->second.closed = true;
        reliable.remove_peer(id); budget.remove_peer(id);
    }
    void close() {
        for (auto& [id, peer] : peers) {
            if (peer.ready && !peer.closed && socket != badSocket) control(id, 6);
            reliable.remove_peer(id); budget.remove_peer(id);
        }
        peers.clear(); accepted.clear(); realtime.clear(); meshRealtime.clear();
        std::fill(inviteSecret.begin(), inviteSecret.end(), '\0');
        inviteSecret.clear();
        std::fill(meshSecret.begin(), meshSecret.end(), '\0');
        meshSecret.clear();
        localLogical = 0; relayId = invalid; stunHost.clear(); stunPort = 0;
        stun = false; stunWindow = 0; stunCount = 0; stunRates = {};
        if (socket != badSocket) {
#if defined(_WIN32)
            closesocket(socket);
#else
            ::close(socket);
#endif
        }
        socket = badSocket;
#if defined(_WIN32)
        if (stack) WSACleanup();
#endif
        stack = false;
    }
    void receive_control(const sockaddr_in& from, std::span<const uint8_t> bytes,
                         uint32_t now, bool encrypted = false) {
        if (bytes.size() != 21) return;
        uint8_t type = bytes[4]; uint64_t token = read64(bytes.data() + 5), session = read64(bytes.data() + 13);
        Id id = find(from);
        // Initial control is necessarily plaintext. Once the key exchange has
        // finished, no unauthenticated control may refresh or close a session.
        if (id != invalid && peers.at(id).secure && peers.at(id).secure->established() && !encrypted)
            return;
        if (type == 1 && server && token && session == 0) {
            if (id == invalid) {
                if (peers.size() >= capacity) return;
                id = nextId++;
                Peer peer; peer.address = from; peer.nonce = token; peer.session = nonce();
                if (!inviteSecret.empty()) {
                    peer.secure.emplace(inviteSecret, false);
                    if (!peer.secure->valid()) return;
                    peer.secure->set_session(peer.session);
                }
                peer.started = peer.lastReceive = now;
                peers.emplace(id, std::move(peer));
            }
            auto& peer = peers.at(id);
            if (peer.nonce == token && uint32_t(now - peer.lastControl) >= 250) control(id, 2);
            return;
        }
        if (id == invalid) return;
        auto& peer = peers.at(id);
        if (peer.nonce != token || peer.closed) return;
        if (type == 2 && !server && !peer.ready && session) {
            if (peer.session && peer.session != session) return;
            const bool firstChallenge = peer.session == 0;
            peer.session = session; peer.lastReceive = now;
            if (peer.secure) peer.secure->set_session(session);
            if (firstChallenge || uint32_t(now - peer.lastControl) >= 100) control(id, 3);
            return;
        }
        if (!session || peer.session != session) return;
        if (type == 3 && server) {
            const bool firstConfirm = !peer.ready;
            if (!ready(id)) { close_peer(id, false); return; }
            if ((!peer.secure || peer.secure->established()) && !peer.accepted) {
                accepted.push_back(id); peer.accepted = true;
            }
            peer.lastReceive = now;
            if (firstConfirm || uint32_t(now - peer.lastControl) >= 100) control(id, 4);
        } else if (type == 4 && !server) {
            if (!ready(id)) { close_peer(id, false); return; }
            peer.lastReceive = now;
            if (peer.secure && peer.secure->retry_packet().empty() &&
                !peer.secure->established()) {
                const auto hello = peer.secure->begin();
                if (hello.empty()) { close_peer(id, false); return; }
                send(id, hello);
                peer.lastSecure = now;
            }
        } else if (type == 5 && peer.ready) peer.lastReceive = now;
        else if (type == 6 && peer.ready) close_peer(id, false);
    }
    void receive_secure_handshake(const sockaddr_in& from,
                                  std::span<const uint8_t> bytes, uint32_t now) {
        const Id id = find(from);
        if (id == invalid) return;
        auto& peer = peers.at(id);
        if (peer.closed || !peer.ready || !peer.secure) return;
        const bool wasEstablished = peer.secure->established();
        const auto reply = peer.secure->handshake(bytes);
        if (!reply.empty()) {
            send(id, reply);
            peer.lastSecure = now;
        }
        if (!wasEstablished && peer.secure->established()) {
            peer.lastReceive = now;
            if (server && !peer.accepted) {
                accepted.push_back(id);
                peer.accepted = true;
            }
        }
    }
    void mesh_input(Id id, std::span<const uint8_t> bytes) {
        auto& peer = peers.at(id);
        if (peer.meshReliable && datagram_kind(bytes) == DatagramKind::Reliable) {
            reliable.input(id, bytes);
        } else if (bytes.size() >= 58 && std::memcmp(bytes.data(), "DMPU", 4) == 0) {
            // Bind the claimed sender BEFORE the application decoder allocates.
            size_t length = 0;
            while (length < 32 && bytes[26 + length]) ++length;
            std::string sender(reinterpret_cast<const char*>(bytes.data() + 26), length);
            if (peer_number(sender) != peer.logical || meshRealtime.size() >= 512) return;
            meshRealtime.push_back({std::move(sender), {bytes.begin(), bytes.end()}});
        }
    }
    void pump_mesh(uint32_t now) {
        for (auto& [id, peer] : peers) {
            if (!peer.logical || peer.closed || !peer.ice) continue;
            if (localLogical < peer.logical && !direct(peer, now) &&
                static_cast<int32_t>(now - peer.retryAt) >= 0) {
                peer.ice = std::make_unique<IceAgent>(stunHost, stunPort);
                ++peer.iceGeneration; peer.hasEcho = false;
                peer.lastIceChange = now;
                peer.retryDelay = std::min(120000U, peer.retryDelay * 2);
                peer.retryAt = now + peer.retryDelay;
            }
            if (direct(peer, now)) { peer.retryDelay = 30000; peer.retryAt = now + peer.retryDelay; }
            if (peer.secure && peer.ice->connected() && !peer.secure->established()) {
                if (localLogical < peer.logical && peer.secure->retry_packet().empty()) {
                    const auto hello = peer.secure->begin();
                    if (!hello.empty()) peer.ice->send(hello);
                    peer.lastSecure = now;
                } else if (!peer.secure->retry_packet().empty() &&
                           uint32_t(now - peer.lastSecure) >= 500) {
                    peer.ice->send(peer.secure->retry_packet());
                    peer.lastSecure = now;
                }
            }
            if (peer.ice->connected() && uint32_t(now - peer.lastProbe) >= 250) {
                std::array<uint8_t, 13> probe{'D','P','I','1',1};
                write64(probe.data() + 5, now);
                if (peer.ice->send(probe)) ++peer.probeSends;
                else ++peer.probeSendFailures;
                peer.lastProbe = now;
            }
            std::vector<uint8_t> bytes;
            for (size_t n = 0; n < 512 && peer.ice->pop(bytes); ++n) {
                peer.lastIcePacket = now;
                if (bytes.size() == 13 && std::memcmp(bytes.data(), "DPI1", 4) == 0) {
                    if (bytes[4] == 1) {
                        ++peer.probeRequests;
                        bytes[4] = 2;
                        peer.ice->send(bytes);
                    }
                    else if (bytes[4] == 2) {
                        const auto echoed = read64(bytes.data() + 5);
                        if (echoed <= UINT32_MAX && uint32_t(now - static_cast<uint32_t>(echoed)) < 3000) {
                            ++peer.probeReplies;
                            peer.probeRtt = uint32_t(now - static_cast<uint32_t>(echoed));
                            peer.hasEcho = true; peer.lastEcho = now;
                        }
                    }
                    continue;
                }
                if (peer.secure) {
                    if (bytes.size() >= 5 && std::memcmp(bytes.data(), "DSH1", 4) == 0) {
                        const auto reply = peer.secure->handshake(bytes);
                        if (!reply.empty()) peer.ice->send(reply);
                        continue;
                    }
                    const auto plaintext = peer.secure->open(bytes);
                    if (!plaintext) continue;
                    bytes = *plaintext;
                }
                peer.lastPayloadPacket = now;
                ++peer.payloadPackets;
                mesh_input(id, bytes);
            }
        }
    }
};
UdpConnection::UdpConnection() : impl_(std::make_unique<Impl>()) {}
UdpConnection::~UdpConnection() { close(); }
void UdpConnection::close() {
    // Join before taking the protocol lock: the worker may be inside poll().
    impl_->workerStop.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex); impl_->close();
    ++impl_->activityGeneration;
    impl_->activity.notify_all();
}
bool UdpConnection::open(std::string_view host, uint16_t port, size_t capacity, bool server,
                         bool stun, std::string_view inviteSecret) {
    close();
    std::lock_guard lock(impl_->mutex);
    if (!capacity || capacity > 4096 ||
        (!inviteSecret.empty() && inviteSecret.size() < 16)) return false;
    impl_->inviteSecret = inviteSecret;
    impl_->meshSecret = inviteSecret;
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) return false;
    impl_->stack = true;
#endif
    sockaddr_in address{};
    if (!resolve(host, port, address)) { impl_->close(); return false; }
    impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == badSocket) { impl_->close(); return false; }
#if defined(_WIN32)
    u_long enabled = 1;
    bool nonblocking = ioctlsocket(impl_->socket, FIONBIO, &enabled) == 0;
#else
    bool nonblocking = fcntl(impl_->socket, F_SETFL, O_NONBLOCK) == 0;
#endif
    int size = 1024 * 1024;
    setsockopt(impl_->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    setsockopt(impl_->socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&size), sizeof(size));
    if (!nonblocking || bind(impl_->socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        impl_->close(); return false;
    }
    impl_->capacity = capacity; impl_->server = server;
    impl_->stun = server && stun;
    impl_->budget.update(clock_ms());
    // Socket servicing must survive game loading/paused updates just as TCP's
    // kernel ACK processing did. All access to protocol state is serialized;
    // no game callbacks execute on this worker.
    const NativeSocket nativeSocket = impl_->socket;
    impl_->workerStop.store(false);
    impl_->worker = std::thread([this, nativeSocket] {
        while (!impl_->workerStop.load()) {
            poll();
            // Wake immediately for ingress. The timeout still services KCP,
            // pacing and shutdown when no datagrams arrive. close() joins this
            // worker before closing/reusing nativeSocket; never hold the
            // protocol mutex while waiting for socket readiness.
#if defined(_WIN32)
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(nativeSocket, &readable);
            timeval timeout{0, 5000};
            select(0, &readable, nullptr, nullptr, &timeout);
#else
            pollfd readable{nativeSocket, POLLIN, 0};
            ::poll(&readable, 1, 5);
#endif
        }
    });
    return true;
}
UdpConnection::Id UdpConnection::connect(std::string_view host, uint16_t port) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->socket == badSocket || impl_->server || impl_->peers.size() >= impl_->capacity) return invalid;
    Impl::Peer peer;
    if (!resolve(host, port, peer.address)) return invalid;
    peer.nonce = impl_->nonce(); peer.started = peer.lastReceive = clock_ms();
    if (!impl_->inviteSecret.empty()) {
        peer.secure.emplace(impl_->inviteSecret, true);
        if (!peer.secure->valid()) return invalid;
    }
    Id id = impl_->nextId++; impl_->peers.emplace(id, std::move(peer));
    impl_->control(id, 1); return id;
}
UdpConnection::Id UdpConnection::accept(Address& address) {
    std::lock_guard lock(impl_->mutex);
    while (!impl_->accepted.empty()) {
        Id id = impl_->accepted.front(); impl_->accepted.pop_front();
        auto it = impl_->peers.find(id);
        if (it == impl_->peers.end() || it->second.closed) continue;
        address = {it->second.address.sin_addr.s_addr, ntohs(it->second.address.sin_port)};
        return id;
    }
    return invalid;
}
void UdpConnection::disconnect(Id id) {
    std::lock_guard lock(impl_->mutex); impl_->close_peer(id, true); impl_->peers.erase(id);
}
bool UdpConnection::alive(Id id) const {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    return it != impl_->peers.end() && !it->second.closed;
}
bool UdpConnection::connected(Id id) const {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    return it != impl_->peers.end() && it->second.ready && !it->second.closed &&
        (!it->second.secure || it->second.secure->established());
}
bool UdpConnection::drained(Id id) const { std::lock_guard lock(impl_->mutex); return impl_->reliable.drained(id); }
bool UdpConnection::send(Id id, const char* bytes, size_t size) {
    std::lock_guard lock(impl_->mutex);
    return impl_->reliable.send(id, {reinterpret_cast<const uint8_t*>(bytes), size});
}
int UdpConnection::receive(Id id, char* bytes, size_t size) {
    std::lock_guard lock(impl_->mutex); auto it = impl_->peers.find(id);
    if (it == impl_->peers.end()) return 0;
    auto& peer = it->second;
    if (peer.rx.empty()) return peer.closed ? 0 : -1;
    size_t count = std::min(size, peer.rx.size());
    std::memcpy(bytes, peer.rx.data(), count); peer.rx.erase(0, count); return static_cast<int>(count);
}
bool UdpConnection::send_realtime(Address address, std::span<const uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex);
    sockaddr_in endpoint{}; endpoint.sin_addr.s_addr = address.ipv4; endpoint.sin_port = htons(address.port);
    Id id = invalid;
    for (const auto& [candidate, peer] : impl_->peers) {
        const auto& destination = peer.realtimeAddress.sin_port ? peer.realtimeAddress : peer.address;
        if (!peer.closed && same(endpoint, destination)) { id = candidate; break; }
    }
    if (id == invalid || !impl_->peers.at(id).ready) return false;
    const bool accepted = impl_->budget.send(id, bytes);
    // Flush available tokens now, rather than adding up to one worker period
    // to each visual hop. Both traffic classes still use the same scheduler.
    impl_->budget.update(clock_ms());
    return accepted;
}
bool UdpConnection::bind_realtime(Id id, Address address) {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end() || it->second.closed || !it->second.ready || !address.port) return false;
    it->second.realtimeAddress.sin_family = AF_INET;
    it->second.realtimeAddress.sin_addr.s_addr = address.ipv4;
    it->second.realtimeAddress.sin_port = htons(address.port);
    return true;
}
int UdpConnection::receive_realtime(Address& address, std::span<uint8_t> bytes,
                                    Id* authenticatedPeer) {
    std::unique_lock lock(impl_->mutex);
    if (impl_->realtime.empty()) {
        // A datagram can arrive after the caller's initial poll, before its
        // pose read. Do not defer that ready datagram to the next game frame
        // merely because the worker has not been scheduled yet. Match the
        // old recvfrom-at-consumption behavior; poll is nonblocking and still
        // owns admission, demultiplexing and the shared outbound budget.
        lock.unlock();
        poll();
        lock.lock();
        if (impl_->realtime.empty()) return -1;
    }
    auto packet = std::move(impl_->realtime.front()); impl_->realtime.pop_front();
    address = packet.address;
    if (authenticatedPeer) *authenticatedPeer = packet.authenticatedPeer;
    size_t size = std::min(bytes.size(), packet.bytes.size());
    std::copy_n(packet.bytes.begin(), size, bytes.begin()); return static_cast<int>(size);
}
void UdpConnection::poll() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->socket == badSocket) return;
    uint32_t now = clock_ms();
    std::array<uint8_t, 2049> bytes{};
    bool received = false;
    for (size_t n = 0; n < 512; ++n) {
        sockaddr_in from{};
#if defined(_WIN32)
        int length = sizeof(from);
#else
        socklen_t length = sizeof(from);
#endif
        int size = recvfrom(impl_->socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &length);
        if (size < 0) break;
        received = true;
        auto packet = std::span<const uint8_t>(bytes).first(static_cast<size_t>(size));
        if (impl_->stun && is_stun_binding(packet)) {
            // Fixed storage, global and hashed per-source budgets. A discovery
            // flood must not allocate peers or consume unbounded response work.
            if (uint32_t(now - impl_->stunWindow) >= 1000) { impl_->stunWindow = now; impl_->stunCount = 0; }
            uint32_t key = ntohl(from.sin_addr.s_addr);
            key ^= key >> 16; key *= 0x7feb352dU; key ^= key >> 15;
            auto& rate = impl_->stunRates[key % impl_->stunRates.size()];
            if (uint32_t(now - rate.window) >= 1000) { rate.window = now; rate.count = 0; }
            if (impl_->stunCount >= 1024 || rate.count >= 64) continue;
            ++impl_->stunCount; ++rate.count;
            const auto reply = stun_binding_reply(packet, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
            if (!reply.empty()) sendto(impl_->socket, reinterpret_cast<const char*>(reply.data()),
                static_cast<int>(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), sizeof(from));
            continue;
        }
        std::optional<std::vector<uint8_t>> decrypted;
        bool encrypted = false;
        Id authenticatedPeer = invalid;
        if (!impl_->inviteSecret.empty()) {
            if (packet.size() >= 36 && std::memcmp(packet.data(), "DSE1", 4) == 0) {
                const uint64_t session = read64(packet.data() + 4);
                Impl::Peer* owner = nullptr;
                for (auto& [id, peer] : impl_->peers) {
                    if (!peer.logical && !peer.closed && peer.session == session &&
                        peer.secure && peer.secure->established()) {
                        owner = &peer;
                        authenticatedPeer = id;
                        break;
                    }
                }
                if (!owner) continue;
                decrypted = owner->secure->open(packet);
                if (!decrypted) continue;
                packet = *decrypted;
                encrypted = true;
                if (!same(from, owner->address) &&
                    packet.size() >= 4 && std::memcmp(packet.data(), "DUR1", 4) == 0) continue;
            } else if (packet.size() < 4 ||
                       (std::memcmp(packet.data(), "DUC1", 4) != 0 &&
                        std::memcmp(packet.data(), "DSH1", 4) != 0)) continue;
        }
        if (peer_group_header(packet)) {
            if (impl_->server && impl_->realtime.size() < 512)
                impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)},
                                           {packet.begin(), packet.end()}, authenticatedPeer});
            continue;
        }
        if (is_peer_tunnel(packet)) {
            if (impl_->server) {
                if (impl_->realtime.size() < 512)
                    impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)},
                                               {packet.begin(), packet.end()}, authenticatedPeer});
            } else {
#ifdef DUSKLIGHT_TRANSPORT_TESTING
                if (impl_->rawTestTunnels) {
                    if (impl_->realtime.size() < 512)
                        impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)},
                                                   {packet.begin(), packet.end()}, authenticatedPeer});
                    continue;
                }
#endif
                const auto relay = impl_->peers.find(impl_->relayId);
                if (relay != impl_->peers.end() && !relay->second.closed && same(from, relay->second.address) &&
                    tunnel_read(packet.subspan(12, 8)) == impl_->localLogical) {
                    const Id id = impl_->logical_peer(tunnel_read(packet.subspan(4, 8)));
                    if (id != invalid) impl_->mesh_input(id, packet.subspan(20));
                }
            }
            continue;
        }
        if (packet.size() >= 4 && std::memcmp(packet.data(), "DUC1", 4) == 0) {
            impl_->receive_control(from, packet, now, encrypted); continue;
        }
        if (!encrypted && packet.size() >= 4 && std::memcmp(packet.data(), "DSH1", 4) == 0) {
            impl_->receive_secure_handshake(from, packet, now); continue;
        }
        // Admission for existing visual registration/rebinding remains in the
        // game/relay token validator. This bounded queue allocates no peer state.
        if (datagram_kind(packet) == DatagramKind::Realtime) {
            if (impl_->realtime.size() < 512)
                impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)},
                                           {packet.begin(), packet.end()}, authenticatedPeer});
            continue;
        }
        Id id = impl_->find(from);
        if (id == invalid || !impl_->peers.at(id).ready) continue;
        bool accepted = dispatch_datagram(id, packet, impl_->reliable, [&](auto, auto data) {
            if (impl_->realtime.size() >= 512) return false;
            impl_->realtime.push_back({{from.sin_addr.s_addr, ntohs(from.sin_port)},
                                       {data.begin(), data.end()}, id});
            return true;
        });
        if (accepted) impl_->peers.at(id).lastReceive = now;
    }
    impl_->pump_mesh(now);
    impl_->reliable.update(now);
    ReliableUdp::Message message;
    while (impl_->reliable.pop(message)) {
        auto it = impl_->peers.find(static_cast<Id>(message.peer));
        if (it == impl_->peers.end() || it->second.closed) continue;
        if (it->second.rx.size() + message.bytes.size() > ReliableUdp::maxBufferedBytes)
            impl_->close_peer(it->first, true);
        else it->second.rx.append(reinterpret_cast<const char*>(message.bytes.data()), message.bytes.size());
    }
    for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
        Id id = it->first; auto& peer = it->second;
        if (peer.logical) {
            if (!peer.closed && peer.meshReliable && impl_->reliable.failed(id)) impl_->close_peer(id, false);
            ++it; continue;
        }
        if (!peer.closed && ((peer.ready && (impl_->reliable.failed(id) || uint32_t(now - peer.lastReceive) >= 15000 ||
            (peer.secure && !peer.secure->established() && uint32_t(now - peer.started) >= 5000))) ||
            (!peer.ready && uint32_t(now - peer.started) >= 10000))) impl_->close_peer(id, true);
        if (peer.closed && impl_->server && !peer.accepted) { it = impl_->peers.erase(it); continue; }
        if (!peer.closed && uint32_t(now - peer.lastControl) >= (peer.ready ? 1000U : 500U))
            impl_->control(id, peer.ready ? 5 : (impl_->server ? 2 : (peer.session ? 3 : 1)));
        if (!peer.closed && peer.secure && peer.ready && !peer.secure->established() &&
            !peer.secure->retry_packet().empty() && uint32_t(now - peer.lastSecure) >= 500) {
            impl_->send(id, peer.secure->retry_packet());
            peer.lastSecure = now;
        }
        ++it;
    }
    impl_->budget.update(now);
    if (received) {
        ++impl_->activityGeneration;
        impl_->activity.notify_all();
    }
}
void UdpConnection::wait_for_activity(uint32_t timeoutMs) {
    std::unique_lock lock(impl_->mutex);
    const uint64_t generation = impl_->activityGeneration;
    impl_->activity.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        if (impl_->socket == badSocket || impl_->activityGeneration != generation ||
            !impl_->realtime.empty() || !impl_->accepted.empty()) return true;
        return std::any_of(impl_->peers.begin(), impl_->peers.end(),
            [](const auto& entry) { return !entry.second.rx.empty(); });
    });
}
bool UdpConnection::set_mesh_secret(std::string_view roomSecret) {
    std::lock_guard lock(impl_->mutex);
    if (roomSecret.size() < 16 || impl_->localLogical ||
        std::any_of(impl_->peers.begin(), impl_->peers.end(),
            [](const auto& entry) { return entry.second.logical != 0; })) return false;
    impl_->meshSecret = roomSecret;
    return true;
}
bool UdpConnection::mesh_open(std::string_view localId, Id relay, std::string_view host, uint16_t port) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->server || !peer_number(localId) ||
        (relay != invalid && !impl_->peers.contains(relay))) return false;
    impl_->localLogical = peer_number(localId); impl_->relayId = relay;
    impl_->stunHost = host; impl_->stunPort = port;
    return true;
}
UdpConnection::Id UdpConnection::mesh_admit(std::string_view peerId, bool reliable) {
    std::lock_guard lock(impl_->mutex);
    const auto number = peer_number(peerId);
    if (!impl_->localLogical || !number || number == impl_->localLogical) return invalid;
    const Id existing = impl_->logical_peer(number);
    if (existing != invalid) return existing;
    // One relay + seven player connections, independent of native capacity.
    if (impl_->peers.size() >= 8) return invalid;
    Impl::Peer peer;
    peer.logical = number;
    peer.meshReliable = reliable;
    // IDs are regenerated on rejoin, so old sessions cannot enter a new link.
    uint64_t low = std::min(number, impl_->localLogical), high = std::max(number, impl_->localLogical);
    peer.session = low ^ (high + 0x9e3779b97f4a7c15ULL + (low << 6) + (low >> 2));
    if (!peer.session) peer.session = 1;
    if (!impl_->meshSecret.empty()) {
        peer.secure.emplace(impl_->meshSecret, impl_->localLogical < number);
        if (!peer.secure->valid()) return invalid;
        peer.secure->set_session(peer.session);
    }
    peer.ice = std::make_unique<IceAgent>(impl_->stunHost, impl_->stunPort);
    peer.retryAt = clock_ms() + peer.retryDelay;
    peer.lastIceChange = clock_ms();
    const Id id = impl_->nextId++;
    impl_->peers.emplace(id, std::move(peer));
    if (!impl_->ready(id)) { impl_->peers.erase(id); return invalid; }
    return id;
}
void UdpConnection::mesh_remove(std::string_view peerId) {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id != invalid) { impl_->close_peer(id, false); impl_->peers.erase(id); }
    std::erase_if(impl_->meshRealtime, [&](const auto& packet) { return packet.peer == peerId; });
}
bool UdpConnection::mesh_signal(std::string_view peerId, const IceAgent::Signal& value) {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id == invalid) return false;
    auto& peer = impl_->peers.at(id);
    const auto kind = static_cast<size_t>(value.kind);
    if (kind >= peer.iceReceived.size()) return false;
    const auto reject = [&]() { ++peer.iceRejected[kind]; return false; };
    if (value.generation != peer.iceGeneration) {
        if (peer.logical >= impl_->localLogical || value.kind != IceAgent::Signal::Description ||
            value.generation != peer.iceGeneration + 1 ||
            uint32_t(clock_ms() - peer.lastIceChange) < 1000) return reject();
        peer.ice = std::make_unique<IceAgent>(impl_->stunHost, impl_->stunPort);
        peer.iceGeneration = value.generation; peer.hasEcho = false;
        peer.lastIceChange = clock_ms();
    }
    if (!peer.ice->signal(value)) return reject();
    ++peer.iceReceived[kind];
    return true;
}
bool UdpConnection::mesh_pop_signal(std::string& peerId, IceAgent::Signal& value) {
    std::lock_guard lock(impl_->mutex);
    for (auto& [id, peer] : impl_->peers) {
        if (peer.logical && !peer.closed && peer.ice->pop_signal(value)) {
            const auto kind = static_cast<size_t>(value.kind);
            if (kind < peer.iceSent.size()) ++peer.iceSent[kind];
            value.generation = peer.iceGeneration;
            peerId = "client_" + std::to_string(peer.logical); return true;
        }
    }
    return false;
}
bool UdpConnection::mesh_send(std::string_view peerId, std::span<const uint8_t> bytes) {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id == invalid) return false;
    const bool accepted = impl_->budget.send(id, bytes);
    impl_->budget.update(clock_ms()); return accepted;
}
int UdpConnection::mesh_receive(std::string& peerId, std::span<uint8_t> bytes) {
    poll();
    std::lock_guard lock(impl_->mutex);
    if (impl_->meshRealtime.empty()) return -1;
    auto packet = std::move(impl_->meshRealtime.front()); impl_->meshRealtime.pop_front();
    if (packet.bytes.size() > bytes.size()) return -1;
    peerId = std::move(packet.peer);
    std::copy(packet.bytes.begin(), packet.bytes.end(), bytes.begin());
    return static_cast<int>(packet.bytes.size());
}
bool UdpConnection::mesh_send_many(std::span<const std::string> names, std::span<const uint8_t> bytes,
    uint32_t* queuedDatagrams, uint64_t* queuedBytes) {
    std::lock_guard lock(impl_->mutex);
    if (names.size() > 7 || bytes.size() < 58 || bytes.size() > 1158 ||
        std::memcmp(bytes.data(), "DMPU", 4) != 0) return false;
    std::vector<uint64_t> fallback;
    bool ok = true;
    const auto enqueue = [&](Id id, std::span<const uint8_t> packet) {
        if (!impl_->budget.send(id, packet)) return false;
        if (queuedDatagrams) ++*queuedDatagrams;
        if (queuedBytes) *queuedBytes += packet.size();
        return true;
    };
    const auto now = clock_ms();
    for (const auto& name : names) {
        const Id id = impl_->logical_peer(peer_number(name));
        if (id == invalid) { ok = false; continue; }
        const auto& peer = impl_->peers.at(id);
        if (impl_->direct(peer, now)) ok = enqueue(id, bytes) && ok;
        else if (std::find(fallback.begin(), fallback.end(), peer.logical) == fallback.end()) fallback.push_back(peer.logical);
    }
    // Cloud rooms have no relay peer. Visuals are lossy: omit them until a
    // direct ICE path is usable rather than uploading them to a room service.
    if (!fallback.empty() && impl_->relayId != invalid)
        ok = enqueue(impl_->relayId, peer_group(impl_->localLogical, fallback, bytes)) && ok;
    impl_->budget.update(now); return ok;
}
bool UdpConnection::mesh_direct(std::string_view peerId) const {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    return id != invalid && impl_->direct(impl_->peers.at(id), clock_ms());
}
std::optional<uint32_t> UdpConnection::mesh_direct_rtt_ms(std::string_view peerId) const {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id == invalid) return std::nullopt;
    const auto& peer = impl_->peers.at(id);
    if (!impl_->direct(peer, clock_ms())) return std::nullopt;
    return peer.probeRtt;
}
std::string UdpConnection::mesh_diagnostics(std::string_view peerId) const {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id == invalid) return "peer=missing";
    const auto& peer = impl_->peers.at(id);
    const auto counts = [](const std::array<uint32_t, 3>& values) {
        return std::to_string(values[0]) + "/" + std::to_string(values[1]) + "/" +
            std::to_string(values[2]);
    };
    const uint32_t now = clock_ms();
    const auto age = [now](uint32_t timestamp) {
        return timestamp ? std::to_string(uint32_t(now - timestamp)) : std::string("never");
    };
    return "state=" + std::string(peer.ice ? peer.ice->state_text() : "invalid") +
        " gen=" + std::to_string(peer.iceGeneration) +
        " tx_desc_cand_done=" + counts(peer.iceSent) +
        " rx_desc_cand_done=" + counts(peer.iceReceived) +
        " rejected_desc_cand_done=" + counts(peer.iceRejected) +
        " echo=" + (peer.hasEcho ? "yes" : "no") +
        " echo_age_ms=" + age(peer.lastEcho) +
        " ice_rx_age_ms=" + age(peer.lastIcePacket) +
        " payload_rx_age_ms=" + age(peer.lastPayloadPacket) +
        " probe_rtt_ms=" + std::to_string(peer.probeRtt) +
        " probe_tx=" + std::to_string(peer.probeSends) +
        " probe_tx_fail=" + std::to_string(peer.probeSendFailures) +
        " probe_rx=" + std::to_string(peer.probeRequests) + "/" + std::to_string(peer.probeReplies) +
        " payload_rx=" + std::to_string(peer.payloadPackets);
}
bool UdpConnection::mesh_retry(std::string_view peerId) {
    std::lock_guard lock(impl_->mutex);
    const Id id = impl_->logical_peer(peer_number(peerId));
    if (id == invalid || impl_->localLogical >= impl_->peers.at(id).logical) return false;
    auto& peer = impl_->peers.at(id);
    peer.ice = std::make_unique<IceAgent>(impl_->stunHost, impl_->stunPort);
    ++peer.iceGeneration; peer.hasEcho = false;
    peer.lastIceChange = clock_ms();
    peer.retryAt = clock_ms() + peer.retryDelay;
    return true;
}
#ifdef DUSKLIGHT_TRANSPORT_TESTING
void UdpConnection::test_raw_tunnels(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    impl_->rawTestTunnels = enabled;
}
#endif
}
