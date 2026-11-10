#include "dusklight_online/net/peer_delivery.hpp"
#include "dusklight_online/net/auth_code.hpp"
#include "dusklight_online/net/cloud_cipher.hpp"
#include "dusklight_online/net/reliable_json.hpp"
#include "dusklight_online/net/pose_ack_history.hpp"
#include "dusklight_online/net/transport.hpp"
#include "dusklight_online/net/udp_connection.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    constexpr socket_t kInvalidSocket = -1;
    #if defined(__APPLE__)
        constexpr int kSendFlags = 0;
    #else
        constexpr int kSendFlags = MSG_NOSIGNAL;
    #endif
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dusklight_online::net {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxDirectPeers = 7;
constexpr size_t kReliableRxBufferMaxBytes = 2 * 1024 * 1024;
constexpr size_t kReliableTxBufferMaxBytes = 256 * 1024;
constexpr size_t kMaxMaterializedEvents = 1024;
constexpr size_t kUdpTxPacerMaxQueuedDatagrams = 512;
constexpr size_t kUdpTxPacerBaseDestinationBytesPerSecond = 512 * 1024;
constexpr size_t kUdpTxPacerVisualFlowBytesPerSecond = 256 * 1024;
constexpr auto kUdpTxPacerFlowActiveWindow = std::chrono::seconds(2);
constexpr auto kUdpTxPacerMinDatagramGap = std::chrono::milliseconds(1);
#if defined(_WIN32)
std::mutex sNetworkStackMutex;
size_t sNetworkStackOwners = 0;
#endif

bool fill_ipv4(sockaddr_in& address, const std::string& host, uint16_t port) {
    address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1) {
        return true;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    address.sin_addr = reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

bool acquire_network_stack(bool& owned, std::string* error) {
    if (owned) return true;
#if defined(_WIN32)
    std::lock_guard lock(sNetworkStackMutex);
    if (sNetworkStackOwners == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error != nullptr) *error = "WSAStartup failed";
            return false;
        }
    }
    ++sNetworkStackOwners;
#else
    (void)error;
#endif
    owned = true;
    return true;
}

void release_network_stack(bool& owned) {
    if (!owned) return;
#if defined(_WIN32)
    std::lock_guard lock(sNetworkStackMutex);
    if (sNetworkStackOwners > 0 && --sNetworkStackOwners == 0) WSACleanup();
#endif
    owned = false;
}

json settings_json(const RoomSettings& settings) {
    return {
        {"dummy_model", settings.dummyModel},
        {"sync_flags", settings.syncFlags},
        {"sync_world", settings.syncWorld},
        {"remote_collision", settings.remoteCollision},
        {"pvp", effective_pvp(settings)},
    };
}

RoomSettings parse_settings(const json& value, RoomSettings fallback) {
    if (!value.is_object()) {
        return fallback;
    }
    fallback.dummyModel = value.value("dummy_model", fallback.dummyModel);
    fallback.syncFlags = value.value("sync_flags", fallback.syncFlags);
    fallback.syncWorld = value.value("sync_world", fallback.syncWorld);
    fallback.remoteCollision = value.value("remote_collision", fallback.remoteCollision);
    fallback.pvp = value.value("pvp", fallback.pvp) && fallback.remoteCollision;
    return fallback;
}

constexpr const char* kSemanticVisualCapability = "semantic_visual_v1";
constexpr const char* kSnapshotDeltaCapability = "semantic_snapshot_delta_v1";

bool has_semantic_visual_capability(const json& message) {
    const auto capabilities = message.find("capabilities");
    if (capabilities == message.end() || !capabilities->is_object()) return false;
    const auto capability = capabilities->find(kSemanticVisualCapability);
    return capability != capabilities->end() && capability->is_boolean() &&
           capability->get<bool>();
}

bool has_snapshot_delta_capability(const json& message) {
    const auto capabilities = message.find("capabilities");
    if (capabilities == message.end() || !capabilities->is_object()) return false;
    const auto capability = capabilities->find(kSnapshotDeltaCapability);
    return capability != capabilities->end() && capability->is_boolean() &&
           capability->get<bool>();
}

std::vector<std::string> take_snapshot_debug_keys(json& message, const char* name) {
    std::vector<std::string> result;
    const auto value = message.find(name);
    if (value != message.end() && value->is_array()) {
        result.reserve(value->size());
        for (const auto& key : *value) {
            if (key.is_string()) result.push_back(key.get<std::string>());
        }
    }
    message.erase(name);
    return result;
}

bool should_forward_direct(const std::string& type) {
    return type != "hello" && type != "ping" && type != "pong" && type != "error";
}

bool cloud_control_type(std::string_view type) {
    return type == "hello" || type == "ice_signal" || type == "room_settings" ||
           type == "settings_ready" || type == "kick";
}

bool valid_room_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    for (char c : name) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string cloud_room_url(std::string_view base, std::string_view room) {
    std::string result(base);
    if (result.starts_with("https://")) result.replace(0, 8, "wss://");
    while (result.ends_with('/')) result.pop_back();
    result += "/room/v2/" + cloud_room_route(room);
    return result;
}

}  // namespace

struct Transport::Impl {
    struct Peer {
        socket_t socket = kInvalidSocket;
        sockaddr_in udpAddress{};
        std::string id;
        std::string name = "Peer";
        std::string rx;
        std::string tx;
        bool welcomed = false;
        bool udpAddressKnown = false;
        bool wantPuppet = true;
        bool wantMidna = false;
        bool supportsSemanticVisuals = false;
        bool supportsSnapshotDeltas = false;
        bool kickPending = false;
        std::string authNonce;
        std::chrono::steady_clock::time_point acceptedAt = std::chrono::steady_clock::now();
    };

    struct PeerPresence {
        std::string stage;
        uint32_t ageTicks = 0;
    };

    struct PacedDatagram {
        sockaddr_in address{};
        udp::Datagram datagram;
        std::string queueKey;
    };

    Status status;
    bool preferredSaveRecovery = true;
    VisualSendStats lastVisualSend;
    socket_t socket = kInvalidSocket;
    UdpConnection connections;
    std::unique_ptr<RoomChannel> cloudChannel;
    std::optional<CloudCipher> cloudCipher;
    std::string cloudUrl;
    std::string cloudStunHost;
    uint16_t cloudStunPort = 0;
    std::chrono::steady_clock::time_point cloudJoinedAt{};
    std::optional<std::chrono::steady_clock::time_point> cloudDisconnectedAt;
    std::string cloudResumeToken, cloudRetireToken, cloudMeshKey;
    bool cloudRecovering = false, cloudRecoveryOpening = false;
    std::chrono::steady_clock::time_point cloudRecoveryStarted{}, cloudRetryAt{};
    static constexpr auto cloudRecoveryLimit = std::chrono::seconds(8);
    struct CloudLinkAttempt {
        std::chrono::steady_clock::time_point started;
        uint32_t nextLogSecond = 3;
        bool retried = false;
        // The joiner times itself out at 15 seconds. Give its peer_left time
        // to arrive before the established room asks the owner to remove it.
        uint32_t nextEvictionSecond = 20;
    };
    std::map<std::string, CloudLinkAttempt> cloudLinkStarted;
    std::map<std::string, std::chrono::steady_clock::time_point> cloudNewcomers;
    bool listening = false;
    bool udpOpen = false;
    sockaddr_in udpRemoteAddress{};
    std::string rx;
    std::string tx;
    std::map<std::string, Peer> directPeers;
    std::map<std::string, std::string> peerNames;
    std::set<std::string> cloudDepartedPeers;
    std::deque<std::string> cloudDepartedOrder;
    bool meshEnabled = false;
    std::map<std::string, bool> meshPuppets;
    std::map<std::string, bool> meshRoutes;
    std::map<std::string, socket_t> meshLinks;
    std::map<std::string, std::string> meshRx;
    struct PendingGameplay { json value; size_t bytes; };
    // Delivery identity survives carrier changes. KCP still handles packet
    // recovery on each path; these bounded journals only bridge a path switch.
    struct PeerDelivery {
        uint64_t nextSend = 1, nextReceive = 1, ackPending = 0;
        std::map<uint64_t, PendingGameplay> sent, received;
    };
    std::map<std::string, PeerDelivery> peerDelivery;
    std::deque<PendingGameplay> deferredSends;
    std::map<std::string,std::deque<PendingGameplay>> futureGameplay;
    std::set<std::string> barrierExpected, barrierSeen, futureBarrierSeen;
    uint64_t settingsGeneration = 0;
    std::string announcedStage;
    bool settingsRequested = false, settingsBarrier = false, barrierObserver = false, barrierReady = false;
    std::chrono::steady_clock::time_point barrierStarted{};
    size_t deliveryBytes = 0;
    bool deliveryFailure = false;
#if defined(DUSKLIGHT_TRANSPORT_TESTING)
    bool testRestarted = false;
#endif
    static constexpr size_t deliveryLimit = 4 * 1024 * 1024;
    bool retain_delivery(size_t bytes) {
        if (deliveryBytes + bytes > deliveryLimit) { deliveryFailure = true; return false; }
        deliveryBytes += bytes; return true;
    }
    std::map<std::string, PeerPresence> peerStages;
    std::map<std::string, std::string> peerPoseStages;
    std::deque<Event> events;
    uint32_t nextDirectPeerId = 1;
    uint32_t reconnectTicks = 0;
    uint32_t relayUdpRegisterTicks = 0;
    bool automaticReconnect = true;
    bool sessionEstablished = false;
    bool helloSent = false;
    bool relayCreateRoom = false;
    bool relayMayRecreateRoom = false;
    bool handshakeRejected = false;
    bool wantPuppet = true;
    bool wantMidna = false;
    bool supportsSnapshotDeltas = true;
    bool visualWireDiagnostics = false;
    std::string lobbyCode;
    std::string sessionId;
    std::string sessionKey;
    std::string authNonce;
    std::string authSalt;
    std::chrono::steady_clock::time_point challengeStartedAt{};
    bool udpRemoteAddressKnown = false;
    udp::Decoder udpDecoder;
    Transport::PoseDeltaExpandCallback poseDeltaExpand = nullptr;
    Transport::PoseDeltaPrepareCallback poseDeltaPrepare = nullptr;
    std::map<std::string, uint32_t> poseAckSequences;
    std::map<std::string, PoseAckHistory> poseAckHistories;
    std::map<std::string, uint32_t> poseAckResetFloors;
    std::mutex udpTxMutex;
    std::condition_variable udpTxCv;
    std::deque<PacedDatagram> udpTxQueue;
    std::thread udpTxThread;
    bool udpTxStop = false;
    bool udpTxRunning = false;
    bool networkStackOwned = false;
    bool eventQueueOverflow = false;
    uint64_t connectionEpoch = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> udpTxNextSendByDestination;
    std::map<std::string,
             std::map<std::string, std::chrono::steady_clock::time_point>>
        udpTxVisualFlowsByDestination;

    ~Impl() {
        close_all();
        release_network_stack(networkStackOwned);
    }

    bool push_event(Event event) {
        event.ingress = {
            connectionEpoch, status.mode, status.welcomed,
            status.semanticVisualsReady, status.snapshotDeltasReady,
            status.settings, status.clientId,
        };
        if (event.kind == EventKind::UdpMessage || event.kind == EventKind::UdpVoice) {
            // Preserve short receive bursts for timed pose playback. Replacing
            // every pending pose with the newest one discards valid samples
            // whenever two datagrams arrive between game updates.
            constexpr size_t maxPendingPosesPerPeer = 16;
            size_t count = 0;
            auto oldest = events.end();
            for (auto it = events.begin(); it != events.end(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->udpType == event.udpType) {
                    if (oldest == events.end()) oldest = it;
                    ++count;
                }
            }
            if (count >= maxPendingPosesPerPeer) events.erase(oldest);
        } else if (event.kind == EventKind::UdpAck) {
            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->detail == event.detail && it->udpType == event.udpType) {
                    if (event.udpSequence > it->udpSequence) *it = std::move(event);
                    return true;
                }
            }
        } else if (event.kind == EventKind::UdpRemoteObject) {
            for (auto it = events.rbegin(); it != events.rend(); ++it) {
                if (it->kind == event.kind && it->peerId == event.peerId &&
                    it->remoteObject.objectId == event.remoteObject.objectId) {
                    if (event.remoteObject.sequence > it->remoteObject.sequence) {
                        *it = std::move(event);
                    }
                    return true;
                }
            }
        }
        // Diagnostic logging must not consume the last slots needed for
        // gameplay, membership or disconnect events.
        if (event.kind == EventKind::Diagnostic &&
            events.size() >= kMaxMaterializedEvents - 64) return false;
        if (events.size() >= kMaxMaterializedEvents) {
            eventQueueOverflow = true;
            return false;
        }
        events.push_back(std::move(event));
        return true;
    }

    bool emit(EventKind kind, std::string peerId = {}, std::string detail = {},
              json message = {}) {
        Event event;
        event.kind = kind;
        event.peerId = std::move(peerId);
        event.detail = std::move(detail);
        event.message = std::move(message);
        return push_event(std::move(event));
    }

    static std::string udp_tx_address_key(const sockaddr_in& address) {
        return std::to_string(ntohl(address.sin_addr.s_addr)) + ":" +
               std::to_string(ntohs(address.sin_port));
    }

    static std::string udp_tx_queue_key(const sockaddr_in& address,
                                        std::string_view senderId,
                                        udp::PacketType type,
                                        std::string_view receiverId) {
        return udp_tx_address_key(address) + "|" +
               std::to_string(static_cast<unsigned>(type)) + "|" +
               std::string(senderId) + "|" + std::string(receiverId);
    }

    static bool is_paced_visual_type(udp::PacketType type) {
        return type == udp::PacketType::PoseMsgpack ||
               type == udp::PacketType::SemanticPoseMsgpack ||
               type == udp::PacketType::MidnaMsgpack;
    }

    size_t udp_tx_target_rate_locked(
        const std::string& destinationKey,
        std::chrono::steady_clock::time_point now) {
        auto destination = udpTxVisualFlowsByDestination.find(destinationKey);
        if (destination == udpTxVisualFlowsByDestination.end()) {
            return kUdpTxPacerBaseDestinationBytesPerSecond;
        }
        auto& flows = destination->second;
        for (auto it = flows.begin(); it != flows.end();) {
            if (now - it->second > kUdpTxPacerFlowActiveWindow) {
                it = flows.erase(it);
            } else {
                ++it;
            }
        }
        if (flows.empty()) {
            udpTxVisualFlowsByDestination.erase(destination);
            return kUdpTxPacerBaseDestinationBytesPerSecond;
        }
        return std::max(kUdpTxPacerBaseDestinationBytesPerSecond,
                        flows.size() * kUdpTxPacerVisualFlowBytesPerSecond);
    }

    static std::chrono::microseconds udp_tx_gap(const udp::Datagram& datagram,
                                                 size_t bytesPerSecond) {
        const size_t bytes = std::max<size_t>(datagram.bytes.size(), 1);
        const auto byteRateGap = std::chrono::microseconds(
            static_cast<int64_t>((bytes * 1000000ull) / bytesPerSecond));
        return std::max(
            std::chrono::duration_cast<std::chrono::microseconds>(kUdpTxPacerMinDatagramGap),
            byteRateGap);
    }

    bool send_udp_datagram_now(const sockaddr_in& address,
                               const udp::Datagram& datagram) {
        if (!udpOpen || datagram.bytes.empty()) {
            return false;
        }
        return connections.send_realtime({address.sin_addr.s_addr, ntohs(address.sin_port)}, datagram.bytes);
    }

    void udp_tx_thread_main() {
        for (;;) {
            PacedDatagram paced;
            std::string destinationKey;
            size_t destinationRate = kUdpTxPacerBaseDestinationBytesPerSecond;
            {
                std::unique_lock<std::mutex> lock(udpTxMutex);
                udpTxCv.wait(lock, [this] { return udpTxStop || !udpTxQueue.empty(); });
                for (;;) {
                    if (udpTxStop) return;
                    const auto now = std::chrono::steady_clock::now();
                    auto ready = udpTxQueue.end();
                    auto nextReady = std::chrono::steady_clock::time_point::max();
                    for (auto it = udpTxQueue.begin(); it != udpTxQueue.end(); ++it) {
                        const std::string key = udp_tx_address_key(it->address);
                        const auto lane = udpTxNextSendByDestination.find(key);
                        const auto eligibleAt = lane == udpTxNextSendByDestination.end() ?
                            now : lane->second;
                        if (eligibleAt <= now) {
                            ready = it;
                            break;
                        }
                        nextReady = std::min(nextReady, eligibleAt);
                    }
                    if (ready != udpTxQueue.end()) {
                        destinationKey = udp_tx_address_key(ready->address);
                        destinationRate = udp_tx_target_rate_locked(destinationKey, now);
                        paced = std::move(*ready);
                        udpTxQueue.erase(ready);
                        break;
                    }
                    udpTxCv.wait_until(lock, nextReady);
                }
            }

            const auto gap = udp_tx_gap(paced.datagram, destinationRate);
            // Realtime carrier backpressure drops this datagram. Reliability
            // retries are owned by KCP, never by this visual producer.
            send_udp_datagram_now(paced.address, paced.datagram);
            std::lock_guard<std::mutex> lock(udpTxMutex);
            udpTxNextSendByDestination[destinationKey] =
                std::chrono::steady_clock::now() + gap;
        }
    }

    void start_udp_tx_pacer() {
        std::lock_guard<std::mutex> lock(udpTxMutex);
        if (udpTxRunning) return;
        udpTxQueue.clear();
        udpTxNextSendByDestination.clear();
        udpTxVisualFlowsByDestination.clear();
        udpTxStop = false;
        udpTxRunning = true;
        udpTxThread = std::thread([this] { udp_tx_thread_main(); });
    }

    void stop_udp_tx_pacer() {
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            if (!udpTxRunning) {
                udpTxQueue.clear();
                udpTxNextSendByDestination.clear();
                udpTxVisualFlowsByDestination.clear();
                return;
            }
            udpTxStop = true;
            udpTxQueue.clear();
            udpTxNextSendByDestination.clear();
            udpTxVisualFlowsByDestination.clear();
        }
        udpTxCv.notify_all();
        if (udpTxThread.joinable()) udpTxThread.join();
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            udpTxStop = false;
            udpTxRunning = false;
        }
    }

    bool enqueue_udp_tx_datagrams(const sockaddr_in& address,
                                  std::vector<udp::Datagram> datagrams,
                                  std::string_view senderId,
                                  std::string_view receiverId,
                                  udp::PacketType type) {
        if (datagrams.empty()) return false;
        const uint32_t sequence = datagrams.front().sequence;
        const std::string queueKey =
            udp_tx_queue_key(address, senderId, type, receiverId);
        {
            std::lock_guard<std::mutex> lock(udpTxMutex);
            if (!udpTxRunning) return false;
            const std::string destinationKey = udp_tx_address_key(address);
            const auto now = std::chrono::steady_clock::now();
            if (is_paced_visual_type(type)) {
                udpTxVisualFlowsByDestination[destinationKey][queueKey] = now;
            }
            (void)udp_tx_target_rate_locked(destinationKey, now);

            for (auto it = udpTxQueue.begin(); it != udpTxQueue.end();) {
                if (it->queueKey == queueKey && it->datagram.sequence < sequence) {
                    it = udpTxQueue.erase(it);
                } else {
                    ++it;
                }
            }
            for (udp::Datagram& datagram : datagrams) {
                udpTxQueue.push_back({address, std::move(datagram), queueKey});
            }
            while (udpTxQueue.size() > kUdpTxPacerMaxQueuedDatagrams) {
                const auto completeSnapshot = std::find_if(
                    udpTxQueue.begin(), udpTxQueue.end(), [](const PacedDatagram& queued) {
                        return queued.datagram.chunkIndex == 0;
                    });
                if (completeSnapshot == udpTxQueue.end()) break;
                const std::string droppedKey = completeSnapshot->queueKey;
                const uint32_t droppedSequence = completeSnapshot->datagram.sequence;
                for (auto it = udpTxQueue.begin(); it != udpTxQueue.end();) {
                    if (it->queueKey == droppedKey &&
                        it->datagram.sequence == droppedSequence) {
                        it = udpTxQueue.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        udpTxCv.notify_one();
        return true;
    }

    void close_reliable(socket_t& id) {
        if (id != kInvalidSocket) connections.disconnect(id);
        id = kInvalidSocket;
    }
    void close_all() {
        stop_udp_tx_pacer();
        if (cloudChannel) cloudChannel->close();
        cloudCipher.reset();
        connections.close();
        socket = kInvalidSocket;
        listening = udpOpen = false;
        directPeers.clear();
    }

    void reset_runtime(bool keepConfiguration, bool preserveAcceptedEvents = false) {
        std::deque<Event> accepted;
        if (preserveAcceptedEvents) accepted = std::move(events);
        close_all();
        if (!cloudResumeToken.empty()) cloudRetireToken = cloudResumeToken;
        cloudResumeToken.clear(); cloudMeshKey.clear();
        cloudRecovering = cloudRecoveryOpening = false;
        rx.clear();
        tx.clear();
        events.clear();
        peerNames.clear();
        cloudDepartedPeers.clear();
        cloudDepartedOrder.clear();
        meshEnabled = false; meshPuppets.clear();
        meshRoutes.clear(); meshLinks.clear(); meshRx.clear();
        cloudLinkStarted.clear();
        cloudNewcomers.clear();
        peerDelivery.clear();
        deferredSends.clear(); futureGameplay.clear(); barrierExpected.clear(); barrierSeen.clear(); futureBarrierSeen.clear();
        deliveryBytes = 0; settingsGeneration = 0; announcedStage.clear(); deliveryFailure = false;
        settingsRequested = settingsBarrier = barrierObserver = barrierReady = false;
        peerStages.clear();
        peerPoseStages.clear();
        poseAckSequences.clear();
        poseAckHistories.clear();
        poseAckResetFloors.clear();
        status.clientId.clear();
        status.ownerClientId.clear();
        status.udpToken.clear();
        status.udpReady = false;
        status.welcomed = false;
        status.isOwner = false;
        status.voiceSettingsReady = false;
        status.saveRecoverySettingsReady = false;
        status.semanticVisualsReady = false;
        status.snapshotDeltasReady = false;
        status.state = State::Disconnected;
        cloudJoinedAt = {};
        nextDirectPeerId = 1;
        reconnectTicks = 0;
        relayUdpRegisterTicks = 0;
        helloSent = false;
        authNonce.clear();
        authSalt.clear();
        challengeStartedAt = {};
        handshakeRejected = false;
        udpRemoteAddressKnown = false;
        udpDecoder.reset();
        eventQueueOverflow = false;
        if (!keepConfiguration) {
            cloudRetireToken.clear();
            cloudDisconnectedAt.reset();
            status = {};
            automaticReconnect = true;
            sessionEstablished = false;
            relayCreateRoom = false;
            relayMayRecreateRoom = false;
            wantPuppet = true;
            wantMidna = false;
            lobbyCode.clear();
            sessionId.clear();
            sessionKey.clear();
            cloudChannel.reset();
            cloudUrl.clear();
            cloudStunHost.clear();
            cloudStunPort = 0;
        }
        if (preserveAcceptedEvents) events = std::move(accepted);
    }

    void fail(const std::string& reason, bool allowReconnect = true,
              bool preserveAcceptedEvents = true) {
        const bool wasActive = status.state != State::Disconnected || status.reconnecting;
        const bool reconnect = sessionEstablished && automaticReconnect && allowReconnect;
        const bool repeatCloudAttempt = status.mode == Mode::CloudRoom &&
            status.reconnecting && !status.welcomed && reconnect;
        if (wasActive && status.mode == Mode::CloudRoom) {
            const auto now = std::chrono::steady_clock::now();
            const auto uptime = cloudJoinedAt == std::chrono::steady_clock::time_point{} ? 0 :
                std::chrono::duration_cast<std::chrono::milliseconds>(now - cloudJoinedAt).count();
            emit(EventKind::Diagnostic, {}, "room_session_failed uptime_ms=" +
                 std::to_string(uptime) + " reconnect=" + (reconnect ? "yes" : "no") +
                 " retained_bytes=" + std::to_string(deliveryBytes) + " reason=" + reason);
            for (const auto& [id, link] : meshLinks)
                emit(EventKind::Diagnostic, id, "peer_at_room_failure " +
                     connections.mesh_diagnostics(id));
            if (sessionEstablished && !cloudDisconnectedAt) cloudDisconnectedAt = now;
        }
        // A reconnect is a new transport epoch. Retaining UDP decoder
        // sequences, ACK baselines, peer identity or the old UDP endpoint can
        // make valid packets in the next connection look stale or route them
        // into the departed session.
        reset_runtime(true, preserveAcceptedEvents);
        status.error = reason;
        automaticReconnect = reconnect;
        status.enabled = reconnect;
        status.reconnecting = reconnect;
        if (wasActive && !repeatCloudAttempt) {
            emit(EventKind::Disconnected, {}, reason);
        }
    }

    bool flush(socket_t target, std::string& buffer) {
        if (target == kInvalidSocket) return false;
        if (buffer.empty()) return true;
        if (!connections.send(target, buffer.data(), buffer.size())) return false;
        buffer.clear();
        return true;
    }

    bool queue(socket_t target, std::string& buffer, const json& message) {
        std::string line = encode_reliable_json(message,meshEnabled ? reliablePeerFrameLimit : reliableJsonLimit);
        line.push_back('\n');
        if (buffer.size() + line.size() > kReliableTxBufferMaxBytes) {
            return false;
        }
        buffer.append(line);
        return flush(target, buffer);
    }

    bool queue_primary(const json& message) {
        if (status.mode != Mode::CloudRoom) return queue(socket, tx, message);
        if (cloudRecovering && message.value("type", "") != "hello") return false;
        // This is an egress security boundary, not merely a Worker policy.
        // Gameplay payloads can only travel on the native ICE mesh.
        if (!cloudChannel || !cloudCipher || !cloudCipher->established() ||
            !cloud_control_type(message.value("type", ""))) return false;
        const std::string wire = cloudCipher->seal(message.dump());
        if (wire.empty()) return false;
        if (cloudChannel->send(wire)) return true;
        emit(EventKind::Diagnostic, {},
             "room_control_send_failed type=" + message.value("type", ""));
        deliveryFailure = true;
        return false;
    }

    bool queue_peer(Peer& peer, const json& message) {
        if (queue(peer.socket, peer.tx, message)) return true;
        // Match final's central send_json_to_peer failure contract: a peer
        // that cannot accept a bounded reliable frame is no longer usable.
        // Map erasure remains deferred to the owning pump iteration.
        close_reliable(peer.socket);
        return false;
    }

    void admit_mesh(const std::string& id) {
        if (meshRoutes.contains(id)) return;
        const auto link = connections.mesh_admit(id, true);
        if (link == kInvalidSocket) { deliveryFailure = true; return; }
        meshLinks[id] = link;
        peerDelivery.try_emplace(id);
        meshRoutes[id] = false;
        if (status.mode == Mode::CloudRoom)
            cloudLinkStarted[id] = {std::chrono::steady_clock::now()};
        emit(EventKind::RouteChanged, id,
             status.mode == Mode::CloudRoom ? "connecting" : "relay");
    }

    void remove_peer(const std::string& peerId, const std::string& reason) {
        auto it = directPeers.find(peerId);
        if (it == directPeers.end()) {
            return;
        }
        close_reliable(it->second.socket);
        directPeers.erase(it);
        peerNames.erase(peerId);
        forget_pose_ack_history(peerId);
        peerStages.erase(peerId);
        peerPoseStages.erase(peerId);
        status.welcomed = std::any_of(directPeers.begin(), directPeers.end(),
                                     [](const auto& item) { return item.second.welcomed; });
        status.semanticVisualsReady = status.welcomed && direct_semantic_visuals_ready();
        status.snapshotDeltasReady = status.welcomed && direct_snapshot_deltas_ready();
        const json left = {
            {"type", "peer_left"},
            {"client_id", peerId},
            {"semantic_visuals_ready", status.semanticVisualsReady},
            {"snapshot_deltas_ready", status.snapshotDeltasReady},
        };
        emit(EventKind::PeerLeft, peerId, reason,
             left);
        std::vector<std::string> failed;
        for (auto& [id, peer] : directPeers) {
            if (peer.welcomed && !peer.kickPending && !queue_peer(peer, left)) {
                failed.push_back(id);
            }
        }
        for (const std::string& id : failed) {
            // Defer map erasure to pump_direct_peers(). This function can be
            // reached while that map is being iterated.
            close_reliable(directPeers.at(id).socket);
        }
    }

    bool broadcast(const json& message, const std::string& excluded = {}) {
        bool sentAny = false;
        std::vector<std::string> failed;
        for (auto& [id, peer] : directPeers) {
            if (!peer.welcomed || peer.kickPending || id == excluded) {
                continue;
            }
            if (queue_peer(peer, message)) {
                sentAny = true;
            } else {
                failed.push_back(id);
            }
        }
        for (const std::string& id : failed) {
            // See remove_peer(): socket invalidation is safe during an
            // iteration; map erasure is performed by the next pump.
            close_reliable(directPeers.at(id).socket);
        }
        return sentAny || directPeers.empty();
    }

    json direct_peer_list(const std::string& excluded) const {
        json peers = json::array();
        for (const auto& [id, peer] : directPeers) {
            if (peer.welcomed && !peer.kickPending && id != excluded) {
                peers.push_back({{"client_id", id}, {"name", peer.name}});
            }
        }
        return peers;
    }

    bool direct_semantic_visuals_ready(const Peer* pendingPeer = nullptr) const {
        for (const auto& [id, peer] : directPeers) {
            (void)id;
            if (!peer.kickPending && (peer.welcomed || &peer == pendingPeer) &&
                !peer.supportsSemanticVisuals) {
                return false;
            }
        }
        return true;
    }

    bool direct_snapshot_deltas_ready(const Peer* pendingPeer = nullptr) const {
        if (!supportsSnapshotDeltas) return false;
        for (const auto& [id, peer] : directPeers) {
            (void)id;
            if (!peer.kickPending && (peer.welcomed || &peer == pendingPeer) &&
                !peer.supportsSnapshotDeltas) {
                return false;
            }
        }
        return true;
    }

    void welcome_direct_peer(Peer& peer) {
        if (peer.welcomed) {
            return;
        }
        const bool semanticVisualsReady = direct_semantic_visuals_ready(&peer);
        const bool snapshotDeltasReady = direct_snapshot_deltas_ready(&peer);
        const json welcome = {
            {"type", "welcome"},
            {"protocol_version", 3},
            {"room_id", status.room},
            {"client_id", peer.id},
            {"direct_peer_name", status.name},
            {"dummy_model", status.settings.dummyModel},
            {"sync_flags", status.settings.syncFlags},
            {"save_recovery", status.settings.saveRecovery},
            {"sync_world", status.settings.syncWorld},
            {"remote_collision", status.settings.remoteCollision},
            {"pvp", status.settings.pvp && status.settings.remoteCollision},
            {"voice_proximity", status.settings.voiceProximity},
            {"voice_proximity_range", status.settings.voiceProximityRange},
            {"semantic_visuals_ready", semanticVisualsReady},
            {"snapshot_deltas_ready", snapshotDeltasReady},
            {"want_puppet", status.settings.dummyModel},
            {"want_midna", wantMidna},
            {"peers", direct_peer_list(peer.id)},
        };
        peer.welcomed = queue_peer(peer, welcome);
        status.welcomed = status.welcomed || peer.welcomed;
        status.semanticVisualsReady = status.welcomed && direct_semantic_visuals_ready();
        status.snapshotDeltasReady = status.welcomed && direct_snapshot_deltas_ready();
    }

    void send_hello() {
        if (helloSent || authNonce.empty() ||
            (status.mode != Mode::CloudRoom && socket == kInvalidSocket)) {
            return;
        }
        json hello = {
            {"type", "hello"},
            {"protocol_version", status.mode == Mode::CloudRoom ? 5 :
                                 (status.mode == Mode::Relay ? 4 : 3)},
            {"room_id", status.room},
            {"session_id", sessionId},
            {"name", status.name},
            {"want_puppet", wantPuppet},
            {"want_midna", wantMidna},
            {"capabilities", {
                {kSemanticVisualCapability, true},
                {kSnapshotDeltaCapability, supportsSnapshotDeltas},
            }},
        };
        if (is_room_mode(status.mode)) {
            const auto verifier = auth::lobby_verifier(lobbyCode, status.room,
                status.mode == Mode::Relay ? sessionKey : std::string{});
            const auto key = auth::room_proof_key(verifier, authSalt);
            if (!key) { fail("Invalid lobby authentication challenge", false); return; }
            hello["code_proof"] = auth::room_proof(*key, authNonce);
            if (relayCreateRoom) hello["code_verifier"] = auth::hex(verifier);
            hello["action"] = relayCreateRoom ? "create" : "join";
            if (status.mode == Mode::CloudRoom) {
                hello["capabilities"]["room_resume_v1"] = true;
                if (cloudRecovering) {
                    hello["action"] = "join";
                    hello["resume_token"] = cloudResumeToken;
                    hello["resume_client_id"] = status.clientId;
                } else if (!cloudRetireToken.empty()) hello["retire_token"] = cloudRetireToken;
            }
            if (relayCreateRoom) {
                hello["settings"] = settings_json(status.settings);
            }
        } else {
            hello["session_proof"] = auth::direct_proof(sessionKey, authNonce);
        }
        helloSent = queue_primary(hello);
    }

    bool begin_cloud() {
        events.clear();
        ++connectionEpoch;
        cloudCipher.emplace(cloud_room_route(status.room));
        if (!cloudCipher->valid()) {
            fail("Could not initialize cloud room encryption", false);
            return false;
        }
        stop_udp_tx_pacer();
        connections.close();
        socket = kInvalidSocket;
        udpOpen = false;
        if (!connections.open("0.0.0.0", 0, 1, false) || !open_udp("0.0.0.0", 0)) {
            fail("Direct UDP socket could not open", false);
            return false;
        }
        std::string error;
        if (!cloudChannel || !cloudChannel->open(cloudUrl, error)) {
            fail(error.empty() ? "Cloudflare room connection could not open" : error, false);
            return false;
        }
        status.state = State::Connecting;
        return true;
    }

    bool healthy_cloud_gameplay() const {
        return meshEnabled && !deliveryFailure && !settingsRequested && !settingsBarrier &&
            std::all_of(meshLinks.begin(), meshLinks.end(),
                [this](const auto& peer) { return delivery_direct(peer.first); });
    }

    bool recover_cloud_socket(const RoomChannel::Event& event) {
        // Protocol rejections, kicks and intentional closes remain terminal.
        if (cloudResumeToken.empty() || !status.welcomed ||
            !(event.closeCode == 0 || event.closeCode == 1001 ||
              event.closeCode == 1006 || event.closeCode == 1011) ||
            (event.closeError != 0 && event.closeError != 6) || !healthy_cloud_gameplay()) return false;
        const auto now = std::chrono::steady_clock::now();
        const bool retry = cloudRecovering;
        if (!cloudRecovering) {
            cloudRecoveryStarted = now;
            emit(EventKind::Diagnostic, {}, "room_recovery_started gameplay=preserved");
        }
        cloudRecovering = true;
        cloudRecoveryOpening = false;
        cloudChannel->close();
        cloudCipher.reset();
        helloSent = false; authNonce.clear(); authSalt.clear(); challengeStartedAt = {};
        cloudRetryAt = now + (retry ? std::chrono::milliseconds(500) : std::chrono::milliseconds(0));
        return true;
    }

    bool pump_cloud_recovery() {
        if (!cloudRecovering) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - cloudRecoveryStarted >= cloudRecoveryLimit || !healthy_cloud_gameplay()) {
            fail("Room recovery could not preserve gameplay; reconnecting the session");
            return false;
        }
        if (!cloudRecoveryOpening && now >= cloudRetryAt) {
            cloudCipher.emplace(cloud_room_route(status.room));
            std::string error;
            cloudRecoveryOpening = cloudCipher->valid() && cloudChannel->open(cloudUrl, error);
            cloudRetryAt = now + std::chrono::milliseconds(500);
        }
        return true;
    }

    void accept_cloud_resume(const json& message) {
        // Preserve every gameplay sequence, journal and decoder only when the
        // server confirms the exact session. A changed roster/settings epoch
        // needs the ordinary, visible full reconnect instead.
        std::set<std::string> peers;
        bool valid = cloudRecovering && message.value("protocol_version", 0) == 5 &&
            message.value("client_id", "") == status.clientId &&
            message.value("owner_client_id", "") == status.ownerClientId &&
            message.value("mesh_key", "") == cloudMeshKey &&
            message.value("settings_generation", uint64_t(-1)) == settingsGeneration &&
            message.value("settings", json::object()) == settings_json(status.settings) &&
            message.value("semantic_visuals_ready", false) == status.semanticVisualsReady &&
            message.value("snapshot_deltas_ready", false) == status.snapshotDeltasReady &&
            message.contains("peers") && message.at("peers").is_array() && healthy_cloud_gameplay();
        if (valid) for (const auto& peer : message.at("peers")) {
            const auto id = peer.value("client_id", "");
            if (!meshLinks.contains(id) || !peers.insert(id).second) valid = false;
        }
        if (!valid || peers.size() != meshLinks.size()) {
            fail("Room changed during recovery; reconnecting the session"); return;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cloudRecoveryStarted).count();
        cloudRecovering = cloudRecoveryOpening = false;
        emit(EventKind::Diagnostic, {}, "room_recovery_completed downtime_ms=" +
            std::to_string(elapsed) + " gameplay=preserved");
    }

    bool begin_connect() {
        events.clear();
        ++connectionEpoch;
        stop_udp_tx_pacer();
        connections.close();
        socket = kInvalidSocket;
        udpOpen = false;
        if (!connections.open("0.0.0.0", 0, 1, false, false, sessionKey)) {
            fail("UDP connection open failed", false);
            return false;
        }
        socket = static_cast<socket_t>(connections.connect(status.host, status.port));
        if (socket == kInvalidSocket || !setup_client_udp()) {
            fail("UDP connect failed");
            return false;
        }
        status.state = State::Connecting;
        return true;
    }

    bool begin_host() {
        events.clear();
        ++connectionEpoch;
        stop_udp_tx_pacer();
        if (!connections.open(status.bindHost, status.port, kMaxDirectPeers,
                              true, false, sessionKey)) {
            fail("UDP listen failed", false);
            return false;
        }
        listening = true;
        status.state = State::Listening;
        sessionEstablished = true;
        status.reconnecting = false;
        status.error.clear();
        return open_udp(status.bindHost, status.port);
    }

    bool open_udp(const std::string&, uint16_t) {
        stop_udp_tx_pacer();
        udpRemoteAddressKnown = false;
        udpDecoder.reset();
        udpOpen = true;
        start_udp_tx_pacer();
        return true;
    }

    bool setup_client_udp() {
        if (!open_udp("0.0.0.0", 0)) {
            return false;
        }
        if (!fill_ipv4(udpRemoteAddress, status.host, status.port)) {
            stop_udp_tx_pacer();
            udpOpen = false;
            return false;
        }
        udpRemoteAddressKnown = true;
        return true;
    }

    bool send_udp_datagram(const sockaddr_in& address, const udp::Datagram& datagram) {
        if (meshEnabled) {
            const auto info = udp::inspect_datagram(datagram.bytes);
            if (info && info->type == udp::PacketType::PoseAck && datagram.bytes.size() == 58 + sizeof(udp::AckPacket)) {
                udp::AckPacket ack{};
                std::memcpy(&ack, datagram.bytes.data() + 58, sizeof(ack));
                return connections.mesh_send(udp::acked_sender_id(ack), datagram.bytes);
            }
            if (info && (info->type == udp::PacketType::RemoteObject ||
                         info->type == udp::PacketType::VoiceOpus)) {
                std::vector<std::string> recipients;
                for (const auto& [id, name] : peerNames) recipients.push_back(id);
                return connections.mesh_send_many(recipients, datagram.bytes);
            }
        }
        return send_udp_datagram_now(address, datagram);
    }

    std::string local_udp_sender_id() const {
        if ((status.mode == Mode::DirectJoin || is_room_mode(status.mode)) &&
            !status.clientId.empty()) {
            return status.clientId;
        }
        return "direct";
    }

    void forget_pose_ack_history(const std::string& peerId) {
        const auto receiver = peerId + '\x1f';
        const auto sender = '\x1f' + peerId + '\x1f';
        std::erase_if(poseAckHistories, [&](const auto& entry) {
            return entry.first.starts_with(receiver) || entry.first.find(sender) != std::string::npos;
        });
    }

    static std::string pose_ack_key(std::string_view receiverId,
                                    std::string_view senderId,
                                    udp::PacketType type) {
        return std::string(receiverId) + '\x1f' + std::string(senderId) + '\x1f' +
               std::to_string(static_cast<uint8_t>(type));
    }

    uint32_t pose_ack_sequence(std::string_view receiverId,
                               std::string_view senderId,
                               udp::PacketType type) const {
        const auto it = poseAckSequences.find(pose_ack_key(receiverId, senderId, type));
        return it == poseAckSequences.end() ? 0 : it->second;
    }

    uint32_t relay_common_ack_sequence(std::string_view senderId,
                                       udp::PacketType type) const {
        if (!status.snapshotDeltasReady || peerNames.empty()) return 0;
        std::vector<const PoseAckHistory*> histories;
        for (const auto& [peerId, name] : peerNames) {
            (void)name;
            if (peerId == senderId) continue;
            const auto found = poseAckHistories.find(pose_ack_key(peerId, senderId, type));
            if (found == poseAckHistories.end()) return 0;
            histories.push_back(&found->second);
        }
        return common_pose_ack(histories);
    }

    bool stages_match(const json& message, const std::string& peerId,
                      std::string_view senderId) const {
        std::string source = message.value("stage", "");
        if (source.empty()) {
            const json state = message.value("state", json::object());
            if (state.is_object()) source = state.value("stage", "");
        }
        if (source.empty() && !senderId.empty()) {
            const auto pose = peerPoseStages.find(std::string(senderId));
            if (pose != peerPoseStages.end()) source = pose->second;
        }
        std::string targetStage;
        const auto presence = peerStages.find(peerId);
        if (presence != peerStages.end()) targetStage = presence->second.stage;
        if (targetStage.empty()) {
            const auto pose = peerPoseStages.find(peerId);
            if (pose != peerPoseStages.end()) targetStage = pose->second;
        }
        return source.empty() || targetStage.empty() || source == targetStage;
    }

    bool send_visual_to_direct_peers(const json& message, std::string_view senderId,
                                     udp::PacketType type,
                                     const std::string& excluded = {}) {
        bool sentAny = false;
        for (auto& [id, peer] : directPeers) {
            if (id == excluded || !peer.welcomed || peer.kickPending ||
                !peer.udpAddressKnown ||
                !peer.wantPuppet || (type == udp::PacketType::MidnaMsgpack && !peer.wantMidna) ||
                (type == udp::PacketType::SemanticPoseMsgpack &&
                 !peer.supportsSemanticVisuals) ||
                !stages_match(message, id, senderId)) {
                continue;
            }
            json wireMessage = message;
            const uint32_t sequence = wireMessage.value("sequence", 0U);
            const uint32_t baseline = pose_ack_sequence(id, senderId, type);
            const bool allowSnapshotDelta =
                supportsSnapshotDeltas && peer.supportsSnapshotDeltas;
            std::string error;
            uint64_t fullSnapshotPreparedBytes = 0;
            uint64_t fullSnapshotWireBytes = 0;
            if (visualWireDiagnostics &&
                type == udp::PacketType::SemanticPoseMsgpack) {
                json fullSnapshotMessage = message;
                std::string diagnosticError;
                if (poseDeltaPrepare == nullptr ||
                    poseDeltaPrepare(fullSnapshotMessage, std::string(senderId),
                                  static_cast<uint8_t>(type), sequence, baseline,
                                  false, false, diagnosticError)) {
                    fullSnapshotMessage.erase("_snapshot_debug_reason");
                    fullSnapshotPreparedBytes = json::to_msgpack(fullSnapshotMessage).size();
                    const auto fullSnapshotDatagrams = udp::encode_message(
                        fullSnapshotMessage, senderId, type, &diagnosticError);
                    for (const auto& datagram : fullSnapshotDatagrams) {
                        fullSnapshotWireBytes += datagram.bytes.size();
                    }
                }
            }
            if (poseDeltaPrepare != nullptr &&
                !poseDeltaPrepare(wireMessage, std::string(senderId),
                               static_cast<uint8_t>(type), sequence,
                               baseline, allowSnapshotDelta, visualWireDiagnostics,
                               error)) {
                emit(EventKind::Error, id, error);
                continue;
            }
            const std::string decision = wireMessage.value("_snapshot_debug_reason", "none");
            wireMessage.erase("_snapshot_debug_reason");
            lastVisualSend.snapshotChangedKeys = take_snapshot_debug_keys(
                wireMessage, "_snapshot_debug_changed_keys");
            lastVisualSend.snapshotUnchangedKeys = take_snapshot_debug_keys(
                wireMessage, "_snapshot_debug_unchanged_keys");
            lastVisualSend.snapshotRemovedKeys = take_snapshot_debug_keys(
                wireMessage, "_snapshot_debug_removed_keys");
            lastVisualSend.fullMsgpackBytes += json::to_msgpack(message).size();
            lastVisualSend.preparedMsgpackBytes += json::to_msgpack(wireMessage).size();
            lastVisualSend.snapshotBaseline = baseline;
            lastVisualSend.snapshotDecision = decision;
            if (wireMessage.value("snapshot_delta_v1", false)) {
                ++lastVisualSend.snapshotDeltas;
            } else {
                ++lastVisualSend.snapshotFulls;
            }
            const auto datagrams = udp::encode_message(wireMessage, senderId, type, &error);
            if (datagrams.empty()) {
                emit(EventKind::Error, id, error);
                continue;
            }
            const bool peerOk = enqueue_udp_tx_datagrams(
                peer.udpAddress, datagrams, senderId, id, type);
            if (peerOk) {
                ++lastVisualSend.recipients;
                lastVisualSend.fullSnapshotPreparedMsgpackBytes +=
                    fullSnapshotPreparedBytes;
                lastVisualSend.fullSnapshotWireBytes += fullSnapshotWireBytes;
                lastVisualSend.datagrams += static_cast<uint32_t>(datagrams.size());
                for (const auto& datagram : datagrams) {
                    lastVisualSend.wireBytes += datagram.bytes.size();
                }
            }
            sentAny = peerOk || sentAny;
        }
        return sentAny || directPeers.empty();
    }

    bool send_object_to_direct_peers(const udp::RemoteObjectPacket& object,
                                     std::string_view senderId,
                                     const std::string& excluded = {}) {
        const auto datagram = udp::encode_remote_object(senderId, object);
        bool sentAny = false;
        for (auto& [id, peer] : directPeers) {
            if (id == excluded || !peer.welcomed || peer.kickPending ||
                !peer.udpAddressKnown) {
                continue;
            }
            sentAny = send_udp_datagram(peer.udpAddress, datagram) || sentAny;
        }
        return sentAny || directPeers.empty();
    }

    bool send_voice_to_direct_peers(std::span<const uint8_t> opus,
                                   uint32_t sequence, const udp::VoicePosition& position,
                                   std::string_view senderId,
                                   std::string_view excluded = {}) {
        const auto datagram = udp::encode_voice(senderId, sequence, position, opus);
        if (datagram.bytes.empty()) return false;
        bool sentAny = false;
        for (auto& [id, peer] : directPeers) {
            if (id == excluded || !peer.welcomed || peer.kickPending ||
                !peer.udpAddressKnown) continue;
            sentAny = send_udp_datagram(peer.udpAddress, datagram) || sentAny;
        }
        return sentAny || directPeers.empty();
    }

    void send_relay_udp_registration() {
        if (status.mode != Mode::Relay || !status.welcomed || !udpRemoteAddressKnown ||
            status.clientId.empty() || status.udpToken.empty()) {
            return;
        }
        send_udp_datagram(udpRemoteAddress,
                          udp::encode_relay_registration(status.clientId, status.udpToken));
    }

    bool same_endpoint(const sockaddr_in& left, const sockaddr_in& right) const {
        return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
               left.sin_addr.s_addr == right.sin_addr.s_addr;
    }

    void handle_udp_result(udp::DecodeResult decoded, const sockaddr_in& from) {
        if (decoded.kind == udp::DecodeKind::None ||
            decoded.senderId == local_udp_sender_id()) {
            return;
        }
        if (status.mode == Mode::DirectHost) {
            auto peer = directPeers.find(decoded.senderId);
            if (peer == directPeers.end() || peer->second.kickPending) {
                return;
            }
            peer->second.udpAddress = from;
            peer->second.udpAddressKnown = true;
        }

        if (decoded.kind == udp::DecodeKind::Message) {
            if (decoded.type == udp::PacketType::MidnaMsgpack) return;
            json routed = std::move(decoded.message);
            if (decoded.senderId != "direct") {
                routed["client_id"] = decoded.senderId;
            }
            if (poseDeltaExpand != nullptr) {
                std::string error;
                if (!poseDeltaExpand(routed, decoded.senderId,
                                  static_cast<uint8_t>(decoded.type), decoded.sequence,
                                  error)) {
                    udpDecoder.discard_message(decoded.messageToken);
                    if (error.rfind("semantic snapshot delta", 0) == 0) {
                        send_udp_datagram(from, udp::encode_ack(
                            local_udp_sender_id(), decoded.senderId, decoded.type,
                            decoded.sequence, udp::AckBaselineReset));
                    }
                    emit(EventKind::Error, decoded.senderId,
                         error.empty() ? "pose delta expansion failed" : error);
                    return;
                }
            }
            std::string poseStage = routed.value("stage", "");
            if (poseStage.empty()) {
                const json state = routed.value("state", json::object());
                if (state.is_object()) poseStage = state.value("stage", "");
            }
            // A corrupt or missing pose delta is not a valid observation
            // and must not poison DirectHost's stage-routing fallback.
            if (!decoded.senderId.empty() && !poseStage.empty()) {
                peerPoseStages[decoded.senderId] = std::move(poseStage);
            }
            Event event;
            event.kind = EventKind::UdpMessage;
            event.peerId = decoded.senderId.empty() ? "direct" : decoded.senderId;
            event.message = routed;
            event.udpType = decoded.type;
            event.udpSequence = decoded.sequence;
            event.udpStressFlags = decoded.stressFlags;
            if (!push_event(std::move(event))) {
                udpDecoder.discard_message(decoded.messageToken);
                return;
            }

            if (status.mode == Mode::DirectHost) {
                send_visual_to_direct_peers(routed, decoded.senderId, decoded.type,
                                            decoded.senderId);
            }
            udpDecoder.commit_message(decoded.messageToken);
            if (decoded.type == udp::PacketType::PoseMsgpack ||
                decoded.type == udp::PacketType::SemanticPoseMsgpack ||
                decoded.type == udp::PacketType::MidnaMsgpack) {
                send_udp_datagram(from, udp::encode_ack(local_udp_sender_id(), decoded.senderId,
                                                        decoded.type, decoded.sequence,
                                                        decoded.stressFlags));
            }
        } else if (decoded.kind == udp::DecodeKind::Voice) {
            Event event;
            event.kind = EventKind::UdpVoice;
            event.peerId = decoded.senderId.empty() ? "direct" : decoded.senderId;
            event.udpType = decoded.type;
            event.udpSequence = decoded.sequence;
            event.voice = decoded.voice;
            event.voicePosition = decoded.voicePosition;
            if (!push_event(std::move(event))) return;
            if (status.mode == Mode::DirectHost)
                send_voice_to_direct_peers(decoded.voice, decoded.sequence,
                                           decoded.voicePosition,
                                           decoded.senderId, decoded.senderId);
        } else if (decoded.kind == udp::DecodeKind::RemoteObject) {
            if (!status.settings.syncWorld || decoded.remoteObject.objectKind == 0) return;
            Event event;
            event.kind = EventKind::UdpRemoteObject;
            event.peerId = decoded.senderId.empty() ? "direct" : decoded.senderId;
            event.udpType = decoded.type;
            event.udpSequence = decoded.sequence;
            event.remoteObject = decoded.remoteObject;
            if (!push_event(std::move(event))) return;
            if (status.mode == Mode::DirectHost) {
                send_object_to_direct_peers(decoded.remoteObject, decoded.senderId,
                                            decoded.senderId);
            }
        } else if (decoded.kind == udp::DecodeKind::Ack) {
            const auto ackedType = static_cast<udp::PacketType>(decoded.ack.ackedType);
            const std::string ackedSender = udp::acked_sender_id(decoded.ack);
            const std::string key = pose_ack_key(decoded.senderId, ackedSender, ackedType);
            uint32_t& ackedSequence = poseAckSequences[key];
            if ((decoded.ack.stressFlags & udp::AckBaselineReset) != 0) {
                poseAckResetFloors[key] = std::max(poseAckResetFloors[key],
                                                   decoded.ack.sequence);
                ackedSequence = 0;
                poseAckHistories.erase(key);
                return;
            }
            const auto resetFloor = poseAckResetFloors.find(key);
            if (resetFloor != poseAckResetFloors.end()) {
                if (decoded.ack.sequence <= resetFloor->second) return;
                poseAckResetFloors.erase(resetFloor);
            }
            if (decoded.ack.sequence <= ackedSequence) return;
            ackedSequence = decoded.ack.sequence;
            remember_pose_ack(poseAckHistories[key], decoded.ack.sequence);
            Event event;
            event.kind = EventKind::UdpAck;
            event.peerId = decoded.senderId;
            event.udpType = ackedType;
            event.udpSequence = decoded.ack.sequence;
            event.udpStressFlags = decoded.ack.stressFlags;
            event.detail = ackedSender;
            (void)push_event(std::move(event));
        }
    }

    void pump_udp() {
        if (!udpOpen) return;
        std::array<uint8_t, 58 + udp::kChunkPayloadBytes> packet{};
        if (meshEnabled) {
            std::string peerId;
            for (size_t n = 0; n < 512; ++n) {
                const int size = connections.mesh_receive(peerId, packet);
                if (size < 0) break;
                if (!peerNames.contains(peerId)) continue;
                const std::span<const uint8_t> wire(packet.data(), static_cast<size_t>(size));
                const auto info = udp::inspect_datagram(wire);
                if (!info || info->senderId != peerId || info->type == udp::PacketType::MidnaMsgpack ||
                    info->type == udp::PacketType::RelayRegister) continue;
                if (!wantPuppet && (info->type == udp::PacketType::PoseJson || info->type == udp::PacketType::PoseMsgpack ||
                    info->type == udp::PacketType::SemanticPoseMsgpack)) continue;
                handle_udp_result(udpDecoder.accept(wire), udpRemoteAddress);
            }
        }
        if (status.mode == Mode::CloudRoom) return;
        while (true) {
            UdpConnection::Address address;
            UdpConnection::Id authenticatedPeer = UdpConnection::invalid;
            const int count = connections.receive_realtime(address, packet, &authenticatedPeer);
            if (count < 0) return;
            sockaddr_in from{};
            from.sin_family = AF_INET;
            from.sin_addr.s_addr = address.ipv4;
            from.sin_port = htons(address.port);
            if (status.mode == Mode::Relay &&
                (!udpRemoteAddressKnown || !same_endpoint(from, udpRemoteAddress))) {
                continue;
            }
            const std::span<const uint8_t> wire(
                packet.data(), static_cast<size_t>(count));
            const std::optional<udp::DatagramInfo> info = udp::inspect_datagram(wire);
            if (!info.has_value()) continue;
            if (!dusk::multiplayer::kRemoteMidnaStreamingEnabled &&
                info->type == udp::PacketType::MidnaMsgpack) {
                continue;
            }

            const std::string admittedSender = info->senderId.empty() ?
                std::string("direct") : info->senderId;
            if (status.mode == Mode::DirectHost) {
                auto peer = directPeers.find(admittedSender);
                if (peer == directPeers.end() || !peer->second.welcomed ||
                    peer->second.kickPending ||
                    peer->second.socket != authenticatedPeer) {
                    // Unknown sender IDs must allocate zero Decoder state.
                    continue;
                }
                // Valid partial chunks are sufficient to learn the endpoint,
                // but only after the sender was admitted by its admitted session identity.
                peer->second.udpAddress = from;
                connections.bind_realtime(peer->second.socket, {from.sin_addr.s_addr, ntohs(from.sin_port)});
                peer->second.udpAddressKnown = true;
            } else if (admittedSender == local_udp_sender_id()) {
                continue;
            }

            auto decoded = udpDecoder.accept(wire);
            handle_udp_result(std::move(decoded), from);
            if (eventQueueOverflow) return;
        }
    }

    void update_connecting() {
        if (!connections.alive(socket)) { fail("UDP connect failed"); return; }
        if (!connections.connected(socket)) return;
        status.state = State::Connected;
        status.error.clear();
        challengeStartedAt = std::chrono::steady_clock::now();
        if (status.mode == Mode::Relay) {
            queue_primary({{"type", "auth_begin"}, {"protocol_version", 4},
                {"room_id", status.room},
                {"action", relayCreateRoom ? "create" : "join"}});
        }
    }

    void accept_peers() {
        while (listening) {
            UdpConnection::Address address;
            auto accepted = connections.accept(address);
            if (accepted == UdpConnection::invalid) return;
            if (directPeers.size() >= kMaxDirectPeers) { connections.disconnect(accepted); continue; }
            Peer peer;
            peer.socket = static_cast<socket_t>(accepted);
            peer.id = "direct" + std::to_string(nextDirectPeerId++);
            peer.authNonce = auth::random_nonce();
            queue_peer(peer, {{"type", "auth_challenge"}, {"nonce", peer.authNonce}});
            directPeers.emplace(peer.id, std::move(peer));
            status.state = State::Connected;
        }
    }

    void handle_direct_message(const json& input, Peer& sender) {
        if (!input.is_object()) {
            emit(EventKind::Error, sender.id, "non-object JSON message rejected");
            return;
        }
        const std::string type = input.value("type", "");
        if (type == "hello") {
            const std::string proof = input.value("session_proof", "");
            if (sender.authNonce.empty() || sender.welcomed ||
                input.value("protocol_version", 0) != 3 ||
                !auth::equal(proof, auth::direct_proof(sessionKey, sender.authNonce))) {
                queue_peer(sender, {{"type", "error"}, {"error", "Invalid direct invite code"}});
                sender.kickPending = true;
                return;
            }
            sender.authNonce.clear();
            sender.name = input.value("name", sender.name);
            sender.wantPuppet = input.value("want_puppet", sender.wantPuppet);
            sender.wantMidna = false;
            sender.supportsSemanticVisuals = has_semantic_visual_capability(input);
            sender.supportsSnapshotDeltas = has_snapshot_delta_capability(input);
            peerNames[sender.id] = sender.name;
            welcome_direct_peer(sender);
            const json joined = {
                {"type", "peer_joined"}, {"client_id", sender.id}, {"name", sender.name},
                {"semantic_visuals_ready", status.semanticVisualsReady},
                {"snapshot_deltas_ready", status.snapshotDeltasReady}};
            broadcast(joined, sender.id);
            emit(EventKind::PeerJoined, sender.id, sender.name, joined);
            return;
        }
        if (!sender.welcomed) {
            return;
        }
        // Kicking revokes the guest immediately. Keep its reliable socket
        // alive only long enough to deliver the explicit removal notice.
        if (sender.kickPending) {
            return;
        }

        json routed = input;
        routed["client_id"] = sender.id;
        if (type == "presence") {
            peerStages[sender.id] = {routed.value("stage", ""), 0};
        }
        if (type == "ping") {
            queue_peer(sender, {{"type", "pong"}});
            return;
        }
        if (type == "pong" || type == "error") {
            emit(EventKind::Message, sender.id, {}, routed);
            return;
        }
        if (type == "puppet_preference") {
            sender.wantPuppet = routed.value("want_puppet", sender.wantPuppet);
            sender.wantMidna = false;
            return;
        }
        // Direct guests cannot inject host-controlled voice settings into the
        // host's broadcast path.
        if (type == "voice_settings") return;
        if (type == "save_recovery_setting" || type == "save_recovery_setting_request") return;
        // A rebroadcast would give a guest's command the host's identity on
        // other clients. Readiness replies are for the host alone.
        if (type == "speedrun_check" || type == "speedrun_prompt" ||
            type == "speedrun_ready_count" || type == "speedrun_countdown" ||
            type == "speedrun_cancel" || type == "speedrun_start" ||
            type == "speedrun_reset") return;
        if (type == "speedrun_ready" || type == "speedrun_player_ready") {
            emit(EventKind::Message, sender.id, {}, routed);
            return;
        }

        const std::string target = routed.value("target_client_id", "");
        const bool targetedSync = type == "sync_request" ||
            (type == "save_snapshot" && routed.value("manual_sync", false));
        if (targetedSync && !status.settings.syncFlags) return;
        if (targetedSync && !target.empty() && target != "direct" && target != "host") {
            auto targetIt = directPeers.find(target);
            if (targetIt != directPeers.end() && targetIt->second.welcomed) {
                if (!queue_peer(targetIt->second, routed)) {
                    close_reliable(targetIt->second.socket);
                }
            } else {
                emit(EventKind::Error, target, "target peer unavailable", routed);
            }
            return;
        }

        // The host must retain a reliable mutation locally before exposing it
        // to other peers. Otherwise queue pressure can create a split state in
        // which joiners apply an event the host silently dropped.
        if (!emit(EventKind::Message, sender.id, {}, routed)) return;
        if (should_forward_direct(type) &&
            type != "sync_request" &&
            !(type == "save_snapshot" && routed.value("manual_sync", false))) {
            broadcast(routed, sender.id);
        }
    }

    bool defer_gameplay(const json& message) {
        const size_t bytes = message.dump().size();
        if (deferredSends.size() >= 4096 || !retain_delivery(bytes)) return false;
        deferredSends.push_back({message,bytes}); return true;
    }
    bool delivery_direct(const std::string& name) const {
#if defined(DUSKLIGHT_TRANSPORT_TESTING)
        if (std::getenv("DUSKLIGHT_TEST_FORCE_FALLBACK")) return false;
#endif
        return connections.mesh_direct(name);
    }
    bool send_delivery(const std::vector<std::string>& recipients, const json& body) {
        const size_t bytes = body.dump().size();
        if (bytes > reliableJsonLimit + 128 || recipients.size() > 7 ||
            bytes * recipients.size() > deliveryLimit - deliveryBytes) return false;
        json fallback = json::array();
        for (const auto& name : recipients) {
            auto& state = peerDelivery.at(name);
            if (state.sent.size() >= 4096 || state.nextSend == UINT64_MAX) return false;
        }
        for (const auto& name : recipients) {
            auto& state = peerDelivery.at(name);
            const uint64_t sequence = state.nextSend++;
            if (!retain_delivery(bytes)) return false;
            state.sent.emplace(sequence, PendingGameplay{body,bytes});
            if (delivery_direct(name)) {
                std::string unused;
                if (!queue(meshLinks.at(name),unused,{{"sequence",sequence},{"body",body}})) return false;
            } else fallback.push_back({{"id",name},{"sequence",sequence}});
        }
        // A relay broadcast uploads its body ONCE, even though recipients may
        // have different sequence numbers because of earlier targeted syncs.
        return fallback.empty() || status.mode == Mode::CloudRoom ||
            queue_primary({{"type","peer_reliable"},{"recipients",fallback},{"body",body}});
    }
    bool resend_delivery(const std::string& name, bool direct) {
        for (const auto& [sequence,item] : peerDelivery.at(name).sent) {
            if (direct) {
                std::string unused;
                if (!queue(meshLinks.at(name),unused,{{"sequence",sequence},{"body",item.value}})) return false;
            } else if (status.mode != Mode::CloudRoom &&
                       !queue_primary({{"type","peer_reliable"},
                           {"recipients",json::array({{{"id",name},{"sequence",sequence}}})},
                           {"body",item.value}})) return false;
        }
        return true;
    }
    void receive_delivery(const std::string& name, const json& frame) {
        auto found = peerDelivery.find(name);
        if (found == peerDelivery.end()) return; // already departed this room
        auto& state = found->second;
        if (frame.contains("ack")) {
            if (!frame.at("ack").is_number_unsigned()) { deliveryFailure = true; return; }
            const auto ack = frame.at("ack").get<uint64_t>();
            if (ack >= state.nextSend) { deliveryFailure = true; return; }
            while (!state.sent.empty() && state.sent.begin()->first <= ack) {
                deliveryBytes -= state.sent.begin()->second.bytes; state.sent.erase(state.sent.begin());
            }
            return;
        }
        const auto& number = frame.at("sequence");
        if (!number.is_number_unsigned()) { deliveryFailure = true; return; }
        const auto sequence = number.get<uint64_t>();
        if (!sequence || sequence == UINT64_MAX) { deliveryFailure = true; return; }
        if (sequence < state.nextReceive) { state.ackPending = state.nextReceive-1; return; }
        if (sequence - state.nextReceive >= 4096) { deliveryFailure = true; return; }
        const auto& body = frame.at("body");
        if (auto duplicate = state.received.find(sequence); duplicate != state.received.end()) {
            if (duplicate->second.value != body) deliveryFailure = true;
            return;
        }
        const size_t bytes = body.dump().size();
        if (bytes > reliableJsonLimit+128 || !retain_delivery(bytes)) { deliveryFailure = true; return; }
        state.received.emplace(sequence, PendingGameplay{body,bytes});
        while (!deliveryFailure && !state.received.empty() && state.received.begin()->first == state.nextReceive) {
            auto item = std::move(state.received.begin()->second);
            state.received.erase(state.received.begin()); deliveryBytes -= item.bytes;
            apply_delivery_body(name,item.value);
            ++state.nextReceive;
        }
        if (!deliveryFailure) state.ackPending = state.nextReceive-1;
    }
    bool send_peer_gameplay(const json& message) {
        if (message.dump().size() > reliableJsonLimit) return false;
        const auto target = message.value("target_client_id", "");
        if (message.value("type", "") == "sync_request" && target.empty()) return false;
        if (!target.empty() && !meshLinks.contains(target)) return false;
        std::vector<std::string> recipients;
        for (const auto& [name,link] : meshLinks) {
            if (!target.empty() && name != target) continue;
            recipients.push_back(name);
        }
        return send_delivery(recipients,{{"generation",settingsGeneration},{"payload",message}});
    }
    bool send_mesh_message(const json& message) {
        const auto type = message.value("type", "");
        const bool setting = type == "room_settings";
        if (setting && cloudRecovering) {
            emit(EventKind::Error, {}, "Lobby connection is recovering; retry the settings change shortly");
            return true; // Rejected explicitly without resetting healthy gameplay.
        }
        if (settingsRequested || settingsBarrier) return defer_gameplay(message);
        if (!setting) {
            if (status.mode != Mode::CloudRoom && type == "progression_state") {
                const auto stage = message.value("stage", "");
                if (stage != announcedStage) {
                    // Stage filtering is relay metadata, updated only on a
                    // stage change. Gameplay never waits for its receipt.
                    if (!queue_primary({{"type","peer_stage"},{"stage",stage}})) return false;
                    announcedStage = stage;
                }
            }
            return send_peer_gameplay(message);
        }
        settingsRequested = true; barrierStarted = std::chrono::steady_clock::now();
        return queue_primary(message);
    }
    bool send_voice_settings_to(const std::string& target = {}) {
        if (!is_room_mode(status.mode) || !status.isOwner || !status.welcomed) return false;
        json message = {{"type", "voice_settings"},
                        {"enabled", status.settings.voiceProximity},
                        {"range_percent", status.settings.voiceProximityRange}};
        if (!target.empty()) message["target_client_id"] = target;
        return send_mesh_message(message);
    }
    bool send_save_recovery_setting_to(const std::string& target = {}) {
        if (!is_room_mode(status.mode) || !status.isOwner || !status.welcomed) return false;
        json message = {{"type", "save_recovery_setting"},
                        {"enabled", status.settings.saveRecovery}};
        if (!target.empty()) message["target_client_id"] = target;
        return send_mesh_message(message);
    }
    void flush_deferred_gameplay() {
        while (!settingsRequested && !settingsBarrier && !deferredSends.empty() && !deliveryFailure) {
            auto item = std::move(deferredSends.front()); deferredSends.pop_front();
            deliveryBytes -= item.bytes;
            if (!send_mesh_message(item.value)) deliveryFailure = true;
        }
    }
    void apply_peer_gameplay(const std::string& name, json message) {
        const auto type = message.value("type", "");
        const bool cloudPeerControl = status.mode == Mode::CloudRoom &&
            (type == "ping" || type == "pong" || type == "presence" ||
             type == "puppet_preference" || type == "cloud_join_failure");
        if (!message.is_object() || message.dump().size() > reliableJsonLimit ||
            !(peer_delivery_type(type) || cloudPeerControl) ||
            (!message.value("target_client_id", "").empty() && message.value("target_client_id", "") != status.clientId)) {
            deliveryFailure = true; return;
        }
        if (type == "cloud_join_failure") {
            // A joined peer can report any client ID, including a healthy
            // newcomer. Only the owner's own ICE timeout may remove a peer.
            emit(EventKind::Diagnostic, name, "ignored_untrusted_newcomer_failure");
            return;
        }
        message["client_id"] = name;
        handle_primary_message(message);
    }
    void receive_primary(const json& message) {
        const auto type = message.value("type", "");
        if (status.mode == Mode::CloudRoom && type == "room_resumed") {
            accept_cloud_resume(message); return;
        }
        if (cloudRecovering && type != "auth_challenge") {
            fail("Room recovery was rejected; reconnecting the session"); return;
        }
        if (status.mode == Mode::CloudRoom && type != "auth_challenge" && type != "welcome" &&
            type != "peer_joined" && type != "peer_left" &&
            type != "owner_changed" && type != "room_settings" &&
            type != "settings_prepare" && type != "ice_signal" &&
            type != "kicked" && type != "error") {
            deliveryFailure = true;
            return;
        }
        if (meshEnabled && (type == "peer_reliable" || type == "peer_receipt")) {
            receive_delivery(message.at("client_id").get<std::string>(), message.at("frame"));
            return;
        }
        if (meshEnabled && type == "settings_prepare") {
            if (settingsBarrier || message.at("generation").get<uint64_t>() != settingsGeneration + 1) {
                deliveryFailure = true; return;
            }
            settingsBarrier = true; barrierReady = false;
            barrierObserver = message.value("observer", false);
            barrierStarted = std::chrono::steady_clock::now();
            barrierExpected.clear();
            if (!barrierObserver) for (const auto& member : message.at("participants")) {
                const auto name = member.get<std::string>();
                if (name == status.clientId) continue;
                if (!meshLinks.contains(name)) { deliveryFailure = true; return; }
                barrierExpected.insert(name);
                // Shares delivery order with gameplay, across BOTH carriers.
                if (!send_delivery({name},{{"barrier",settingsGeneration+1}})) deliveryFailure = true;
            }
            return;
        }
        if (meshEnabled && type == "room_settings") {
            const auto generation = message.at("settings_generation").get<uint64_t>();
            if (!settingsBarrier || generation != settingsGeneration+1 || (!barrierObserver && !barrierReady)) {
                deliveryFailure = true; return;
            }
            handle_primary_message(message);
            settingsGeneration = generation;
            settingsRequested = settingsBarrier = barrierObserver = barrierReady = false;
            barrierExpected.clear(); barrierSeen = std::move(futureBarrierSeen); futureBarrierSeen.clear();
            return;
        }
        if (meshEnabled && type == "error" && !settingsBarrier && settingsRequested) {
            settingsRequested = false;
        }
        handle_primary_message(message);
    }
    void apply_delivery_body(const std::string& name, const json& envelope) {
        auto& pending = futureGameplay[name];
        // The relay stream can deliver a settings commit and the next frame
        // in one read. Flush earlier deferred frames before that next frame.
        while (!pending.empty() && pending.front().value.at("generation").get<uint64_t>() == settingsGeneration) {
            auto item = std::move(pending.front()); pending.pop_front(); deliveryBytes -= item.bytes;
            apply_peer_gameplay(name,item.value.at("payload"));
        }
        if (envelope.contains("barrier")) {
            if (!envelope["barrier"].is_number_unsigned()) { deliveryFailure = true; return; }
            const auto marker = envelope["barrier"].get<uint64_t>();
            if (marker == settingsGeneration+1) {
                if (!barrierSeen.insert(name).second) deliveryFailure = true;
            } else if (marker == settingsGeneration+2 && settingsBarrier && (barrierReady || barrierObserver)) {
                if (!futureBarrierSeen.insert(name).second) deliveryFailure = true;
            } else deliveryFailure = true;
            return;
        }
        const auto& number = envelope.at("generation");
        if (!number.is_number_unsigned()) { deliveryFailure = true; return; }
        const auto generation = number.get<uint64_t>();
        if (generation < settingsGeneration || generation > settingsGeneration+1) { deliveryFailure = true; return; }
        if (generation == settingsGeneration) {
            if (barrierSeen.contains(name)) { deliveryFailure = true; return; }
            apply_peer_gameplay(name,envelope.at("payload"));
        } else {
            if (!settingsBarrier || (!barrierReady && !barrierObserver)) { deliveryFailure = true; return; }
            const size_t bytes = envelope.dump().size();
            if (pending.size() >= 4096 || !retain_delivery(bytes)) { deliveryFailure = true; return; }
            pending.push_back({envelope,bytes});
        }

    }
    void drain_peer_gameplay(const std::string& name, socket_t link, bool departing = false) {
        auto& pending = futureGameplay[name];
        // A remote peer can receive the commit before this peer does.
        // Retain its next-generation data until our authoritative commit.
        while (!pending.empty() && pending.front().value.at("generation").get<uint64_t>() == settingsGeneration) {
            auto item = std::move(pending.front()); pending.pop_front(); deliveryBytes -= item.bytes;
            apply_peer_gameplay(name,item.value.at("payload"));
        }
        // A departed peer may close its direct link before peer_left arrives.
        if (!receive(link,meshRx[name],[&](const json& frame) { receive_delivery(name,frame); },true) &&
            !departing && !(status.mode == Mode::CloudRoom && cloudNewcomers.contains(name) &&
                            !connections.connected(link))) {
            if (status.mode == Mode::CloudRoom)
                emit(EventKind::Diagnostic, name,
                     "peer_reliable_receive_failed " + connections.mesh_diagnostics(name));
            deliveryFailure = true;
        }
    }
    void pump_deliveries() {
        for (const auto& [name,link] : meshLinks) drain_peer_gameplay(name,link);
        for (auto& [name,state] : peerDelivery) if (state.ackPending) {
            const json receipt = {{"ack",state.ackPending}};
            bool accepted;
            if (delivery_direct(name)) {
                std::string unused; accepted = queue(meshLinks.at(name),unused,receipt);
            } else if (status.mode == Mode::CloudRoom) {
                // Keep the receipt pending until the direct path recovers.
                continue;
            } else accepted = queue_primary({{"type","peer_receipt"},{"target_client_id",name},{"frame",receipt}});
            if (!accepted) deliveryFailure = true;
            state.ackPending = 0;
        }
        if (settingsBarrier && !barrierObserver && !barrierReady &&
            std::all_of(barrierExpected.begin(),barrierExpected.end(),[&](const auto& id){return barrierSeen.contains(id);})) {
            barrierReady = true;
            if (!queue_primary({{"type","settings_ready"},{"generation",settingsGeneration+1}})) deliveryFailure = true;
        }
        if ((settingsBarrier || settingsRequested) && std::chrono::steady_clock::now()-barrierStarted > std::chrono::seconds(30))
            deliveryFailure = true;
        flush_deferred_gameplay();
    }

    void handle_primary_message(const json& message) {
        if (!message.is_object()) {
            emit(EventKind::Error, {}, "non-object JSON message rejected");
            return;
        }
        const std::string type = message.value("type", "");
        if (type == "auth_challenge" && (!status.welcomed || cloudRecovering) && !helloSent) {
            const std::string nonce = message.value("nonce", "");
            const std::string salt = message.value("salt", "");
            if (nonce.size() != 32 || auth::direct_proof("check", nonce).empty() ||
                (is_room_mode(status.mode) &&
                 (!auth::room_proof_key(auth::hmac_sha256("", ""), salt)))) {
                status.error = "Invalid authentication challenge";
                handshakeRejected = true;
                return;
            }
            authNonce = nonce;
            authSalt = salt;
            send_hello();
            return;
        }
        if (type == "welcome") {
            const int expectedVersion = status.mode == Mode::CloudRoom ? 5 :
                (status.mode == Mode::Relay ? 4 : 3);
            if (message.value("protocol_version", 0) != expectedVersion) {
                fail("Lobby transport protocol version mismatch", false);
                return;
            }
            if (status.mode == Mode::CloudRoom) {
                const std::string meshKey = message.value("mesh_key", "");
                if (!auth::unhex_digest(meshKey) ||
                    !connections.set_mesh_secret(meshKey)) {
                    fail("Cloud room service needs the secure peer update", false);
                    return;
                }
                cloudMeshKey = meshKey;
                const auto token = message.value("resume_token", "");
                cloudResumeToken = auth::unhex_digest(token) ? token : std::string{};
                cloudRetireToken.clear();
            }
            status.welcomed = true;
            if (is_room_mode(status.mode)) {
                settingsGeneration = message.value("settings_generation",uint64_t(0));
                settingsBarrier = barrierObserver = message.value("settings_pending",false);
                if (settingsBarrier) barrierStarted = std::chrono::steady_clock::now();
            }
            status.error.clear();
            status.clientId = message.value("client_id", "");
            status.ownerClientId = message.value("owner_client_id", "");
            status.udpToken = message.value("udp_token", "");
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            status.voiceSettingsReady = status.mode == Mode::DirectJoin || status.isOwner;
            status.saveRecoverySettingsReady = status.mode == Mode::DirectJoin || status.isOwner;
            status.semanticVisualsReady =
                message.value("semantic_visuals_ready", false);
            status.snapshotDeltasReady =
                message.value("snapshot_deltas_ready", false);
            if (status.mode == Mode::CloudRoom) {
                meshEnabled = connections.mesh_open(status.clientId, UdpConnection::invalid,
                    cloudStunHost, cloudStunPort);
                if (!meshEnabled) deliveryFailure = true;
                emit(EventKind::Diagnostic, {},
                     std::string("room_welcome mesh=") + (meshEnabled ? "ready" : "failed"));
                status.udpReady = meshEnabled;
                if (relayCreateRoom) relayMayRecreateRoom = true;
                relayCreateRoom = false;
                status.settings = parse_settings(message.value("settings", json::object()),
                                                 status.settings);
            } else if (status.mode == Mode::Relay) {
                const int port = message.value("stun_port", 0);
                meshEnabled = connections.mesh_open(status.clientId, socket,
                    port > 0 ? status.host : std::string{}, static_cast<uint16_t>(std::clamp(port, 0, 65535)));
                if (relayCreateRoom) {
                    relayMayRecreateRoom = true;
                }
                relayCreateRoom = false;
                status.settings = parse_settings(message.value("settings", json::object()),
                                                 status.settings);
                send_relay_udp_registration();
            } else {
                status.settings.dummyModel = message.value("dummy_model", status.settings.dummyModel);
                status.settings.syncFlags = message.value("sync_flags", status.settings.syncFlags);
                status.settings.saveRecovery = message.value("save_recovery", status.settings.saveRecovery);
                status.settings.syncWorld = message.value("sync_world", status.settings.syncWorld);
                status.settings.remoteCollision =
                    message.value("remote_collision", status.settings.remoteCollision);
                status.settings.pvp = message.value("pvp", status.settings.pvp) &&
                                      status.settings.remoteCollision;
                status.settings.voiceProximity =
                    message.value("voice_proximity", status.settings.voiceProximity);
                status.settings.voiceProximityRange = std::clamp(
                    message.value("voice_proximity_range", status.settings.voiceProximityRange),
                    0, 200);
                const std::string hostName = message.value("direct_peer_name", "Host");
                peerNames["direct"] = hostName;
            }
            const json peers = message.value("peers", json::array());
            if (!peers.is_array()) {
                emit(EventKind::Error, {}, "welcome peers field is not an array");
                return;
            }
            for (const json& peer : peers) {
                if (!peer.is_object()) continue;
                const std::string id = peer.value("client_id", "");
                if (!id.empty()) {
                    peerNames[id] = peer.value("name", id);
                    if (meshEnabled) {
                        admit_mesh(id);
                        meshPuppets[id] = peer.value("want_puppet", true);
                        peerStages[id] = {peer.value("stage", ""), 0};
                    }
                }
            }
            sessionEstablished = true;
            status.reconnecting = false;
            if (status.mode == Mode::CloudRoom) {
                cloudJoinedAt = std::chrono::steady_clock::now();
                if (cloudDisconnectedAt) {
                    const auto downtime = std::chrono::duration_cast<std::chrono::milliseconds>(
                        cloudJoinedAt - *cloudDisconnectedAt).count();
                    emit(EventKind::Diagnostic, {},
                         "room_reconnected downtime_ms=" + std::to_string(downtime));
                    cloudDisconnectedAt.reset();
                }
            }
            if (is_room_mode(status.mode) && !status.isOwner &&
                meshLinks.contains(status.ownerClientId) &&
                !send_mesh_message({{"type", "voice_settings_request"},
                                    {"target_client_id", status.ownerClientId}})) {
                deliveryFailure = true;
            }
            if (is_room_mode(status.mode) && !status.isOwner &&
                meshLinks.contains(status.ownerClientId) &&
                !send_mesh_message({{"type", "save_recovery_setting_request"},
                                    {"target_client_id", status.ownerClientId}})) {
                deliveryFailure = true;
            }
            if (status.mode == Mode::CloudRoom)
                emit(EventKind::Diagnostic, {},
                     "room_membership self=" + status.clientId +
                     " owner=" + status.ownerClientId +
                     " peers=" + std::to_string(meshLinks.size()));
            emit(EventKind::Connected, status.clientId, {}, message);
            emit(EventKind::Message, status.clientId, {}, message);
            return;
        }
        if (type == "peer_joined") {
            const std::string id = message.value("client_id", "");
            if (!id.empty()) {
                cloudDepartedPeers.erase(id);
                const bool alreadyKnown = peerNames.contains(id);
                if (status.mode == Mode::CloudRoom && !alreadyKnown)
                    cloudNewcomers[id] = std::chrono::steady_clock::now();
                peerNames[id] = message.value("name", id);
                if (meshEnabled) {
                    admit_mesh(id);
                    meshPuppets[id] = message.value("want_puppet", true);
                }
                if (!alreadyKnown)
                    emit(EventKind::PeerJoined, id, peerNames[id], message);
            }
            if (status.mode == Mode::CloudRoom)
                emit(EventKind::Diagnostic, id,
                     "room_peer_joined peers=" + std::to_string(meshLinks.size()));
            status.semanticVisualsReady =
                message.value("semantic_visuals_ready", false);
            status.snapshotDeltasReady =
                message.value("snapshot_deltas_ready", false);
        } else if (type == "peer_left") {
            const std::string id = message.value("client_id", "");
            if (status.mode == Mode::CloudRoom && !id.empty() &&
                cloudDepartedPeers.insert(id).second) {
                cloudDepartedOrder.push_back(id);
                if (cloudDepartedOrder.size() > 256) {
                    cloudDepartedPeers.erase(cloudDepartedOrder.front());
                    cloudDepartedOrder.pop_front();
                }
            }
            if (status.mode == Mode::CloudRoom)
                emit(EventKind::Diagnostic, id,
                     "room_peer_left peers_before=" + std::to_string(meshLinks.size()));
            if (meshLinks.contains(id)) drain_peer_gameplay(id,meshLinks.at(id),true);
            peerNames.erase(id);
            connections.mesh_remove(id);
            cloudLinkStarted.erase(id);
            cloudNewcomers.erase(id);
            meshRoutes.erase(id); meshLinks.erase(id); meshRx.erase(id);
            if (auto it = peerDelivery.find(id); it != peerDelivery.end()) {
                for (const auto& [sequence,item] : it->second.sent) deliveryBytes -= item.bytes;
                for (const auto& [sequence,item] : it->second.received) deliveryBytes -= item.bytes;
                peerDelivery.erase(it);
            }
            barrierExpected.erase(id); barrierSeen.erase(id); futureBarrierSeen.erase(id);
            if (auto it = futureGameplay.find(id); it != futureGameplay.end()) {
                for (const auto& item : it->second) deliveryBytes -= item.bytes;
                futureGameplay.erase(it);
            }
            meshPuppets.erase(id);
            forget_pose_ack_history(id);
            peerStages.erase(id);
            peerPoseStages.erase(id);
            status.semanticVisualsReady =
                message.value("semantic_visuals_ready", false);
            status.snapshotDeltasReady =
                message.value("snapshot_deltas_ready", false);
            emit(EventKind::PeerLeft, id, {}, message);
        } else if (type == "kicked") {
            // Do not let the normal established-session reconnect policy put a
            // deliberately removed player straight back into the lobby.
            automaticReconnect = false;
            status.error = "Kicked by lobby host";
            handshakeRejected = true;
            return;
        } else if (type == "ice_signal") {
            const std::string source = message.value("client_id", "");
            // Cloud rooms authenticate the sender and target before forwarding
            // ICE. If the peer-joined notification was missed, the signal is
            // enough to recover membership and reply without Worker changes.
            if (status.mode == Mode::CloudRoom && meshEnabled && !source.empty() &&
                source != status.clientId && !cloudDepartedPeers.contains(source) &&
                message.value("kind", -1) == IceAgent::Signal::Description &&
                !meshLinks.contains(source)) {
                peerNames[source] = source;
                admit_mesh(source);
                meshPuppets[source] = true;
                peerStages[source] = {"", 0};
                emit(EventKind::Diagnostic, source, "room_peer_recovered_from_ice");
                emit(EventKind::PeerJoined, source, source,
                     {{"type", "peer_joined"}, {"client_id", source}, {"name", source}});
            }
            if (meshEnabled && meshLinks.contains(source)) {
                const int kind = message.value("kind", -1);
                if (kind >= 0 && kind <= 2) {
                    const auto generation = message.value("generation", 0U);
                    const bool accepted = connections.mesh_signal(source,
                        {static_cast<IceAgent::Signal::Kind>(kind), message.value("data", ""), generation});
                    if (kind == IceAgent::Signal::Description || generation > 0 || !accepted)
                        emit(EventKind::Diagnostic, source,
                             "ice_rx kind=" + std::to_string(kind) +
                             " gen=" + std::to_string(generation) +
                             " accepted=" + (accepted ? "yes" : "no"));
                } else {
                    emit(EventKind::Diagnostic, source, "ice_rx invalid_kind");
                }
            } else if (status.mode == Mode::CloudRoom) {
                emit(EventKind::Diagnostic, source, "ice_rx unknown_peer");
            }
            return;
        } else if (type == "owner_changed") {
            status.ownerClientId = message.value("owner_client_id", "");
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            if (status.isOwner) {
                status.voiceSettingsReady = true;
                if (!send_voice_settings_to()) deliveryFailure = true;
                // Keep the lobby's last known value when ownership moves.
                // Fall back to this client's preference only if the former
                // owner never delivered the setting.
                if (!status.saveRecoverySettingsReady)
                    status.settings.saveRecovery = preferredSaveRecovery;
                status.saveRecoverySettingsReady = true;
                if (!send_save_recovery_setting_to()) deliveryFailure = true;
            } else {
                status.voiceSettingsReady = false;
                status.saveRecoverySettingsReady = false;
                if (meshLinks.contains(status.ownerClientId) &&
                    !send_mesh_message({{"type", "voice_settings_request"},
                                        {"target_client_id", status.ownerClientId}})) {
                    deliveryFailure = true;
                }
                if (meshLinks.contains(status.ownerClientId) &&
                    !send_mesh_message({{"type", "save_recovery_setting_request"},
                                        {"target_client_id", status.ownerClientId}})) {
                    deliveryFailure = true;
                }
            }
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "voice_settings_request") {
            const std::string source = message.value("client_id", "");
            if (is_room_mode(status.mode) && status.isOwner &&
                meshLinks.contains(source) && !send_voice_settings_to(source)) {
                deliveryFailure = true;
            }
        } else if (type == "voice_settings") {
            const std::string source = message.value("client_id", "");
            if (is_room_mode(status.mode) && source != status.ownerClientId) return;
            if (status.mode != Mode::DirectJoin && !is_room_mode(status.mode)) return;
            const auto enabled = message.find("enabled");
            const auto range = message.find("range_percent");
            if (enabled == message.end() || !enabled->is_boolean() ||
                range == message.end() || !range->is_number_integer()) return;
            const int64_t percent = range->get<int64_t>();
            if (percent < 0 || percent > 200) return;
            status.settings.voiceProximity = enabled->get<bool>();
            status.settings.voiceProximityRange = static_cast<int>(percent);
            status.voiceSettingsReady = true;
            emit(EventKind::Message, source, {}, message);
        } else if (type == "save_recovery_setting_request") {
            const std::string source = message.value("client_id", "");
            if (is_room_mode(status.mode) && status.isOwner &&
                meshLinks.contains(source) && !send_save_recovery_setting_to(source)) {
                deliveryFailure = true;
            }
        } else if (type == "save_recovery_setting") {
            const std::string source = message.value("client_id", "");
            if (is_room_mode(status.mode) && source != status.ownerClientId) return;
            if (status.mode != Mode::DirectJoin && !is_room_mode(status.mode)) return;
            const auto enabled = message.find("enabled");
            if (enabled == message.end() || !enabled->is_boolean()) return;
            status.settings.saveRecovery = enabled->get<bool>();
            status.saveRecoverySettingsReady = true;
            emit(EventKind::Message, source, {}, message);
        } else if (type == "room_settings") {
            status.ownerClientId = message.value("owner_client_id", status.ownerClientId);
            status.isOwner = !status.clientId.empty() && status.clientId == status.ownerClientId;
            status.settings = parse_settings(message.value("settings", json::object()),
                                             status.settings);
            status.semanticVisualsReady =
                message.value("semantic_visuals_ready", status.semanticVisualsReady);
            status.snapshotDeltasReady =
                message.value("snapshot_deltas_ready", status.snapshotDeltasReady);
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "udp_ready") {
            status.udpReady = true;
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "dummy_model") {
            status.settings.dummyModel = message.value("enabled", status.settings.dummyModel);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "sync_flags") {
            status.settings.syncFlags = message.value("enabled", status.settings.syncFlags);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "sync_world") {
            status.settings.syncWorld = message.value("enabled", status.settings.syncWorld);
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "remote_collision") {
            status.settings.remoteCollision =
                message.value("enabled", status.settings.remoteCollision);
            if (!status.settings.remoteCollision) {
                status.settings.pvp = false;
            }
            emit(EventKind::Message, {}, {}, message);
        } else if (status.mode == Mode::DirectJoin && type == "pvp_enabled") {
            status.settings.pvp = message.value("enabled", status.settings.pvp) &&
                                  status.settings.remoteCollision;
            emit(EventKind::Message, {}, {}, message);
        } else if (type == "ping") {
            if (status.mode == Mode::CloudRoom)
                send_peer_gameplay({{"type", "pong"},
                                    {"target_client_id", message.value("client_id", "")}});
            else queue_primary({{"type", "pong"}});
        } else if (type == "error") {
            const std::string reason = message.value("error", "remote error");
            if (!(status.mode == Mode::CloudRoom && status.reconnecting && reason == "room_recovering"))
                emit(EventKind::Error, {}, reason, message);
            if ((is_room_mode(status.mode) || status.mode == Mode::DirectJoin) && !status.welcomed) {
                const bool recreate = reason == "lobby_not_found" && relayMayRecreateRoom;
                relayCreateRoom = recreate;
                automaticReconnect = recreate || (reason == "room_recovering" && sessionEstablished);
                handshakeRejected = true;
                status.error = recreate ? "Room vanished; recreating it" : reason;
            }
        } else {
            if (meshEnabled && type == "puppet_preference")
                meshPuppets[message.value("client_id", "")] = message.value("want_puppet", true);
            if (type == "presence" || type == "progression_state") {
                const std::string id = message.value("client_id", "direct");
                peerStages[id] = {message.value("stage", ""), 0};
            }
            // Direct-host messages intentionally omit client_id on the wire.
            // Attribute those messages to the host instead of discarding them
            // as anonymous or self-invalid events.
            const char* fallbackSender = status.mode == Mode::DirectJoin ? "direct" : "";
            emit(EventKind::Message, message.value("client_id", fallbackSender), {}, message);
        }
    }

    template <typename Handler>
    bool receive(socket_t source, std::string& buffer, Handler&& handler, bool peerFrame = false) {
        std::array<char, 4096> bytes{};
        while (true) {
            const int count = connections.receive(source, bytes.data(), bytes.size());
            if (count > 0) {
                buffer.append(bytes.data(), static_cast<size_t>(count));
                if (buffer.size() > kReliableRxBufferMaxBytes) {
                    return false;
                }
                size_t newline = std::string::npos;
                while ((newline = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    if (line.empty()) {
                        continue;
                    }
                    try {
                        handler(decode_reliable_json(line, (peerFrame || meshEnabled) ? reliablePeerFrameLimit : reliableJsonLimit));
                    } catch (const ReliableJsonError&) {
                        emit(EventKind::Error, {}, "invalid compressed JSON");
                        return false;
                    } catch (const json::exception&) {
                        emit(EventKind::Error, {}, "invalid JSON");
                    }
                    if (handshakeRejected) {
                        return false;
                    }
                    if (eventQueueOverflow) return false;
                }
                continue;
            }
            if (count == 0) {
                return false;
            }
            return count == -1;
        }
    }

    void pump_primary() {
        if (!flush(socket, tx) ||
            !receive(socket, rx, [this](const json& message) { receive_primary(message); }) ||
            !flush(socket, tx)) {
            const bool overflow = eventQueueOverflow;
            const bool rejected = handshakeRejected;
            handshakeRejected = false;
            std::string reason = "remote closed";
            if (overflow) {
                reason = "transport event queue limit reached";
            } else if (rejected) {
                reason = status.error.empty() ? "relay handshake rejected" : status.error;
            }
            fail(reason,
                 overflow ? true : (rejected ? automaticReconnect : true),
                 !overflow && !rejected);
        }
    }

    void pump_cloud() {
        if (!cloudChannel) { fail("Cloud room channel unavailable", false); return; }
        RoomChannel::Event event{};
        for (size_t n = 0; n < 128 && cloudChannel->poll(event); ++n) {
            if (event.kind == RoomChannel::EventKind::Open) {
                status.state = State::Connected;
                status.error.clear();
                challengeStartedAt = std::chrono::steady_clock::now();
                emit(EventKind::Diagnostic, {}, "room_websocket_open");
                // The room sends a fresh challenge after opening.
            } else if (event.kind == RoomChannel::EventKind::Message) {
                if (event.text.size() > CloudCipher::maxWire) {
                    fail("Cloud room message exceeds limit", false); return;
                }
                try {
                    if (!cloudCipher || !cloudCipher->established()) {
                        const auto hello = json::parse(event.text);
                        if (!cloudCipher || !hello.is_object() ||
                            hello.value("type", "") != "secure_hello" ||
                            hello.value("version", 0) != 1 ||
                            !cloudCipher->exchange(hello.value("public_key", ""))) {
                            fail("Cloud room service needs the encryption update", false); return;
                        }
                        const json ready = {{"type", "secure_ready"}, {"version", 1},
                                            {"public_key", cloudCipher->public_key()}};
                        if (!cloudChannel->send(ready.dump())) {
                            fail("Could not establish cloud room encryption", false); return;
                        }
                        continue;
                    }
                    const auto plaintext = cloudCipher->open(event.text);
                    if (!plaintext) {
                        fail("Invalid encrypted cloud room message", false); return;
                    }
                    const auto message = json::parse(*plaintext);
                    if (!message.is_object() || !message.contains("type") ||
                        !message["type"].is_string()) {
                        fail("Invalid cloud room message", false); return;
                    }
                    receive_primary(message);
                } catch (const json::exception&) {
                    fail("Invalid cloud room JSON", false); return;
                }
                if (handshakeRejected || deliveryFailure) {
                    const bool rejected = handshakeRejected;
                    fail(rejected ? status.error : "Invalid cloud room protocol message",
                         rejected ? automaticReconnect : true, !rejected);
                    return;
                }
            } else if (event.kind == RoomChannel::EventKind::Closed) {
                emit(EventKind::Diagnostic, {},
                     "room_websocket_closed code=" + std::to_string(event.closeCode) +
                     " error=" + std::to_string(event.closeError) +
                     " reason=" + event.text);
                if (!recover_cloud_socket(event))
                    fail(event.text.empty() ? "Cloud room connection closed" : event.text);
                return;
            } else if (event.kind == RoomChannel::EventKind::Diagnostic) {
                emit(EventKind::Diagnostic, {}, event.text);
            }
            if (eventQueueOverflow) { fail("transport event queue limit reached"); return; }
        }
    }

    void pump_direct_peers() {
        std::vector<std::pair<std::string, std::string>> failed;
        for (auto& [id, peer] : directPeers) {
            if (!peer.welcomed && !peer.kickPending &&
                std::chrono::steady_clock::now() - peer.acceptedAt > std::chrono::seconds(10)) {
                failed.emplace_back(id, "direct invite authentication timed out");
                continue;
            }
            if (peer.kickPending) {
                if (!flush(peer.socket, peer.tx)) {
                    failed.emplace_back(id, "invalid direct invite code");
                    continue;
                }
                // UdpConnection::drained means the reliable kick notice was
                // acknowledged. Old clients that do not understand the notice
                // are still removed after acknowledging it.
                if (!connections.alive(peer.socket) || connections.drained(peer.socket)) {
                    failed.emplace_back(id, "invalid direct invite code");
                }
                continue;
            }
            if (!flush(peer.socket, peer.tx) ||
                !receive(peer.socket, peer.rx,
                         [this, &peer](const json& message) {
                             handle_direct_message(message, peer);
                         }) ||
                !flush(peer.socket, peer.tx)) {
                failed.emplace_back(id, "remote closed");
            }
        }
        for (const auto& [id, reason] : failed) {
            remove_peer(id, reason);
        }
    }

    void tick() {
        if (!status.enabled) {
            return;
        }
        connections.poll();
        if (!pump_cloud_recovery()) return;
        if (status.state == State::Connected && status.mode != Mode::DirectHost &&
            !helloSent && challengeStartedAt != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - challengeStartedAt > std::chrono::seconds(12)) {
            fail("Authentication challenge timed out; host or room service needs updating", false);
            return;
        }
        for (auto it = peerStages.begin(); it != peerStages.end();) {
            if (++it->second.ageTicks > 180) {
                it = peerStages.erase(it);
            } else {
                ++it;
            }
        }
        if (status.state == State::Disconnected && automaticReconnect) {
            if ((reconnectTicks++ % 30) == 0) {
                if (status.mode == Mode::DirectHost) {
                    begin_host();
                } else if (status.mode == Mode::CloudRoom) {
                    begin_cloud();
                } else {
                    begin_connect();
                }
            }
            return;
        }
        if (status.state == State::Connecting) {
            if (status.mode == Mode::CloudRoom) pump_cloud();
            else update_connecting();
            return;
        }
        if (status.mode == Mode::DirectHost &&
            (status.state == State::Listening || status.state == State::Connected)) {
            accept_peers();
            if (status.state == State::Disconnected) return;
            pump_direct_peers();
            if (eventQueueOverflow) {
                fail("transport event queue limit reached");
                return;
            }
            pump_udp();
            if (eventQueueOverflow) fail("transport event queue limit reached");
            return;
        }
        if (status.state == State::Connected) {
            if (status.mode == Mode::CloudRoom) pump_cloud();
            else { pump_primary(); }
            if (status.state == State::Disconnected) return;
            if (meshEnabled) {
#if defined(DUSKLIGHT_TRANSPORT_TESTING)
                if (std::getenv("DUSKLIGHT_TEST_RESTART_ICE") && !testRestarted) {
                    for (const auto& [id,link] : meshLinks) connections.mesh_retry(id);
                    testRestarted = true;
                }
#endif
                try { pump_deliveries(); }
                catch (const json::exception&) { deliveryFailure = true; }
                if (deliveryFailure) { fail("reliable peer delivery failed or exceeded its bounded queue/deadline"); return; }
                for (auto& [id, direct] : meshRoutes) {
                    const bool current = delivery_direct(id);
                    if (current != direct) {
                        if (status.mode == Mode::CloudRoom)
                            emit(EventKind::Diagnostic, id,
                                 std::string(current ? "ice_route_restored " : "ice_route_lost ") +
                                 connections.mesh_diagnostics(id));
                        direct = current;
                        if (!resend_delivery(id,direct)) deliveryFailure = true;
                        emit(EventKind::RouteChanged, id, direct ? "direct" :
                            (status.mode == Mode::CloudRoom ? "connecting" : "relay"));
                    }
                    if (status.mode == Mode::CloudRoom) {
                        if (current) cloudLinkStarted.erase(id);
                        else {
                            auto [it, inserted] = cloudLinkStarted.try_emplace(
                                id, CloudLinkAttempt{std::chrono::steady_clock::now()});
                            auto& attempt = it->second;
                            const auto elapsed = std::chrono::steady_clock::now() - attempt.started;
                            const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                            if (elapsedSeconds >= attempt.nextLogSecond) {
                                emit(EventKind::Diagnostic, id,
                                     "ice_progress elapsed_s=" + std::to_string(elapsedSeconds) +
                                     " " + connections.mesh_diagnostics(id));
                                attempt.nextLogSecond += 4;
                            }
                            if (!attempt.retried && elapsed >= std::chrono::seconds(6)) {
                                attempt.retried = true;
                                if (connections.mesh_retry(id))
                                    emit(EventKind::Diagnostic, id, "ice_retry " + connections.mesh_diagnostics(id));
                            }
                            if (elapsed >= std::chrono::seconds(15)) {
                                const auto newcomer = cloudNewcomers.find(id);
                                if (newcomer != cloudNewcomers.end() &&
                                    std::chrono::steady_clock::now() - newcomer->second <
                                        std::chrono::seconds(60)) {
                                    if (elapsedSeconds >= attempt.nextEvictionSecond) {
                                        if (status.isOwner) {
                                            const bool requested = queue_primary({{"type", "kick"},
                                                {"target_client_id", id}});
                                            emit(EventKind::Diagnostic, id,
                                                 "unreachable_newcomer_removal_requested=" +
                                                 std::string(requested ? "yes " : "no ") +
                                                 connections.mesh_diagnostics(id));
                                        } else if (meshLinks.contains(status.ownerClientId) &&
                                                   delivery_direct(status.ownerClientId)) {
                                            // An incumbent's report cannot authorize a
                                            // kick; wait for the owner/newcomer to resolve
                                            // this link instead of evicting a healthy peer.
                                            emit(EventKind::Diagnostic, id,
                                                 "waiting_for_newcomer_route " +
                                                 connections.mesh_diagnostics(id));
                                        }
                                        attempt.nextEvictionSecond =
                                            static_cast<uint32_t>(elapsedSeconds + 5);
                                    }
                                    continue;
                                }
                                emit(EventKind::Diagnostic, id,
                                     "ice_timeout elapsed_s=" + std::to_string(elapsedSeconds) +
                                     " " + connections.mesh_diagnostics(id));
                                fail("Direct peer connection timed out after 15 seconds.", false);
                                return;
                            }
                        }
                    }
                }
                std::string target;
                IceAgent::Signal signal;
                bool serviceSignals = true;
#if defined(DUSKLIGHT_TRANSPORT_TESTING)
                // Compiled only into the standalone harness, never the mod.
                serviceSignals = std::getenv("DUSKLIGHT_TEST_RELAY_ONLY") == nullptr;
#endif
                for (size_t n = 0; serviceSignals && !cloudRecovering && n < 128 && connections.mesh_pop_signal(target, signal); ++n) {
                    if (!queue_primary({{"type", "ice_signal"}, {"target_client_id", target},
                        {"kind", static_cast<int>(signal.kind)}, {"data", signal.text}, {"generation", signal.generation}})) {
                        fail("ICE signaling queue full"); return;
                    }
                    if (status.mode == Mode::CloudRoom && signal.kind == IceAgent::Signal::Description)
                        emit(EventKind::Diagnostic, target,
                             "ice_tx kind=0 gen=" + std::to_string(signal.generation));
                }
            }
            pump_udp();
            if (eventQueueOverflow) {
                fail("transport event queue limit reached");
                return;
            }
            if (status.mode == Mode::Relay && status.welcomed &&
                (++relayUdpRegisterTicks % 30) == 0) {
                send_relay_udp_registration();
            }
        }
    }
};

Transport::Transport() : impl_(std::make_unique<Impl>()) {}
Transport::~Transport() = default;

bool Transport::start_direct_host(const DirectHostConfig& config, std::string* error) {
    if (config.port == 0 || config.bindHost.empty() || config.publicHost.empty()) {
        if (error != nullptr) {
            *error = "Direct host requires a valid port, bind host, and public host";
        }
        return false;
    }
    if (config.sessionKey.empty()) {
        if (error) *error = "Direct host requires an invite session key";
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::DirectHost;
    impl_->status.name = config.name.empty() ? "Host" : config.name;
    impl_->status.room = config.room.empty() ? "Lobby" : config.room;
    impl_->status.bindHost = config.bindHost;
    impl_->status.publicHost = config.publicHost;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->preferredSaveRecovery = config.settings.saveRecovery;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->status.voiceSettingsReady = true;
    impl_->status.saveRecoverySettingsReady = true;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->supportsSnapshotDeltas = config.supportsSnapshotDeltas;
    impl_->automaticReconnect = true;
    if (!impl_->begin_host()) {
        impl_->status.enabled = false;
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

bool Transport::start_direct_join(const DirectJoinConfig& config, std::string* error) {
    if (config.port == 0 || config.host.empty()) {
        if (error != nullptr) {
            *error = "Direct join requires a valid host and port";
        }
        return false;
    }
    if (config.sessionKey.empty()) {
        if (error) *error = "Direct invite is missing its session key";
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::DirectJoin;
    impl_->status.name = config.name.empty() ? "Joiner" : config.name;
    impl_->status.room = config.room.empty() ? "Lobby" : config.room;
    impl_->status.host = config.host;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->preferredSaveRecovery = config.settings.saveRecovery;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->supportsSnapshotDeltas = config.supportsSnapshotDeltas;
    impl_->automaticReconnect = true;
    if (!impl_->begin_connect() && impl_->status.state == State::Disconnected) {
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

bool Transport::start_relay(const RelayConfig& config, std::string* error) {
    if (config.port == 0 || config.host.empty() || config.room.empty()) {
        if (error != nullptr) {
            *error = "Relay connection requires a valid host, port, and lobby name";
        }
        return false;
    }
    if (config.lobbyCode.size() < 6 || config.lobbyCode.size() > 128) {
        if (error != nullptr) {
            *error = "Lobby code must be between 6 and 128 characters";
        }
        return false;
    }
    if (config.sessionKey.size() < 16) {
        if (error) *error = "Manual relay code is outdated; restart the updated relay";
        return false;
    }
    if (!acquire_network_stack(impl_->networkStackOwned, error)) {
        return false;
    }
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::Relay;
    impl_->status.name = config.name.empty() ? (config.createRoom ? "Host" : "Player") : config.name;
    impl_->status.room = config.room;
    impl_->status.host = config.host;
    impl_->status.port = config.port;
    impl_->status.settings = config.settings;
    impl_->preferredSaveRecovery = config.settings.saveRecovery;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->lobbyCode = config.lobbyCode;
    impl_->sessionId = config.sessionId;
    impl_->sessionKey = config.sessionKey;
    impl_->relayCreateRoom = config.createRoom;
    impl_->relayMayRecreateRoom = false;
    impl_->wantPuppet = config.wantPuppet;
    impl_->wantMidna = false;
    impl_->supportsSnapshotDeltas = config.supportsSnapshotDeltas;
    impl_->automaticReconnect = true;
    if (!impl_->begin_connect() && impl_->status.state == State::Disconnected) {
        if (error != nullptr) {
            *error = impl_->status.error;
        }
        return false;
    }
    return true;
}

bool Transport::start_cloud_room(const CloudRoomConfig& config,
                                 std::unique_ptr<RoomChannel> channel,
                                 std::string* error) {
    const auto reject = [error](const char* reason) {
        if (error) *error = reason;
        return false;
    };
    if (!channel) return reject("Cloud room channel unavailable");
    if (!valid_room_name(config.room))
        return reject("Lobby name must be 1-64 letters, numbers, spaces, _ or - with no outer spaces");
    if (config.lobbyCode.size() < 6 || config.lobbyCode.size() > 128)
        return reject("Lobby code must be between 6 and 128 characters");
    std::string server = config.serverUrl;
    while (server.ends_with('/')) server.pop_back();
    if (!(server.starts_with("https://") || server.starts_with("wss://")))
        return reject("Room service URL must begin with https:// or wss://");
    const auto authority = server.substr(server.find("://") + 3);
    if (authority.empty() || authority.find_first_of("/?#@\\") != std::string::npos)
        return reject("Room service URL must contain only a secure hostname");
    if (!acquire_network_stack(impl_->networkStackOwned, error)) return false;
    impl_->reset_runtime(false);
    impl_->status.enabled = true;
    impl_->status.mode = Mode::CloudRoom;
    impl_->status.name = config.name.empty() ? (config.createRoom ? "Host" : "Player") : config.name;
    impl_->status.room = config.room;
    impl_->status.host = server;
    impl_->status.settings = config.settings;
    impl_->preferredSaveRecovery = config.settings.saveRecovery;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    impl_->lobbyCode = config.lobbyCode;
    impl_->relayCreateRoom = config.createRoom;
    impl_->wantPuppet = config.wantPuppet;
    impl_->supportsSnapshotDeltas = config.supportsSnapshotDeltas;
    impl_->cloudUrl = cloud_room_url(server, config.room);
    impl_->cloudStunHost = config.stunHost;
    impl_->cloudStunPort = config.stunPort;
    impl_->cloudChannel = std::move(channel);
    impl_->automaticReconnect = true;
    if (!impl_->begin_cloud() && impl_->status.state == State::Disconnected) {
        if (error) *error = impl_->status.error;
        return false;
    }
    return true;
}

void Transport::tick() {
    impl_->tick();
}

bool Transport::send(const nlohmann::json& message) {
    if (!impl_->status.enabled || !message.is_object()) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->broadcast(message);
    }
    const auto type = message.value("type", "");
    if (impl_->cloudRecovering && type == "kick") {
        impl_->emit(EventKind::Error, {}, "Lobby connection is recovering; retry removing the player shortly");
        return false;
    }
    const bool cloudPeerControl = impl_->status.mode == Mode::CloudRoom &&
        (type == "ping" || type == "pong" || type == "presence" ||
         type == "puppet_preference");
    const bool peerGameplay = impl_->meshEnabled &&
        (peer_delivery_type(type) || cloudPeerControl || type == "room_settings");
    if (impl_->status.state != State::Connected ||
        !(peerGameplay ? impl_->send_mesh_message(message) : impl_->queue_primary(message))) {
        impl_->fail("send failed");
        return false;
    }
    return true;
}

bool Transport::send_to(const std::string& peerId, const nlohmann::json& message) {
    if (!impl_->status.enabled || peerId.empty() || !message.is_object()) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        auto peer = impl_->directPeers.find(peerId);
        return peer != impl_->directPeers.end() && peer->second.welcomed &&
               !peer->second.kickPending &&
               impl_->queue_peer(peer->second, message);
    }
    nlohmann::json targeted = message;
    targeted["target_client_id"] = peerId;
    const auto type = targeted.value("type", "");
    if (impl_->cloudRecovering && type == "kick") {
        impl_->emit(EventKind::Error, {}, "Lobby connection is recovering; retry removing the player shortly");
        return false;
    }
    const bool cloudPeerControl = impl_->status.mode == Mode::CloudRoom &&
        (type == "ping" || type == "pong" || type == "presence" ||
         type == "puppet_preference");
    if (impl_->status.state != State::Connected ||
        !(impl_->meshEnabled && (peer_delivery_type(type) || cloudPeerControl) ?
            impl_->send_mesh_message(targeted) : impl_->queue_primary(targeted))) {
        impl_->fail("targeted send failed");
        return false;
    }
    return true;
}

#if defined(DUSKLIGHT_CLOUD_SECURITY_TESTING)
bool Transport::send_test_peer_message(const std::string& peerId,
                                       const nlohmann::json& message) {
    if (impl_->status.mode != Mode::CloudRoom || !impl_->status.welcomed ||
        !message.is_object() || !impl_->meshLinks.contains(peerId)) return false;
    nlohmann::json targeted = message;
    targeted["target_client_id"] = peerId;
    return impl_->send_peer_gameplay(targeted);
}
#endif

#if defined(DUSKLIGHT_DIRECT_SECURITY_TESTING)
bool Transport::send_test_visual_as(const nlohmann::json& message,
                                    const std::string& claimedSender) {
    if (impl_->status.mode != Mode::DirectJoin || !impl_->status.welcomed ||
        !message.is_object() || !impl_->udpRemoteAddressKnown) return false;
    const auto datagrams = udp::encode_message(message, claimedSender,
                                                udp::PacketType::PoseMsgpack);
    if (datagrams.empty()) return false;
    for (const auto& datagram : datagrams) {
        if (!impl_->send_udp_datagram_now(impl_->udpRemoteAddress, datagram)) return false;
    }
    return true;
}
#endif

bool Transport::send_visual(const nlohmann::json& message, udp::PacketType type) {
    if (!impl_->status.enabled || !impl_->status.welcomed || !message.is_object() ||
        type == udp::PacketType::MidnaMsgpack ||
        (type != udp::PacketType::PoseJson && type != udp::PacketType::PoseMsgpack &&
         type != udp::PacketType::SemanticPoseMsgpack &&
         type != udp::PacketType::MidnaMsgpack)) {
        return false;
    }
    impl_->lastVisualSend = {};
    impl_->lastVisualSend.type = type;
    impl_->lastVisualSend.sequence = message.value("sequence", 0U);
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->send_visual_to_direct_peers(message, impl_->local_udp_sender_id(), type);
    }
    if (!impl_->meshEnabled && !impl_->udpRemoteAddressKnown) {
        return false;
    }
    nlohmann::json wireMessage = message;
    const std::string senderId = impl_->local_udp_sender_id();
    const std::string receiverId = impl_->status.mode == Mode::DirectJoin ? "direct" : "";
    const uint32_t sequence = wireMessage.value("sequence", 0U);
    const uint32_t baseline = is_room_mode(impl_->status.mode) ?
        impl_->relay_common_ack_sequence(senderId, type) :
        impl_->pose_ack_sequence(receiverId, senderId, type);
    const bool allowSnapshotDelta = impl_->status.snapshotDeltasReady;
    std::string error;
    uint64_t fullSnapshotPreparedBytes = 0;
    uint64_t fullSnapshotWireBytes = 0;
    if (impl_->visualWireDiagnostics &&
        type == udp::PacketType::SemanticPoseMsgpack) {
        nlohmann::json fullSnapshotMessage = message;
        std::string diagnosticError;
        if (impl_->poseDeltaPrepare == nullptr ||
            impl_->poseDeltaPrepare(fullSnapshotMessage, senderId,
                                 static_cast<uint8_t>(type), sequence, baseline,
                                 false, false, diagnosticError)) {
            fullSnapshotMessage.erase("_snapshot_debug_reason");
            fullSnapshotPreparedBytes =
                nlohmann::json::to_msgpack(fullSnapshotMessage).size();
            const auto fullSnapshotDatagrams = udp::encode_message(
                fullSnapshotMessage, senderId, type, &diagnosticError);
            for (const auto& datagram : fullSnapshotDatagrams) {
                fullSnapshotWireBytes += datagram.bytes.size();
            }
        }
    }
    if (impl_->poseDeltaPrepare != nullptr &&
        !impl_->poseDeltaPrepare(wireMessage, senderId, static_cast<uint8_t>(type), sequence,
                              baseline, allowSnapshotDelta, impl_->visualWireDiagnostics,
                              error)) {
        impl_->emit(EventKind::Error, {}, error);
        return false;
    }
    impl_->lastVisualSend.snapshotDecision =
        wireMessage.value("_snapshot_debug_reason", "none");
    wireMessage.erase("_snapshot_debug_reason");
    impl_->lastVisualSend.snapshotChangedKeys = take_snapshot_debug_keys(
        wireMessage, "_snapshot_debug_changed_keys");
    impl_->lastVisualSend.snapshotUnchangedKeys = take_snapshot_debug_keys(
        wireMessage, "_snapshot_debug_unchanged_keys");
    impl_->lastVisualSend.snapshotRemovedKeys = take_snapshot_debug_keys(
        wireMessage, "_snapshot_debug_removed_keys");
    impl_->lastVisualSend.fullMsgpackBytes = nlohmann::json::to_msgpack(message).size();
    impl_->lastVisualSend.preparedMsgpackBytes =
        nlohmann::json::to_msgpack(wireMessage).size();
    impl_->lastVisualSend.snapshotBaseline = baseline;
    if (wireMessage.value("snapshot_delta_v1", false)) {
        impl_->lastVisualSend.snapshotDeltas = 1;
    } else {
        impl_->lastVisualSend.snapshotFulls = 1;
    }
    const auto datagrams =
        udp::encode_message(wireMessage, senderId, type, &error);
    if (datagrams.empty()) {
        impl_->emit(EventKind::Error, {}, error);
        return false;
    }
    if (impl_->meshEnabled) {
        bool accepted = true;
        std::vector<std::string> recipients;
        for (const auto& [id, name] : impl_->peerNames) {
            if (impl_->meshPuppets.contains(id) && !impl_->meshPuppets.at(id)) continue;
            if (!impl_->stages_match(message, id, senderId)) continue;
            recipients.push_back(id);
            ++impl_->lastVisualSend.recipients;
        }
        for (const auto& datagram : datagrams) {
            accepted = impl_->connections.mesh_send_many(recipients, datagram.bytes,
                &impl_->lastVisualSend.datagrams, &impl_->lastVisualSend.wireBytes) && accepted;
        }
        return accepted;
    }
    const bool queued = impl_->enqueue_udp_tx_datagrams(impl_->udpRemoteAddress, datagrams,
                                                        senderId, receiverId, type);
    if (queued) {
        impl_->lastVisualSend.recipients = 1;
        impl_->lastVisualSend.fullSnapshotPreparedMsgpackBytes =
            fullSnapshotPreparedBytes;
        impl_->lastVisualSend.fullSnapshotWireBytes = fullSnapshotWireBytes;
        impl_->lastVisualSend.datagrams = static_cast<uint32_t>(datagrams.size());
        for (const auto& datagram : datagrams) {
            impl_->lastVisualSend.wireBytes += datagram.bytes.size();
        }
    }
    return queued;
}

bool Transport::send_remote_object(const udp::RemoteObjectPacket& object) {
    if (!impl_->status.enabled || !impl_->status.welcomed ||
        !impl_->status.settings.syncWorld) {
        return false;
    }
    if (impl_->status.mode == Mode::DirectHost) {
        return impl_->send_object_to_direct_peers(object, impl_->local_udp_sender_id());
    }
    return (impl_->meshEnabled || impl_->udpRemoteAddressKnown) &&
           impl_->send_udp_datagram(
               impl_->udpRemoteAddress,
               udp::encode_remote_object(impl_->local_udp_sender_id(), object));
}

bool Transport::send_voice(uint32_t sequence, const udp::VoicePosition& position,
                           std::span<const uint8_t> opus) {
    if (!impl_->status.enabled || !impl_->status.welcomed ||
        !impl_->status.voiceSettingsReady || opus.empty() ||
        opus.size() > 400) return false;
    const std::string senderId = impl_->local_udp_sender_id();
    if (impl_->status.mode == Mode::DirectHost)
        return impl_->send_voice_to_direct_peers(opus, sequence, position, senderId);
    if (!impl_->meshEnabled && !impl_->udpRemoteAddressKnown) return false;
    const auto datagram = udp::encode_voice(senderId, sequence, position, opus);
    return !datagram.bytes.empty() &&
        impl_->send_udp_datagram(impl_->udpRemoteAddress, datagram);
}

void Transport::disconnect() {
    if (!impl_->status.enabled) {
        return;
    }
    impl_->reset_runtime(false);
    impl_->automaticReconnect = false;
    impl_->emit(EventKind::Disconnected, {}, "user requested");
}

Status Transport::status() const {
    Status result = impl_->status;
    for (const auto& [id, direct] : impl_->meshRoutes) {
        if (direct) ++result.natPeerCount;
        else if (result.mode == Mode::Relay) ++result.relayPeerCount;
    }
    return result;
}

VisualSendStats Transport::last_visual_send_stats() const {
    return impl_->lastVisualSend;
}

const std::map<std::string, std::string>& Transport::peers() const {
    return impl_->peerNames;
}

bool Transport::reliable_peer_ready(std::string_view peerId) const {
    if (impl_->status.mode == Mode::DirectHost) {
        const auto peer = impl_->directPeers.find(std::string(peerId));
        return peer != impl_->directPeers.end() && peer->second.welcomed &&
               !peer->second.kickPending;
    }
    if (impl_->status.mode == Mode::DirectJoin)
        return peerId == "direct" && impl_->status.welcomed;
    if (!is_room_mode(impl_->status.mode) || !impl_->status.welcomed ||
        !impl_->meshLinks.contains(std::string(peerId))) return false;
    // Cloud rooms have no relay fallback for peer gameplay. Wait until the
    // reliable direct route is actually established before a group action.
    return impl_->status.mode != Mode::CloudRoom ||
           impl_->connections.mesh_direct(std::string(peerId));
}

std::optional<uint32_t> Transport::direct_peer_rtt_ms(std::string_view peerId) const {
    if (!impl_->meshEnabled) return std::nullopt;
    return impl_->connections.mesh_direct_rtt_ms(peerId);
}

bool Transport::has_events() const {
    return !impl_->events.empty();
}

Event Transport::pop_event() {
    if (impl_->events.empty()) {
        return {};
    }
    Event event = std::move(impl_->events.front());
    impl_->events.pop_front();
    return event;
}

bool Transport::publish_room_settings(const RoomSettings& settings) {
    if (is_room_mode(impl_->status.mode) && !impl_->status.isOwner) {
        return false;
    }
    if (!is_room_mode(impl_->status.mode) && impl_->status.mode != Mode::DirectHost) {
        return false;
    }
    if (is_room_mode(impl_->status.mode)) {
        auto requested = settings; requested.pvp &= requested.remoteCollision;
        return send({{"type","room_settings"},{"settings",settings_json(requested)}});
    }
    const RoomSettings previous = impl_->status.settings;
    impl_->status.settings = settings;
    impl_->status.settings.pvp &= impl_->status.settings.remoteCollision;
    bool ok = true;
    if (previous.dummyModel != impl_->status.settings.dummyModel) {
        ok &= send({{"type", "dummy_model"},
                    {"enabled", impl_->status.settings.dummyModel}});
    }
    if (previous.syncFlags != impl_->status.settings.syncFlags) {
        ok &= send({{"type", "sync_flags"},
                    {"enabled", impl_->status.settings.syncFlags}});
    }
    if (previous.syncWorld != impl_->status.settings.syncWorld) {
        ok &= send({{"type", "sync_world"},
                    {"enabled", impl_->status.settings.syncWorld}});
    }
    const bool collisionChanged =
        previous.remoteCollision != impl_->status.settings.remoteCollision;
    if (collisionChanged) {
        ok &= send({{"type", "remote_collision"},
                    {"enabled", impl_->status.settings.remoteCollision}});
    }
    if (collisionChanged || previous.pvp != impl_->status.settings.pvp) {
        ok &= send({{"type", "pvp_enabled"},
                    {"enabled", effective_pvp(impl_->status.settings)}});
    }
    return ok;
}

bool Transport::publish_voice_settings(bool proximity, int rangePercent) {
    if (!impl_->status.enabled ||
        (impl_->status.mode != Mode::DirectHost &&
         (!is_room_mode(impl_->status.mode) || !impl_->status.isOwner))) return false;
    const bool changed = impl_->status.settings.voiceProximity != proximity ||
        impl_->status.settings.voiceProximityRange != std::clamp(rangePercent, 0, 200);
    impl_->status.settings.voiceProximity = proximity;
    impl_->status.settings.voiceProximityRange = std::clamp(rangePercent, 0, 200);
    impl_->status.voiceSettingsReady = true;
    if (!changed) return true;
    if (impl_->status.mode == Mode::DirectHost) {
        return send({{"type", "voice_settings"}, {"enabled", proximity},
                     {"range_percent", impl_->status.settings.voiceProximityRange}});
    }
    return impl_->send_voice_settings_to();
}

bool Transport::publish_save_recovery_setting(bool enabled) {
    if (!impl_->status.enabled ||
        (impl_->status.mode != Mode::DirectHost &&
         (!is_room_mode(impl_->status.mode) || !impl_->status.isOwner))) return false;
    const bool changed = impl_->status.settings.saveRecovery != enabled;
    impl_->status.settings.saveRecovery = enabled;
    impl_->preferredSaveRecovery = enabled;
    impl_->status.saveRecoverySettingsReady = true;
    if (!changed) return true;
    if (impl_->status.mode == Mode::DirectHost)
        return send({{"type", "save_recovery_setting"}, {"enabled", enabled}});
    return impl_->send_save_recovery_setting_to();
}

bool Transport::publish_visual_preferences(bool wantPuppet, bool wantMidna) {
    (void)wantMidna;
    impl_->wantPuppet = wantPuppet;
    impl_->wantMidna = false;
    return send({{"type", "puppet_preference"},
                 {"want_puppet", wantPuppet},
                 {"want_midna", false}});
}

bool Transport::kick_peer(const std::string& peerId, std::string* error) {
    const auto reject = [error](const char* reason) {
        if (error != nullptr) *error = reason;
        return false;
    };
    if (peerId.empty()) return reject("Choose a connected player");

    if (impl_->status.mode == Mode::DirectHost) {
        auto peer = impl_->directPeers.find(peerId);
        if (peer == impl_->directPeers.end() || !peer->second.welcomed) {
            return reject("Player is no longer connected");
        }
        if (peer->second.kickPending) return reject("Player removal is already pending");
        if (!impl_->queue_peer(peer->second,
                              {{"type", "kicked"}, {"reason", "removed_by_host"}})) {
            return reject("Could not notify the player");
        }
        peer->second.kickPending = true;
        return true;
    }

    if (!is_room_mode(impl_->status.mode) || !impl_->status.welcomed ||
        !impl_->status.isOwner) {
        return reject("Only the lobby host can kick players");
    }
    if (peerId == impl_->status.clientId || !impl_->peerNames.contains(peerId)) {
        return reject("Player is no longer connected");
    }
    if (!send({{"type", "kick"}, {"target_client_id", peerId}})) {
        return reject("Could not send the kick request");
    }
    return true;
}

void Transport::set_pose_delta_codec(PoseDeltaExpandCallback expand,
                                     PoseDeltaPrepareCallback prepare) {
    impl_->poseDeltaExpand = expand;
    impl_->poseDeltaPrepare = prepare;
}

void Transport::set_visual_wire_diagnostics(bool enabled) {
    impl_->visualWireDiagnostics = enabled;
}

}  // namespace dusklight_online::net
