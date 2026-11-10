#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include "dusklight_online/net/wire_limits.hpp"

namespace dusklight_online::net {
using LogicalPeerId = uint64_t;
// Carrier owns sockets, addressing, traversal and admission. Consume/copy bytes
// before returning and never reenter ReliableUdp. False means local packet loss.
class DatagramTransport {
public:
    virtual ~DatagramTransport() = default;
    virtual bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) = 0;
};
// Single-owner protocol engine: no sockets, DNS, threads, clock reads or
// connection establishment. Owner admits peers, feeds input and supplies time.
class ReliableUdp {
public:
    static constexpr size_t maxMessageBytes = 2 * 1024 * 1024;
    static constexpr size_t maxBufferedBytes = 8 * 1024 * 1024;
    static constexpr size_t maxDatagramBytes = kReliableDatagramBytes;
    // Accept packets from an older sender while its path is being updated.
    static constexpr size_t maxReceiveDatagramBytes = kIceDatagramBytes;
    struct Message { LogicalPeerId peer; uint16_t group; std::vector<uint8_t> bytes; };
    explicit ReliableUdp(DatagramTransport& carrier, size_t maxPeers = 8, uint16_t groups = 1);
    ~ReliableUdp();
    ReliableUdp(const ReliableUdp&) = delete;
    ReliableUdp& operator=(const ReliableUdp&) = delete;
    // Fresh shared generation supplied by connection layer; not authentication.
    // Unknown peers/sessions never allocate state.
    bool add_peer(LogicalPeerId peer, uint64_t session);
    void remove_peer(LogicalPeerId peer);
    bool send(LogicalPeerId peer, std::span<const uint8_t> message, uint16_t group = 0);
    bool input(LogicalPeerId peer, std::span<const uint8_t> datagram);
    void update(uint32_t milliseconds);
    bool pop(Message& message);
    bool failed(LogicalPeerId peer) const;
    bool drained(LogicalPeerId peer) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
