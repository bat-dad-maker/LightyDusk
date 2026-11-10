#include "dusklight_online/net/reliable_udp.hpp"
#include "ikcp.h"
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>

namespace dusklight_online::net {
namespace {
constexpr size_t headerBytes = 14;
constexpr size_t segmentBytes = ReliableUdp::maxDatagramBytes - headerBytes - 24;
void put(std::vector<uint8_t>& bytes, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
uint64_t get(std::span<const uint8_t> bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(bytes[i]) << (8 * i);
    return value;
}
}
struct ReliableUdp::Impl {
    struct Group {
        Impl* owner;
        LogicalPeerId peer;
        uint64_t session;
        uint16_t index;
        ikcpcb* kcp = nullptr;
        int32_t baselineRtt = 0;
        std::vector<uint8_t> rx;
        ~Group() { if (kcp) ikcp_release(kcp); }
    };
    struct Peer {
        uint64_t session;
        bool failed = false;
        std::vector<std::unique_ptr<Group>> groups;
        bool waiting = false;
        uint32_t progressTime = 0;
        uint64_t acknowledged = 0;
    };
    DatagramTransport& carrier;
    size_t maxPeers;
    uint16_t groupCount;
    std::map<LogicalPeerId, Peer> peers;
    std::deque<Message> messages;
    size_t queuedBytes = 0;
    Impl(DatagramTransport& carrier, size_t peers, uint16_t groups)
        : carrier(carrier), maxPeers(peers), groupCount(groups) {}
    static int output(const char* bytes, int count, ikcpcb*, void* user) {
        auto& group = *static_cast<Group*>(user);
        std::vector<uint8_t> wire{'D','U','R','1'};
        put(wire, group.session, 8);
        put(wire, group.index, 2);
        wire.insert(wire.end(), bytes, bytes + count);
        // Unacknowledged data stays in KCP; output loss gets normal retries.
        group.owner->carrier.send(group.peer, wire);
        return 0;
    }
    void read(Group& group, Peer& peer) {
        std::array<char, 4096> bytes{};
        for (;;) {
            int size = ikcp_peeksize(group.kcp);
            if (size < 0) break;
            if (size > static_cast<int>(bytes.size())) { peer.failed = true; break; }
            int count = ikcp_recv(group.kcp, bytes.data(), static_cast<int>(bytes.size()));
            if (count <= 0) break;
            if (group.rx.size() + size_t(count) > maxMessageBytes + 4 + bytes.size()) {
                peer.failed = true; break;
            }
            group.rx.insert(group.rx.end(), bytes.data(), bytes.data() + count);
            size_t offset = 0;
            while (group.rx.size() - offset >= 4) {
                auto pending = std::span<const uint8_t>(group.rx).subspan(offset);
                size_t length = static_cast<size_t>(get(pending.first(4)));
                if (!length || length > maxMessageBytes) { peer.failed = true; return; }
                if (pending.size() < length + 4) break;
                if (messages.size() >= 1024 || length > maxBufferedBytes - queuedBytes) {
                    peer.failed = true; return;
                }
                messages.push_back({group.peer, group.index, {pending.begin() + 4, pending.begin() + 4 + length}});
                queuedBytes += length;
                offset += length + 4;
            }
            if (offset) group.rx.erase(group.rx.begin(), group.rx.begin() + offset);
        }
    }
};
ReliableUdp::ReliableUdp(DatagramTransport& carrier, size_t peers, uint16_t groups)
    : impl_(std::make_unique<Impl>(carrier, peers, groups)) {
    if (!peers || peers > 4096 || !groups || groups > 8) throw std::invalid_argument("invalid reliability limits");
}
ReliableUdp::~ReliableUdp() = default;
bool ReliableUdp::add_peer(LogicalPeerId id, uint64_t session) {
    if (!session || impl_->peers.contains(id) || impl_->peers.size() >= impl_->maxPeers) return false;
    Impl::Peer peer{session};
    for (uint16_t index = 0; index < impl_->groupCount; ++index) {
        auto group = std::make_unique<Impl::Group>();
        group->owner = impl_.get(); group->peer = id; group->session = session; group->index = index;
        group->kcp = ikcp_create(0, group.get());
        if (!group->kcp) return false;
        ikcp_setoutput(group->kcp, Impl::output);
        ikcp_setmtu(group->kcp, static_cast<int>(maxDatagramBytes - headerBytes));
        // Bound in-flight data to roughly 36 KiB. Shared pacing and receiver
        // flow control stay active. update() chooses recovery behaviour from
        // measured queue delay rather than treating every random loss as a
        // reason to serialize gameplay behind a one-segment window.
        ikcp_wndsize(group->kcp, 32, 128);
        ikcp_nodelay(group->kcp, 1, 10, 2, 0); // conservative startup until RTT is measured
        group->kcp->stream = 1;
        peer.groups.push_back(std::move(group));
    }
    impl_->peers.emplace(id, std::move(peer));
    return true;
}
void ReliableUdp::remove_peer(LogicalPeerId peer) {
    impl_->peers.erase(peer);
    for (auto it = impl_->messages.begin(); it != impl_->messages.end();) {
        if (it->peer == peer) { impl_->queuedBytes -= it->bytes.size(); it = impl_->messages.erase(it); }
        else ++it;
    }
}
bool ReliableUdp::send(LogicalPeerId id, std::span<const uint8_t> bytes, uint16_t index) {
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end() || it->second.failed || index >= impl_->groupCount ||
        bytes.empty() || bytes.size() > maxMessageBytes) return false;
    auto& peer = it->second;
    size_t queued = 0;
    for (const auto& group : peer.groups) queued += size_t(ikcp_waitsnd(group->kcp)) * segmentBytes;
    if (queued > maxBufferedBytes || bytes.size() + 4 > maxBufferedBytes - queued) return false;
    std::vector<uint8_t> frame;
    frame.reserve(bytes.size() + 4);
    put(frame, bytes.size(), 4);
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    auto* kcp = peer.groups[index]->kcp;
    // Bound individual KCP calls below its fragmentation limit, even in stream
    // mode. Only a complete application frame is ever exposed to the game.
    for (size_t offset = 0; offset < frame.size();) {
        size_t count = std::min<size_t>(32 * 1024, frame.size() - offset);
        if (ikcp_send(kcp, reinterpret_cast<const char*>(frame.data() + offset), static_cast<int>(count)) < 0) {
            peer.failed = true; return false;
        }
        offset += count;
    }
    return true;
}
bool ReliableUdp::input(LogicalPeerId id, std::span<const uint8_t> bytes) {
    auto it = impl_->peers.find(id);
    if (it == impl_->peers.end() || it->second.failed || bytes.size() < headerBytes + 24 ||
        bytes.size() > maxReceiveDatagramBytes || bytes[0] != 'D' || bytes[1] != 'U' ||
        bytes[2] != 'R' || bytes[3] != '1' || get(bytes.subspan(4, 8)) != it->second.session) return false;
    auto group = get(bytes.subspan(12, 2));
    if (group >= impl_->groupCount) return false;
    auto payload = bytes.subspan(headerBytes);
    // Validate the entire compound packet before KCP can partially consume it.
    auto remaining = payload;
    while (!remaining.empty()) {
        if (remaining.size() < 24 || get(remaining.first(4)) != 0 || remaining[5] != 0 ||
            remaining[4] < 81 || remaining[4] > 84) return false;
        auto length = get(remaining.subspan(20, 4));
        if (length > remaining.size() - 24 || (remaining[4] != 81 && length != 0)) return false;
        remaining = remaining.subspan(24 + static_cast<size_t>(length));
    }
    return ikcp_input(it->second.groups[group]->kcp,
        reinterpret_cast<const char*>(payload.data()), static_cast<long>(payload.size())) == 0;
}
void ReliableUdp::update(uint32_t milliseconds) {
    for (auto& [id, peer] : impl_->peers) {
        if (peer.failed) continue;
        uint64_t acknowledged = 0;
        bool waiting = false;
        for (const auto& group : peer.groups) {
            acknowledged += group->kcp->snd_una;
            waiting |= ikcp_waitsnd(group->kcp) != 0;
        }
        if (!peer.waiting || !waiting || acknowledged != peer.acknowledged)
            peer.progressTime = milliseconds;
        peer.waiting = waiting;
        peer.acknowledged = acknowledged;
        // Bounded no-progress failure rather than unlimited exponential retries.
        // Connection teardown/recovery is the owner's responsibility.
        if (waiting && uint32_t(milliseconds - peer.progressTime) >= 30000) {
            peer.failed = true;
            continue;
        }
        for (auto& group : peer.groups) {
            // A stable RTT permits the bounded send window even when packets
            // are lost. Rising RTT is evidence of queueing: restore KCP's
            // congestion window and exponential backoff until it subsides.
            // Baseline lifetime is this admitted peer/session, never a socket.
            auto* kcp = group->kcp;
            if (kcp->rx_srtt > 0 && (!group->baselineRtt || kcp->rx_srtt < group->baselineRtt))
                group->baselineRtt = kcp->rx_srtt;
            const bool queuedLink = group->baselineRtt &&
                kcp->rx_srtt > group->baselineRtt + std::max(50, group->baselineRtt / 4);
            ikcp_nodelay(kcp, queuedLink ? 0 : 1, 10, 2, group->baselineRtt && !queuedLink ? 1 : 0);
            // Four packets bound startup while avoiding the one-packet stall.
            // The shared scheduler also paces every retry and realtime packet.
            if (!queuedLink && kcp->cwnd < 4) {
                kcp->cwnd = 4;
                kcp->incr = 4 * kcp->mss;
            }
            ikcp_update(group->kcp, milliseconds);
            if (group->kcp->state == std::numeric_limits<IUINT32>::max()) { peer.failed = true; break; }
            impl_->read(*group, peer);
            if (peer.failed) break;
        }
    }
}
bool ReliableUdp::pop(Message& message) {
    if (impl_->messages.empty()) return false;
    message = std::move(impl_->messages.front()); impl_->messages.pop_front();
    impl_->queuedBytes -= message.bytes.size();
    return true;
}
bool ReliableUdp::failed(LogicalPeerId peer) const {
    auto it = impl_->peers.find(peer);
    return it == impl_->peers.end() || it->second.failed;
}
bool ReliableUdp::drained(LogicalPeerId peer) const {
    auto it = impl_->peers.find(peer);
    return it == impl_->peers.end() || std::all_of(it->second.groups.begin(), it->second.groups.end(),
        [](const auto& group) { return ikcp_waitsnd(group->kcp) == 0; });
}
}
