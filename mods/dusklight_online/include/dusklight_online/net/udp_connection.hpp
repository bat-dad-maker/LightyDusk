#pragma once
#include "dusklight_online/net/reliable_udp.hpp"
#include "dusklight_online/net/ice_agent.hpp"
#include <string_view>
#include <string>
#include <optional>

namespace dusklight_online::net {
// Native carrier/session adapter. Game transport depends on this connection
// facade; ReliableUdp below it remains unaware of sockets and handshakes.
class UdpConnection {
public:
    using Id = uintptr_t;
    static constexpr Id invalid = static_cast<Id>(-1);
    struct Address { uint32_t ipv4 = 0; uint16_t port = 0; };
    UdpConnection();
    ~UdpConnection();
    bool open(std::string_view host, uint16_t port, size_t capacity, bool server,
              bool stun = false, std::string_view inviteSecret = {});
    void close();
    Id connect(std::string_view host, uint16_t port);
    Id accept(Address& address);
    void disconnect(Id peer);
    bool connected(Id peer) const;
    bool alive(Id peer) const;
    bool drained(Id peer) const;
    bool send(Id peer, const char* data, size_t size);
    // -1 idle, 0 closed, positive bytes. Framing stays in the application.
    int receive(Id peer, char* data, size_t size);
    bool send_realtime(Address address, std::span<const uint8_t> bytes);
    // Called only AFTER the existing relay token/sender validation succeeds.
    // Preserves authenticated visual endpoint rebinding without changing the
    // reliable session's address or allocating a second bandwidth allowance.
    bool bind_realtime(Id peer, Address address);
    // Reports the secure native peer that authenticated this datagram when
    // available. Callers must match it to the claimed visual sender.
    int receive_realtime(Address& address, std::span<uint8_t> bytes,
                         Id* authenticatedPeer = nullptr);
    void poll();
    // Relay idle wait: wake for received data without imposing a polling delay.
    // Does not service game callbacks or bypass the shared datagram budget.
    void wait_for_activity(uint32_t timeoutMs);
    // Logical ICE links. An invalid relay ID forbids gameplay fallback.
    bool set_mesh_secret(std::string_view roomSecret);
    bool mesh_open(std::string_view localId, Id relay, std::string_view stunHost, uint16_t stunPort);
    Id mesh_admit(std::string_view peerId, bool reliable = false);
    void mesh_remove(std::string_view peerId);
    bool mesh_signal(std::string_view peerId, const IceAgent::Signal& signal);
    bool mesh_pop_signal(std::string& peerId, IceAgent::Signal& signal);
    bool mesh_send(std::string_view peerId, std::span<const uint8_t> bytes);
    bool mesh_send_many(std::span<const std::string> peers, std::span<const uint8_t> bytes,
        uint32_t* queuedDatagrams = nullptr, uint64_t* queuedBytes = nullptr);
    int mesh_receive(std::string& peerId, std::span<uint8_t> bytes);
    bool mesh_direct(std::string_view peerId) const;
    std::optional<uint32_t> mesh_direct_rtt_ms(std::string_view peerId) const;
    // Redacted ICE state/counters for diagnostics; never includes SDP or IPs.
    std::string mesh_diagnostics(std::string_view peerId) const;
    // Explicit ICE retry retains the logical session and reliable queues.
    bool mesh_retry(std::string_view peerId);
#ifdef DUSKLIGHT_TRANSPORT_TESTING
    void test_raw_tunnels(bool enabled);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
