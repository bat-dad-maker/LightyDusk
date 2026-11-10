#include "dusklight_online/net/datagram_scheduler.hpp"
#include "dusklight_online/net/peer_tunnel.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <stdexcept>

namespace dusklight_online::net {
DatagramKind datagram_kind(std::span<const uint8_t> bytes) {
    if (const auto header = peer_group_header(bytes)) {
        const auto payload = bytes.subspan(header);
        return std::memcmp(payload.data(), "DMPU", 4) == 0 ? DatagramKind::Realtime : DatagramKind::Invalid;
    }
    if (is_peer_tunnel(bytes)) {
        // Exactly one envelope. Nested wrappers cannot consume recursive stack.
        const auto inner = bytes.subspan(20);
        if (is_peer_tunnel(inner)) return DatagramKind::Invalid;
        return datagram_kind(inner);
    }
    if (bytes.size() >= 58 && bytes.size() <= 2048 && std::memcmp(bytes.data(), "DMPU", 4) == 0)
        return DatagramKind::Realtime;
    if (bytes.size() >= 38 && bytes.size() <= ReliableUdp::maxDatagramBytes &&
        std::memcmp(bytes.data(), "DUR1", 4) == 0) return DatagramKind::Reliable;
    return DatagramKind::Invalid;
}
bool dispatch_datagram(LogicalPeerId peer, std::span<const uint8_t> bytes, ReliableUdp& reliable,
    const std::function<bool(LogicalPeerId, std::span<const uint8_t>)>& realtime) {
    switch (datagram_kind(bytes)) {
    case DatagramKind::Reliable: return reliable.input(peer, bytes);
    case DatagramKind::Realtime: return realtime(peer, bytes); // existing codec validates full header/payload
    default: return false;
    }
}
struct DatagramScheduler::Impl {
    // Charge conservative encapsulation overhead as well as payload bytes.
    static constexpr size_t overhead = 64;
    static constexpr double burst = 2 * (2048 + overhead);
    struct Packet { uint32_t queuedAt; std::vector<uint8_t> bytes; };
    struct Peer {
        std::array<std::deque<Packet>, 2> queues;
        std::array<size_t, 2> bytes{};
        unsigned nextClass = 0;
        double tokens = burst;
    };
    DatagramTransport& carrier;
    Limits limits;
    std::map<LogicalPeerId, Peer> peers;
    LogicalPeerId nextPeer = 0;
    uint32_t now = 0;
    bool started = false;
    double tokens = burst;
    Impl(DatagramTransport& carrier, Limits limits) : carrier(carrier), limits(limits) {}
};
DatagramScheduler::DatagramScheduler(DatagramTransport& carrier) : DatagramScheduler(carrier, Limits{}) {}
DatagramScheduler::DatagramScheduler(DatagramTransport& carrier, Limits limits)
    : impl_(std::make_unique<Impl>(carrier, limits)) {
    if (!limits.perPeerBytesPerSecond || !limits.totalBytesPerSecond ||
        limits.queuedBytesPerClass < 2048 || !limits.maxQueueAgeMs)
        throw std::invalid_argument("invalid datagram budget");
}
DatagramScheduler::~DatagramScheduler() = default;
bool DatagramScheduler::add_peer(LogicalPeerId peer) {
    if (impl_->peers.size() >= 4096) return false;
    return impl_->peers.try_emplace(peer).second;
}
void DatagramScheduler::remove_peer(LogicalPeerId peer) { impl_->peers.erase(peer); }
bool DatagramScheduler::send(LogicalPeerId peer, std::span<const uint8_t> bytes) {
    auto it = impl_->peers.find(peer);
    auto kind = datagram_kind(bytes);
    if (it == impl_->peers.end() || kind == DatagramKind::Invalid) return false;
    auto& state = it->second;
    unsigned index = kind == DatagramKind::Realtime ? 0 : 1;
    auto& queue = state.queues[index];
    auto& queuedBytes = state.bytes[index];
    if (bytes.size() > impl_->limits.queuedBytesPerClass - queuedBytes) {
        if (index == 1) return false; // KCP retains reliable data and retries
        while (!queue.empty() && bytes.size() > impl_->limits.queuedBytesPerClass - queuedBytes) {
            queuedBytes -= queue.front().bytes.size(); queue.pop_front();
        }
    }
    queue.push_back({impl_->now, {bytes.begin(), bytes.end()}});
    queuedBytes += bytes.size();
    return true;
}
void DatagramScheduler::update(uint32_t milliseconds) {
    uint32_t elapsed = impl_->started ? milliseconds - impl_->now : 0;
    impl_->started = true;
    impl_->now = milliseconds;
    impl_->tokens = std::min(Impl::burst, impl_->tokens + elapsed * (impl_->limits.totalBytesPerSecond / 1000.0));
    for (auto& [id, peer] : impl_->peers) {
        peer.tokens = std::min(Impl::burst, peer.tokens + elapsed * (impl_->limits.perPeerBytesPerSecond / 1000.0));
        for (unsigned index = 0; index < 2; ++index) {
            auto& queue = peer.queues[index];
            while (!queue.empty() && uint32_t(milliseconds - queue.front().queuedAt) > impl_->limits.maxQueueAgeMs) {
                peer.bytes[index] -= queue.front().bytes.size(); queue.pop_front();
            }
        }
    }
    // Round-robin peers and traffic classes. No producer can bypass this budget.
    for (size_t pass = 0; pass < 4096 && !impl_->peers.empty(); ++pass) {
        bool sent = false;
        auto it = impl_->peers.lower_bound(impl_->nextPeer);
        if (it == impl_->peers.end()) it = impl_->peers.begin();
        for (size_t visited = 0; visited < impl_->peers.size(); ++visited) {
            auto& peer = it->second;
            for (unsigned attempt = 0; attempt < 2; ++attempt) {
                unsigned index = (peer.nextClass + attempt) % 2;
                auto& queue = peer.queues[index];
                if (queue.empty()) continue;
                size_t cost = queue.front().bytes.size() + Impl::overhead;
                if (cost > peer.tokens || cost > impl_->tokens) continue;
                peer.tokens -= cost; impl_->tokens -= cost;
                impl_->carrier.send(it->first, queue.front().bytes);
                peer.bytes[index] -= queue.front().bytes.size(); queue.pop_front();
                peer.nextClass = 1 - index;
                sent = true;
                break;
            }
            ++it;
            if (it == impl_->peers.end()) it = impl_->peers.begin();
            if (sent) { impl_->nextPeer = it->first; break; }
        }
        if (!sent) break;
    }
}
}
