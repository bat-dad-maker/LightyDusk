#pragma once

#include "dusklight_online/net/udp_codec.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace dusklight_online::net {

enum class Mode : uint8_t {
    Disabled,
    DirectHost,
    DirectJoin,
    Relay,
    CloudRoom,
};

[[nodiscard]] inline bool is_room_mode(Mode mode) {
    return mode == Mode::Relay || mode == Mode::CloudRoom;
}

enum class State : uint8_t {
    Disconnected,
    Listening,
    Connecting,
    Connected,
};

struct RoomSettings {
    bool dummyModel = true;
    bool syncFlags = true;
    bool saveRecovery = true;
    bool syncWorld = false;
    bool remoteCollision = true;
    bool pvp = true;
    bool voiceProximity = true;
    int voiceProximityRange = 50;
};

[[nodiscard]] inline bool effective_remote_collision(const RoomSettings& settings) {
    return settings.dummyModel && settings.remoteCollision;
}

[[nodiscard]] inline bool effective_pvp(const RoomSettings& settings) {
    return effective_remote_collision(settings) && settings.pvp;
}

struct DirectHostConfig {
    std::string name = "Host";
    std::string room = "Lobby";
    std::string bindHost = "0.0.0.0";
    std::string publicHost = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
    bool supportsSnapshotDeltas = true;
};

struct DirectJoinConfig {
    std::string name = "Joiner";
    std::string room = "Lobby";
    std::string host = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
    bool supportsSnapshotDeltas = true;
};

struct RelayConfig {
    std::string name = "Player";
    std::string room = "Lobby";
    std::string lobbyCode;
    std::string host = "127.0.0.1";
    std::string sessionId;
    std::string sessionKey;
    uint16_t port = 34197;
    bool createRoom = false;
    RoomSettings settings;
    bool wantPuppet = true;
    bool wantMidna = false;
    bool supportsSnapshotDeltas = true;
};

struct CloudRoomConfig {
    std::string name = "Player";
    std::string room = "Lobby";
    std::string lobbyCode;
    std::string serverUrl;
    std::string stunHost = "stun.cloudflare.com";
    uint16_t stunPort = 3478;
    bool createRoom = false;
    RoomSettings settings;
    bool wantPuppet = true;
    bool supportsSnapshotDeltas = true;
};

// The mod adapts Dusklight's WebSocket service here; standalone transport
// tests supply their own room channel. This interface keeps room signaling
// separate from peer gameplay traffic.
class RoomChannel {
public:
    enum class EventKind { Open, Message, Closed, Diagnostic };
    struct Event {
        EventKind kind;
        std::string text;
        uint16_t closeCode = 0;
        int closeError = 0;
    };
    virtual ~RoomChannel() = default;
    virtual bool open(std::string_view url, std::string& error) = 0;
    virtual bool send(std::string_view text) = 0;
    virtual bool poll(Event& event) = 0;
    virtual void close() = 0;
};

enum class EventKind : uint8_t {
    Connected,
    Disconnected,
    PeerJoined,
    PeerLeft,
    Message,
    UdpMessage,
    UdpRemoteObject,
    UdpVoice,
    UdpAck,
    Error,
    RouteChanged,
    Diagnostic,
};

struct EventContext {
    uint64_t epoch = 0;
    Mode mode = Mode::Disabled;
    bool welcomed = false;
    bool semanticVisualsReady = false;
    bool snapshotDeltasReady = false;
    RoomSettings settings;
    std::string clientId;
};

struct Event {
    EventKind kind = EventKind::Message;
    std::string peerId;
    std::string detail;
    nlohmann::json message;
    udp::PacketType udpType = udp::PacketType::PoseMsgpack;
    uint32_t udpSequence = 0;
    uint8_t udpStressFlags = 0;
    udp::RemoteObjectPacket remoteObject;
    std::vector<uint8_t> voice;
    udp::VoicePosition voicePosition;
    EventContext ingress;
};

struct Status {
    uint32_t natPeerCount = 0;
    uint32_t relayPeerCount = 0;
    Mode mode = Mode::Disabled;
    State state = State::Disconnected;
    bool enabled = false;
    bool reconnecting = false;
    bool welcomed = false;
    bool udpReady = false;
    bool isOwner = false;
    bool voiceSettingsReady = false;
    bool saveRecoverySettingsReady = false;
    bool semanticVisualsReady = false;
    bool snapshotDeltasReady = false;
    std::string name;
    std::string room;
    std::string host;
    std::string bindHost;
    std::string publicHost;
    std::string clientId;
    std::string ownerClientId;
    std::string udpToken;
    std::string error;
    uint16_t port = 0;
    RoomSettings settings;
};

struct VisualSendStats {
    udp::PacketType type = udp::PacketType::PoseMsgpack;
    uint32_t sequence = 0;
    uint32_t recipients = 0;
    uint32_t datagrams = 0;
    uint64_t wireBytes = 0;
    uint64_t fullMsgpackBytes = 0;
    uint64_t preparedMsgpackBytes = 0;
    uint64_t fullSnapshotPreparedMsgpackBytes = 0;
    uint64_t fullSnapshotWireBytes = 0;
    uint32_t snapshotDeltas = 0;
    uint32_t snapshotFulls = 0;
    uint32_t snapshotBaseline = 0;
    std::string snapshotDecision;
    std::vector<std::string> snapshotChangedKeys;
    std::vector<std::string> snapshotUnchangedKeys;
    std::vector<std::string> snapshotRemovedKeys;
};

// Non-blocking reliable transport for every JSON gameplay lane. tick() is
// called from the mod's update hook; it never waits for network input.
class Transport {
public:
    using PoseDeltaExpandCallback = bool (*)(nlohmann::json&, const std::string&, uint8_t,
                                             uint32_t, std::string&);
    using PoseDeltaPrepareCallback = bool (*)(nlohmann::json&, const std::string&, uint8_t,
                                              uint32_t, uint32_t, bool, bool, std::string&);
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool start_direct_host(const DirectHostConfig& config, std::string* error = nullptr);
    bool start_direct_join(const DirectJoinConfig& config, std::string* error = nullptr);
    bool start_relay(const RelayConfig& config, std::string* error = nullptr);
    bool start_cloud_room(const CloudRoomConfig& config, std::unique_ptr<RoomChannel> channel,
                          std::string* error = nullptr);

    void tick();
    bool send(const nlohmann::json& message);
    bool send_to(const std::string& peerId, const nlohmann::json& message);
#if defined(DUSKLIGHT_CLOUD_SECURITY_TESTING)
    // Simulate a modified peer sending a control frame over the authenticated mesh.
    bool send_test_peer_message(const std::string& peerId, const nlohmann::json& message);
#endif
#if defined(DUSKLIGHT_DIRECT_SECURITY_TESTING)
    // Simulate a direct guest claiming another player's visual sender ID.
    bool send_test_visual_as(const nlohmann::json& message,
                             const std::string& claimedSender);
#endif
    bool send_visual(const nlohmann::json& message,
                     udp::PacketType type = udp::PacketType::PoseMsgpack);
    bool send_remote_object(const udp::RemoteObjectPacket& object);
    bool send_voice(uint32_t sequence, const udp::VoicePosition& position,
                    std::span<const uint8_t> opus);
    void disconnect();

    [[nodiscard]] Status status() const;
    [[nodiscard]] VisualSendStats last_visual_send_stats() const;
    [[nodiscard]] const std::map<std::string, std::string>& peers() const;
    [[nodiscard]] bool reliable_peer_ready(std::string_view peerId) const;
    [[nodiscard]] std::optional<uint32_t> direct_peer_rtt_ms(std::string_view peerId) const;
    [[nodiscard]] bool has_events() const;
    Event pop_event();

    // Room owners use this after changing a host-controlled option. Direct
    // sessions emit the legacy individual setting messages; relay sessions
    // emit protocol-2 room_settings.
    bool publish_room_settings(const RoomSettings& settings);
    // Proximity settings use the reliable peer path; the room service never
    // stores or forwards them.
    bool publish_voice_settings(bool proximity, int rangePercent);
    // Sent over the reliable peer path so room services never handle this option.
    bool publish_save_recovery_setting(bool enabled);
    bool publish_visual_preferences(bool wantPuppet, bool wantMidna);
    // Removes one guest from the lobby. Direct hosts enforce this locally;
    // relay lobby owners request an owner-validated removal from the relay.
    bool kick_peer(const std::string& peerId, std::string* error = nullptr);
    void set_pose_delta_codec(PoseDeltaExpandCallback expand,
                              PoseDeltaPrepareCallback prepare);
    void set_visual_wire_diagnostics(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dusklight_online::net
