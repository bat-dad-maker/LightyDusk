#pragma once
#include "dusklight_online/net/reliable_udp.hpp"
#include <functional>

namespace dusklight_online::net {
enum class DatagramKind { Invalid, Realtime, Reliable };
DatagramKind datagram_kind(std::span<const uint8_t> bytes);
bool dispatch_datagram(LogicalPeerId peer, std::span<const uint8_t> bytes,
    ReliableUdp& reliable,
    const std::function<bool(LogicalPeerId, std::span<const uint8_t>)>& realtime);

// One owner-thread budget for BOTH producers, above any native/ICE carrier.
// The connection layer must admit/remove peers here as well as in reliability.
class DatagramScheduler : public DatagramTransport {
public:
    struct Limits {
        size_t perPeerBytesPerSecond = 512 * 1024;
        size_t totalBytesPerSecond = 4 * 1024 * 1024;
        size_t queuedBytesPerClass = 256 * 1024;
        uint32_t maxQueueAgeMs = 100;
    };
    explicit DatagramScheduler(DatagramTransport& carrier);
    DatagramScheduler(DatagramTransport& carrier, Limits limits);
    ~DatagramScheduler();
    bool add_peer(LogicalPeerId peer);
    void remove_peer(LogicalPeerId peer);
    bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) override;
    void update(uint32_t milliseconds);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
