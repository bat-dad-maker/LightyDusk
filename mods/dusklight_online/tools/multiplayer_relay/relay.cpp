#include "dusklight_online/net/peer_delivery.hpp"
#include "dusklight_online/net/auth_code.hpp"
#include "dusklight_online/net/reliable_json.hpp"
#include "dusk/multiplayer/invite_code.hpp"
#include "dusklight_online/net/udp_connection.hpp"
#include "dusklight_online/net/peer_tunnel.hpp"
#include "dusklight_online/net/datagram_scheduler.hpp"
#include <thread>
#include "nlohmann/json.hpp"

#if _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mmsystem.h>
    using socket_t = SOCKET;
    static constexpr int kSendFlags = 0;
#else
    #include <arpa/inet.h>
    #include <cerrno>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr int kSendFlags =
    #if defined(__APPLE__)
        0;
    #else
        MSG_NOSIGNAL;
    #endif
    #ifndef INVALID_SOCKET
        #define INVALID_SOCKET -1
    #endif
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifndef DUSKLIGHT_RELAY_VERSION
#define DUSKLIGHT_RELAY_VERSION "dev"
#endif

namespace {

using json = nlohmann::json;
using dusklight_online::net::UdpConnection;

constexpr int kProtocolVersion = 4;
constexpr const char* kSemanticVisualCapability = "semantic_visual_v1";
constexpr const char* kSnapshotDeltaCapability = "semantic_snapshot_delta_v1";
constexpr size_t kMaxLineBytes = dusklight_online::net::reliablePeerFrameLimit;
constexpr size_t kMaxQueuedBytes = 8 * 1024 * 1024;
constexpr size_t kMaxRoomClients = 8;
constexpr size_t kMaxRoomIdBytes = 64;
constexpr size_t kMaxUsernameBytes = 32;
constexpr size_t kMaxReliableSequences = 4096;
constexpr size_t kMaxReadBytesPerTick = 256 * 1024;
constexpr double kHelloTimeoutSeconds = 10.0;
constexpr size_t kUdpSenderIdBytes = 32;
constexpr size_t kMaxUdpDatagramBytes = 2048;
constexpr uint8_t kUdpPacketTypePoseJson = 1;
constexpr uint8_t kUdpPacketTypePoseMsgpack = 2;
constexpr uint8_t kUdpPacketTypeRemoteObject = 3;
constexpr uint8_t kUdpPacketTypeMidnaMsgpack = 4;
constexpr uint8_t kUdpPacketTypePoseAck = 5;
constexpr uint8_t kUdpPacketTypeRelayRegister = 6;
constexpr uint8_t kUdpPacketTypeSemanticPoseMsgpack = 7;
constexpr uint8_t kUdpPacketTypeVoiceOpus = 8;
constexpr size_t kMaxUdpBytesPerClientSecond = 4 * 1024 * 1024;

#pragma pack(push, 1)
struct UdpRelayHeader {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t headerSize;
    uint32_t sequence;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t uncompressedSize;
    uint32_t compressedSize;
    uint16_t payloadSize;
    char senderId[kUdpSenderIdBytes];
};

struct UdpPoseAckPacket {
    uint32_t sequence;
    uint8_t ackedType;
    char ackedSenderId[kUdpSenderIdBytes];
    uint8_t stressFlags;
};
#pragma pack(pop)

static_assert(sizeof(UdpRelayHeader) == 58);

const std::set<std::string> kGameplayRouteTypes = {
    "event_bit",
    "tbox_bit",
    "switch_bit",
    "room_switch_bit",
    "item_bit",
    "dungeon_item_bit",
    "save_snapshot",
    "key_num",
    "light_drop_num",
    "light_drop_get_flag",
    "max_life_update",
    "bottle_slots",
    "bomb_bag_slot",
    "rupee_count",
    "rupee_delta",
    "poe_count",
    "malo_fundraising",
    "charlo_offering",
    "fish_record",
    "collect_smell",
    "item_get",
    "trade_item",
    "rando_item_get",
    "item_first_bit",
    "collect_crystal",
    "collect_mirror",
    "dark_clear_lv",
    "transform_lv",
    "region_bit",
    "collect",
    "visited_room",
    "letter_get",
    "presence",
    "progression_state",
    "puppet_preference",
    "midna_preference",
    "midna_pose",
    "pvp_hit",
    "coop_presence",
    "coop_settings",
    "coop_enemy_damage",
    "ganondorf_owner_claim",
    "ganondorf_owner",
    "ganondorf_hit",
    "ganondorf_reaction",
    "ganondorf_player_damage",
    "ganondorf_state",
    "ooccoo_state",
    "chat",
};

using SteadyClock = std::chrono::steady_clock;

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
            std::strcmp(value, "ON") == 0);
}

bool relay_packet_trace_enabled() {
    static const bool enabled = env_enabled("DUSK_MP_RELAY_PACKET_TRACE");
    return enabled;
}

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

const char* packet_category(const std::string& type) {
    if (type == "pose") {
        return "pose";
    }
    if (type == "hello" || type == "welcome" || type == "peer_joined" ||
        type == "peer_left" || type == "owner_changed" ||
        type == "room_settings" || type == "name_labels")
    {
        return "session";
    }
    if (type == "ping" || type == "pong" || type == "error" || type == "ack") {
        return "control";
    }
    if (type == "chat") {
        return "chat";
    }
    if (type == "sync_request") {
        return "manual_sync_request";
    }
    if (type == "save_snapshot") {
        return "save_snapshot";
    }
    if (type == "event_bit" || type == "tbox_bit" || type == "switch_bit" ||
        type == "room_switch_bit" || type == "item_bit" || type == "dungeon_item_bit")
    {
        return "world_state";
    }
    if (type == "item_get" || type == "trade_item" || type == "item_first_bit" ||
        type == "collect_smell" ||
        type == "collect_crystal" || type == "collect_mirror" ||
        type == "dark_clear_lv" || type == "transform_lv" || type == "region_bit" ||
        type == "collect" || type == "visited_room" || type == "letter_get")
    {
        return "inventory_progress";
    }
    if (type == "key_num" || type == "light_drop_num" || type == "light_drop_get_flag" ||
        type == "max_life_update" || type == "bottle_slots" || type == "rupee_count" ||
        type == "rupee_delta" || type == "poe_count" || type == "malo_fundraising" ||
        type == "charlo_offering" || type == "fish_record")
    {
        return "counters";
    }
    if (type == "reliable") {
        return "reliable_envelope";
    }
    return "other";
}

size_t json_field_bytes(const json& object, const char* key) {
    const auto it = object.find(key);
    return it == object.end() ? 0 : it->dump().size();
}

size_t pose_base_state_bytes(const json& state) {
    if (!state.is_object()) {
        return 0;
    }

    json base = state;
    base.erase("link_matrices");
    base.erase("audio_events");
    return base.dump().size();
}

void trace_packet_tx(const std::string& clientId, const json& message, size_t bytes) {
    if (!relay_packet_trace_enabled()) {
        return;
    }

    const std::string type = message.value("type", "");
    const char* category = packet_category(type);
    if (type == "pose") {
        const json state = message.value("state", json::object());
        std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
                  << " type=" << type << " bytes=" << bytes
                  << " sequence=" << message.value("sequence", 0U)
                  << " base_state=" << pose_base_state_bytes(state)
                  << " link_matrices=" << json_field_bytes(state, "link_matrices")
                  << " audio_events=" << json_field_bytes(state, "audio_events") << "\n";
        return;
    }

    if (type == "save_snapshot") {
        std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
                  << " type=" << type << " bytes=" << bytes
                  << " manual_sync=" << message.value("manual_sync", false)
                  << " full_state=" << json_field_bytes(message, "full_state")
                  << " event_flags=" << json_field_bytes(message, "event_flags")
                  << " chests=" << json_field_bytes(message, "chests")
                  << " switches=" << json_field_bytes(message, "switches")
                  << " items=" << json_field_bytes(message, "items")
                  << " dungeon_items=" << json_field_bytes(message, "dungeon_items") << "\n";
        return;
    }

    std::cout << "MP_RELAY_PACKET_TX client=" << clientId << " category=" << category
              << " type=" << type << " bytes=" << bytes << "\n";
}

struct Client {
    socket_t sock = INVALID_SOCKET;
    std::string id;
    std::string peerEndpoint;
    std::string roomId;
    std::string pendingRoomId;
    std::string pendingAction;
    std::string authNonce;
    std::string authSalt;
    std::string name;
    std::string stage;
    std::string rxBuffer;
    std::deque<std::string> txQueue;
    size_t txOffset = 0;
    size_t txQueuedBytes = 0;
    std::deque<uint32_t> reliableOrder;
    std::unordered_set<uint32_t> reliableSeen;
    std::string udpToken;
    sockaddr_in udpAddr{};
    SteadyClock::time_point acceptedAt = SteadyClock::now();
    SteadyClock::time_point udpRateWindowStarted = SteadyClock::now();
    size_t udpRateWindowBytes = 0;
    SteadyClock::time_point signalWindow = SteadyClock::now();
    size_t signalBytes = 0;
    uint32_t poseCount = 0;
    bool closeAfterFlush = false;
    bool disconnectRequested = false;
    bool wantsPuppet = true;
    bool wantsMidna = false;
    bool supportsSemanticVisuals = false;
    bool supportsSnapshotDeltas = false;
    bool udpAddrKnown = false;
};

struct Room {
    std::string id;
    std::string authSalt;
    std::string proofKey;
    std::vector<std::string> clientIds;
    std::string ownerClientId;
    uint64_t settingsGeneration = 0;
    json pendingSettings;
    std::set<std::string> settingsWaiting;
    SteadyClock::time_point settingsStarted{};
    bool dummyModel = true;
    bool syncFlags = true;
    bool syncWorld = false;
    bool semanticVisualsReady = false;
    bool snapshotDeltasReady = false;
    bool remoteCollision = true;
    bool pvp = false;
};

struct Options {
    std::string host = "127.0.0.1";
    std::string publicHost = "127.0.0.1";
    int port = 34197;
    int publicPort = 0;
    double helloTimeoutSeconds = kHelloTimeoutSeconds;
    bool verbose = false;
    std::string endpointKey;
};

json room_settings_json(const Room& room) {
    return {
        {"dummy_model", room.dummyModel},
        {"sync_flags", room.syncFlags},
        {"sync_world", room.syncWorld},
        {"remote_collision", room.remoteCollision},
        {"pvp", room.pvp},
    };
}

bool apply_room_settings(Room& room, const json& settings) {
    if (!settings.is_object()) {
        return false;
    }

    auto read_bool = [&](const char* key, bool& value) {
        const auto it = settings.find(key);
        if (it == settings.end()) {
            return true;
        }
        if (!it->is_boolean()) {
            return false;
        }
        value = it->get<bool>();
        return true;
    };

    bool dummyModel = room.dummyModel;
    bool syncFlags = room.syncFlags;
    bool syncWorld = room.syncWorld;
    bool remoteCollision = room.remoteCollision;
    bool pvp = room.pvp;
    if (!read_bool("dummy_model", dummyModel) ||
        !read_bool("sync_flags", syncFlags) ||
        !read_bool("sync_world", syncWorld) ||
        !read_bool("remote_collision", remoteCollision) ||
        !read_bool("pvp", pvp))
    {
        return false;
    }

    room.dummyModel = dummyModel;
    room.syncFlags = syncFlags;
    room.syncWorld = syncWorld;
    room.remoteCollision = remoteCollision;
    room.pvp = remoteCollision && pvp;
    return true;
}

class Relay {
public:
    explicit Relay(Options options) : mOptions(std::move(options)) {}

    ~Relay() {
        for (auto& entry : mClients) {
            mConnections.disconnect(entry.second.sock);
        }
        mConnections.close();
#if _WIN32
        if (mWinsockStarted) {
            WSACleanup();
        }
#endif
    }

    bool run() {
#if _WIN32
        // Windows otherwise rounds the 1 ms relay and 5 ms carrier sleeps to
        // coarse timer ticks. Batched poses then supersede each other before
        // the game consumes them. Scope the finer cadence to the running server.
        struct TimerResolution {
            bool active = timeBeginPeriod(1) == TIMERR_NOERROR;
            ~TimerResolution() { if (active) timeEndPeriod(1); }
        } timerResolution;
        WSADATA wsaData{};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            return false;
        }
        mWinsockStarted = true;
#endif

        if (!mConnections.open(mOptions.host, static_cast<uint16_t>(mOptions.port),
                               FD_SETSIZE - 2, true, true, mOptions.endpointKey)) {
            std::cerr << "UDP listen failed\n";
            return false;
        }
        std::cout << "TP relay listening on UDP " << mOptions.host << ":" << mOptions.port << "\n";
        std::cout << "STUN discovery shares the relay UDP port (public " << mOptions.publicPort << ")\n";
        while (true) {
            tick();
        }
    }

private:
    void tick() {
        mConnections.poll();
        accept_client();
        receive_udp_datagrams();
        std::vector<std::string> disconnected;
        for (auto& entry : mClients) {
            Client& client = entry.second;
            if (client.disconnectRequested) {
                disconnected.push_back(client.id);
                continue;
            }
            if (!client.closeAfterFlush && !read_from_client(client))
            {
                disconnected.push_back(client.id);
                continue;
            }
            if (!flush_client(client))
            {
                disconnected.push_back(client.id);
                continue;
            }
            if (client.closeAfterFlush && client.txQueue.empty() && mConnections.drained(client.sock)) {
                disconnected.push_back(client.id);
            }
        }

        const auto now = SteadyClock::now();
        for (auto& entry : mClients) {
            Client& client = entry.second;
            if (client.roomId.empty() && !client.closeAfterFlush &&
                std::chrono::duration<double>(now - client.acceptedAt).count() >=
                    mOptions.helloTimeoutSeconds)
            {
                reject_and_close(client, "hello_timeout");
            }
        }

        for (const auto& [id,room] : mRooms) if (!room.pendingSettings.is_null() &&
            now-room.settingsStarted > std::chrono::seconds(30)) {
            // Never apply half a settings transition or stall it indefinitely.
            for (const auto& member : room.clientIds) disconnected.push_back(member);
        }
        std::sort(disconnected.begin(), disconnected.end());
        disconnected.erase(std::unique(disconnected.begin(), disconnected.end()),
                           disconnected.end());
        for (const std::string& clientId : disconnected) {
            remove_client(clientId);
        }
        mConnections.wait_for_activity(5);
        if (mOptions.verbose || relay_packet_trace_enabled()) {
            std::cout.flush();
        }
    }

    void accept_client() {
        while (true) {
            UdpConnection::Address endpoint;
            auto accepted = mConnections.accept(endpoint);
            if (accepted == UdpConnection::invalid) return;
            sockaddr_in peerAddr{};
            peerAddr.sin_family = AF_INET;
            peerAddr.sin_addr.s_addr = endpoint.ipv4;
            peerAddr.sin_port = htons(endpoint.port);
            if (mClients.size() >= static_cast<size_t>(FD_SETSIZE - 2)) {
                mConnections.disconnect(accepted);
                continue;
            }
            Client client;
            client.sock = static_cast<socket_t>(accepted);
            client.id = make_id("client");
            client.udpToken = make_id("udp");
            char peerHost[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peerAddr.sin_addr, peerHost, sizeof(peerHost));
            client.peerEndpoint =
                std::string(peerHost) + ":" + std::to_string(ntohs(peerAddr.sin_port));
            const std::string clientId = client.id;
            log("connection accepted client=" + clientId + " peer=" + client.peerEndpoint);
            mClients.emplace(clientId, std::move(client));
        }
    }

    bool read_from_client(Client& client) {
        std::array<char, 4096> buffer{};
        size_t readThisTick = 0;
        while (true) {
            const int read = mConnections.receive(client.sock, buffer.data(), buffer.size());
            if (read > 0) {
                readThisTick += static_cast<size_t>(read);
                client.rxBuffer.append(buffer.data(), static_cast<size_t>(read));

                size_t newline = std::string::npos;
                while ((newline = client.rxBuffer.find('\n')) != std::string::npos) {
                    if (newline > kMaxLineBytes) {
                        client.rxBuffer.clear();
                        reject_and_close(client, "message_too_large");
                        return true;
                    }
                    std::string line = client.rxBuffer.substr(0, newline);
                    client.rxBuffer.erase(0, newline + 1);
                    if (line.empty()) {
                        continue;
                    }

                    try {
                        route_message(client, dusklight_online::net::decode_reliable_json(line,kMaxLineBytes));
                    } catch (const dusklight_online::net::ReliableJsonError&) {
                        reject_and_close(client, "invalid_compressed_json");
                        return true;
                    } catch (const json::exception&) {
                        send_error(client, "invalid_json");
                    }
                    if (client.closeAfterFlush || client.disconnectRequested) {
                        return true;
                    }
                }
                if (client.rxBuffer.size() > kMaxLineBytes) {
                    client.rxBuffer.clear();
                    reject_and_close(client, "message_too_large");
                    return true;
                }
                if (readThisTick >= kMaxReadBytesPerTick) {
                    return true;
                }
                continue;
            }

            if (read == 0) {
                return false;
            }
            if (read == -1) {
                return true;
            }
            return false;
        }
    }

    void route_message(Client& client, const json& message) {
        if (!message.is_object()) {
            send_error(client, "invalid_message");
            return;
        }

        const std::string type = message.value("type", "");
        if (type != "peer_reliable" && message.dump().size() > dusklight_online::net::reliableJsonLimit) {
            reject_and_close(client,"message_too_large"); return;
        }
        if (type == "auth_begin") {
            if (!client.roomId.empty() || !client.authNonce.empty() ||
                message.value("protocol_version", -1) != kProtocolVersion) {
                reject_and_close(client, "invalid_auth_begin"); return;
            }
            const std::string roomId = trim(message.value("room_id", ""));
            const std::string action = message.value("action", "");
            if (roomId.empty() || roomId.size() > kMaxRoomIdBytes ||
                (action != "create" && action != "join")) {
                reject_and_close(client, "invalid_auth_begin"); return;
            }
            const auto room = mRooms.find(roomId);
            if (room == mRooms.end() && action != "create") {
                reject_and_close(client, "lobby_not_found"); return;
            }
            client.pendingRoomId = roomId;
            client.pendingAction = action;
            client.authNonce = dusklight_online::net::auth::random_nonce();
            client.authSalt = room == mRooms.end() ?
                dusklight_online::net::auth::random_nonce() : room->second.authSalt;
            send_json(client, {{"type", "auth_challenge"},
                {"nonce", client.authNonce}, {"salt", client.authSalt}});
            return;
        }
        if (type == "hello") {
            if (!client.roomId.empty()) {
                send_error(client, "already_joined");
                return;
            }
            handle_hello(client, message);
            return;
        }

        if (client.roomId.empty()) {
            send_error(client, "expected_hello");
            return;
        }

        if (type == "ping") {
            send_json(client, {{"type", "pong"}, {"time", now_seconds()}});
            return;
        }

        if (type == "peer_reliable") {
            const auto& recipients = message.at("recipients");
            const auto& body = message.at("body");
            if (!recipients.is_array() || recipients.empty() || recipients.size() > 7 ||
                !body.is_object() || body.dump().size() > 512*1024+128) {
                reject_and_close(client,"invalid_peer_delivery"); return;
            }
            std::set<std::string> targets;
            auto room = mRooms.find(client.roomId);
            if (room == mRooms.end()) return;
            for (const auto& recipient : recipients) {
                const auto name = recipient.at("id").get<std::string>();
                const auto& sequence = recipient.at("sequence");
                if (name == client.id || !targets.insert(name).second ||
                    !sequence.is_number_unsigned() || !sequence.get<uint64_t>() ||
                    std::find(room->second.clientIds.begin(),room->second.clientIds.end(),name) == room->second.clientIds.end()) {
                    reject_and_close(client,"invalid_peer_delivery"); return;
                }
            }
            for (const auto& recipient : recipients) send_to_client(client,recipient.at("id").get<std::string>(),
                {{"type","peer_reliable"},{"client_id",client.id},
                 {"frame",{{"sequence",recipient.at("sequence")},{"body",body}}}});
            return;
        }
        if (type == "peer_receipt") {
            const auto& ack = message.at("frame").at("ack");
            if (!ack.is_number_unsigned()) { reject_and_close(client,"invalid_peer_receipt"); return; }
            send_to_client(client,message.at("target_client_id").get<std::string>(),
                {{"type","peer_receipt"},{"client_id",client.id},{"frame",{{"ack",ack}}}});
            return;
        }

        if (type == "ice_signal") {
            const auto now = SteadyClock::now();
            if (now - client.signalWindow >= std::chrono::seconds(1)) {
                client.signalWindow = now; client.signalBytes = 0;
            }
            const auto target = message.find("target_client_id");
            const auto kind = message.find("kind");
            const auto data = message.find("data");
            const auto generation = message.find("generation");
            if (target == message.end() || !target->is_string() ||
                kind == message.end() || !kind->is_number_integer() ||
                data == message.end() || !data->is_string() || generation == message.end() ||
                !generation->is_number_unsigned() || generation->get<uint64_t>() > UINT32_MAX) return;
            const auto text = data->get<std::string>();
            const int k = kind->get<int>();
            if (k < 0 || k > 2 || text.size() > (k == 0 ? 4095U : k == 1 ? 255U : 0U) ||
                text.find('\0') != std::string::npos || client.signalBytes + text.size() + 64 > 65536) return;
            client.signalBytes += text.size() + 64;
            send_to_client(client, target->get<std::string>(), {
                {"type", "ice_signal"}, {"client_id", client.id}, {"kind", k}, {"data", text}, {"generation", *generation}});
            return;
        }

        if (type == "peer_stage") {
            const auto stage = message.find("stage");
            if (stage != message.end() && stage->is_string() && stage->get_ref<const std::string&>().size() <= 32)
                client.stage = stage->get<std::string>();
            return;
        }
        if (type == "settings_ready") {
            auto room = mRooms.find(client.roomId);
            const auto number = message.find("generation");
            if (room == mRooms.end() || room->second.pendingSettings.is_null() ||
                number == message.end() || !number->is_number_unsigned() ||
                number->get<uint64_t>() != room->second.settingsGeneration+1) return;
            room->second.settingsWaiting.erase(client.id);
            finish_settings(room->second);
            return;
        }

        if (type == "kick") {
            auto room = mRooms.find(client.roomId);
            if (room == mRooms.end()) {
                send_error(client, "lobby_not_found");
                return;
            }
            if (room->second.ownerClientId != client.id) {
                send_error(client, "owner_only");
                return;
            }
            const auto targetField = message.find("target_client_id");
            if (targetField == message.end() || !targetField->is_string()) {
                send_error(client, "invalid_target");
                return;
            }
            const std::string targetId = targetField->get<std::string>();
            const bool admitted = targetId != client.id &&
                std::find(room->second.clientIds.begin(), room->second.clientIds.end(),
                          targetId) != room->second.clientIds.end();
            auto target = mClients.find(targetId);
            if (!admitted || target == mClients.end() || target->second.closeAfterFlush) {
                send_error(client, "unknown_target");
                return;
            }
            if (!send_json(target->second,
                           {{"type", "kicked"}, {"reason", "removed_by_host"}})) {
                target->second.disconnectRequested = true;
            } else {
                // Stop accepting anything else from the removed member, then
                // close only after the reliable notice has been acknowledged.
                target->second.closeAfterFlush = true;
            }
            log("kick room=" + client.roomId + " owner=" + client.id +
                " target=" + targetId);
            return;
        }

        if (type == "puppet_preference") {
            client.wantsPuppet = message.value("want_puppet", client.wantsPuppet);
            client.wantsMidna = message.value("want_midna", client.wantsMidna);
        } else if (type == "midna_preference") {
            client.wantsMidna = message.value("want_midna", client.wantsMidna);
        }
        if (type == "presence" || type == "progression_state") {
            client.stage = message.value("stage", client.stage);
        }

        if (type == "room_settings") {
            auto roomIt = mRooms.find(client.roomId);
            if (roomIt == mRooms.end()) {
                send_error(client, "lobby_not_found");
                return;
            }
            Room& room = roomIt->second;
            if (room.ownerClientId != client.id) {
                send_error(client, "owner_only");
                return;
            }
            if (!room.pendingSettings.is_null()) { send_error(client,"settings_busy"); return; }
            Room candidate = room;
            if (!apply_room_settings(candidate, message.value("settings",json{}))) {
                send_error(client,"invalid_settings"); return;
            }
            room.pendingSettings = room_settings_json(candidate);
            room.settingsWaiting = {room.clientIds.begin(),room.clientIds.end()};
            room.settingsStarted = SteadyClock::now();
            const json prepare = {{"type","settings_prepare"},{"generation",room.settingsGeneration+1},
                {"participants",room.clientIds}};
            send_json(client,prepare); broadcast(client,prepare);
            return;
        }

        if (type == "pose") {
            json routed = {
                {"type", "pose"},
                {"client_id", client.id},
                {"sequence", message.value("sequence", 0U)},
                {"state", message.value("state", json::object())},
            };
            broadcast(client, routed);
            ++client.poseCount;
            if (mOptions.verbose && client.poseCount % 30 == 1) {
                const json state = message.value("state", json::object());
                std::cout << "pose client=" << client.id
                          << " seq=" << message.value("sequence", 0U)
                          << " stage=" << state.value("stage", "")
                          << " room=" << state.value("room", -1) << "\n";
            }
            return;
        }

        if (type == "reliable") {
            const uint32_t sequence = message.value("sequence", 0U);
            if (!client.reliableSeen.insert(sequence).second) {
                return;
            }
            client.reliableOrder.push_back(sequence);
            if (client.reliableOrder.size() > kMaxReliableSequences) {
                client.reliableSeen.erase(client.reliableOrder.front());
                client.reliableOrder.pop_front();
            }
            broadcast(client, {
                {"type", "reliable"},
                {"client_id", client.id},
                {"sequence", sequence},
                {"state", message.value("state", json::object())},
            });
            send_json(client, {{"type", "ack"}, {"sequence", sequence}});
            return;
        }

        if (type == "sync_request") {
            const std::string targetClientId = message.value("target_client_id", "");
            if (targetClientId.empty()) {
                send_error(client, "missing_target");
                return;
            }

            json routed = message;
            routed["client_id"] = client.id;
            if (!send_to_client(client, targetClientId, routed)) {
                send_error(client, "unknown_target");
            }
            return;
        }

        if (kGameplayRouteTypes.find(type) != kGameplayRouteTypes.end()) {
            json routed = message;
            routed["client_id"] = client.id;
            const std::string targetClientId = message.value("target_client_id", "");
            if (!targetClientId.empty()) {
                if (!send_to_client(client, targetClientId, routed)) {
                    send_error(client, "unknown_target");
                }
            } else {
                broadcast(client, routed);
            }
            return;
        }

        send_error(client, "unknown_message");
    }

    void handle_hello(Client& client, const json& hello) {
        if (hello.value("protocol_version", -1) != kProtocolVersion) {
            send_error(client, "protocol_version");
            return;
        }

        std::string roomId = trim(hello.value("room_id", ""));
        std::string name = trim(hello.value("name", ""));
        const std::string action = hello.value("action", "");
        const std::string proof = hello.value("code_proof", "");
        if (roomId.empty()) {
            send_error(client, "missing_lobby");
            return;
        }
        if (name.empty()) {
            send_error(client, "missing_username");
            return;
        }
        if (roomId.size() > kMaxRoomIdBytes) {
            send_error(client, "lobby_too_long");
            return;
        }
        if (name.size() > kMaxUsernameBytes) {
            send_error(client, "username_too_long");
            return;
        }
        if (action != "create" && action != "join") {
            send_error(client, "invalid_action");
            return;
        }

        if (roomId != client.pendingRoomId || action != client.pendingAction ||
            client.authNonce.empty() || !dusklight_online::net::auth::unhex_digest(proof)) {
            reject_and_close(client, "invalid_lobby_code_proof"); return;
        }
        auto roomIt = mRooms.find(roomId);
        if (action == "create") {
            if (roomIt != mRooms.end()) {
                send_error(client, "lobby_exists");
                return;
            }
            const auto verifier = dusklight_online::net::auth::unhex_digest(
                hello.value("code_verifier", ""));
            const auto key = verifier ? dusklight_online::net::auth::room_proof_key(
                *verifier, client.authSalt) : std::nullopt;
            if (!key || !dusklight_online::net::auth::equal(proof,
                dusklight_online::net::auth::room_proof(*key, client.authNonce))) {
                reject_and_close(client, "invalid_lobby_code_proof"); return;
            }
            Room room;
            room.id = roomId;
            room.authSalt = client.authSalt;
            room.proofKey = dusklight_online::net::auth::hex(*key);
            room.ownerClientId = client.id;
            if (!apply_room_settings(room, hello.value("settings", json::object()))) {
                send_error(client, "invalid_settings");
                return;
            }
            roomIt = mRooms.emplace(roomId, std::move(room)).first;
        } else if (roomIt == mRooms.end()) {
            send_error(client, "lobby_not_found");
            return;
        } else if (client.authSalt != roomIt->second.authSalt ||
                   !dusklight_online::net::auth::equal(proof,
                   dusklight_online::net::auth::room_proof(
                     *dusklight_online::net::auth::unhex_digest(roomIt->second.proofKey),
                     client.authNonce))) {
            send_error(client, "bad_lobby_code");
            return;
        }

        if (roomIt->second.clientIds.size() >= kMaxRoomClients) {
            send_error(client, "lobby_full");
            return;
        }

        client.roomId = roomId;
        client.name = name;
        client.wantsPuppet = hello.value("want_puppet", true);
        client.wantsMidna = hello.value("want_midna", false);
        client.supportsSemanticVisuals = has_semantic_visual_capability(hello);
        client.supportsSnapshotDeltas = has_snapshot_delta_capability(hello);

        json peers = json::array();
        for (const std::string& peerId : roomIt->second.clientIds) {
            const auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end()) {
                peers.push_back({{"client_id", peerIt->second.id}, {"name", peerIt->second.name},
                    {"want_puppet", peerIt->second.wantsPuppet}, {"stage", peerIt->second.stage}});
            }
        }

        roomIt->second.clientIds.push_back(client.id);
        roomIt->second.semanticVisualsReady =
            room_semantic_visuals_ready(roomIt->second);
        roomIt->second.snapshotDeltasReady =
            room_snapshot_deltas_ready(roomIt->second);
        log("join room=" + roomId + " client=" + client.id + " name=" + client.name);

        send_json(client, {
            {"type", "welcome"},
            {"protocol_version", kProtocolVersion},
            {"room_id", roomId},
            {"client_id", client.id},
            {"udp_token", client.udpToken},
            {"stun_port", mOptions.publicPort},
            {"settings_generation", roomIt->second.settingsGeneration},
            {"settings_pending", !roomIt->second.pendingSettings.is_null()},
            {"owner_client_id", roomIt->second.ownerClientId},
            {"semantic_visuals_ready", roomIt->second.semanticVisualsReady},
            {"snapshot_deltas_ready", roomIt->second.snapshotDeltasReady},
            {"settings", room_settings_json(roomIt->second)},
            {"peers", peers},
        });
        broadcast(client, {
            {"type", "peer_joined"},
            {"client_id", client.id},
            {"want_puppet", client.wantsPuppet},
            {"name", client.name},
            {"semantic_visuals_ready", roomIt->second.semanticVisualsReady},
            {"snapshot_deltas_ready", roomIt->second.snapshotDeltasReady},
        });
    }

    bool room_semantic_visuals_ready(const Room& room) const {
        if (room.clientIds.empty()) return false;
        for (const std::string& clientId : room.clientIds) {
            const auto client = mClients.find(clientId);
            if (client == mClients.end() || !client->second.supportsSemanticVisuals) {
                return false;
            }
        }
        return true;
    }

    bool room_snapshot_deltas_ready(const Room& room) const {
        if (room.clientIds.empty()) return false;
        for (const std::string& clientId : room.clientIds) {
            const auto client = mClients.find(clientId);
            if (client == mClients.end() || !client->second.supportsSnapshotDeltas) {
                return false;
            }
        }
        return true;
    }

    void broadcast(const Client& sender, const json& message) {
        const auto roomIt = mRooms.find(sender.roomId);
        if (roomIt == mRooms.end()) {
            return;
        }

        for (const std::string& peerId : roomIt->second.clientIds) {
            if (peerId == sender.id) {
                continue;
            }
            auto peerIt = mClients.find(peerId);
            if (peerIt != mClients.end() && !peerIt->second.closeAfterFlush) {
                if (!send_json(peerIt->second, message)) {
                    peerIt->second.disconnectRequested = true;
                }
            }
        }
    }

    static std::string udp_sender_id(const UdpRelayHeader& header) {
        size_t length = 0;
        while (length < sizeof(header.senderId) && header.senderId[length] != '\0') {
            ++length;
        }
        return std::string(header.senderId, length);
    }

    static std::string udp_acked_sender_id(const UdpPoseAckPacket& ack) {
        size_t length = 0;
        while (length < sizeof(ack.ackedSenderId) && ack.ackedSenderId[length] != '\0') {
            ++length;
        }
        return std::string(ack.ackedSenderId, length);
    }

    static bool same_udp_endpoint(const sockaddr_in& left, const sockaddr_in& right) {
        return left.sin_family == right.sin_family &&
               left.sin_port == right.sin_port &&
               left.sin_addr.s_addr == right.sin_addr.s_addr;
    }

    void send_udp_to_client(Client& client, const uint8_t* bytes, size_t size) {
        if (!client.udpAddrKnown) {
            return;
        }
        mConnections.send_realtime({client.udpAddr.sin_addr.s_addr, ntohs(client.udpAddr.sin_port)}, {bytes, size});
    }

    void receive_udp_datagrams() {
        std::array<uint8_t, kMaxUdpDatagramBytes> packet{};
        for (size_t receivedThisTick = 0; receivedThisTick < 512; ++receivedThisTick) {
            UdpConnection::Address endpoint;
            UdpConnection::Id authenticatedPeer = UdpConnection::invalid;
            const int received = mConnections.receive_realtime(endpoint, packet,
                                                               &authenticatedPeer);
            if (received < 0) return;
            sockaddr_in from{};
            from.sin_family = AF_INET;
            from.sin_addr.s_addr = endpoint.ipv4;
            from.sin_port = htons(endpoint.port);
            const std::span<const uint8_t> wire(packet.data(), static_cast<size_t>(received));
            if (const auto header = dusklight_online::net::peer_group_header(wire)) {
                using namespace dusklight_online::net;
                const auto payload = wire.subspan(header);
                if (std::memcmp(payload.data(), "DMPU", 4) != 0) continue;
                const auto sender = tunnel_read(wire.subspan(4, 8));
                std::array<uint64_t, 7> seen{};
                for (size_t i = 0; i < wire[12]; ++i) {
                    const auto target = tunnel_read(wire.subspan(13 + i * 8, 8));
                    if (!target || std::find(seen.begin(), seen.end(), target) != seen.end()) continue;
                    seen[i] = target;
                    route_peer_datagram(from, peer_tunnel(sender, target, payload),
                                        authenticatedPeer);
                }
                continue;
            }
            if (dusklight_online::net::is_peer_tunnel(wire)) {
                route_peer_datagram(from, wire, authenticatedPeer);
                continue;
            }
            if (static_cast<size_t>(received) < sizeof(UdpRelayHeader)) {
                continue;
            }

            UdpRelayHeader header{};
            std::memcpy(&header, packet.data(), sizeof(header));
            const size_t packetSize = static_cast<size_t>(received);
            if (std::memcmp(header.magic, "DMPU", 4) != 0 || header.version != 1 ||
                header.headerSize != sizeof(UdpRelayHeader) ||
                sizeof(UdpRelayHeader) + header.payloadSize != packetSize)
            {
                continue;
            }

            const std::string senderId = udp_sender_id(header);
            auto senderIt = mClients.find(senderId);
            if (senderIt == mClients.end() || senderIt->second.roomId.empty() ||
                senderIt->second.closeAfterFlush) {
                continue;
            }
            Client& sender = senderIt->second;
            if (sender.sock != authenticatedPeer) continue;
            const uint8_t* payload = packet.data() + sizeof(UdpRelayHeader);

            if (header.type == kUdpPacketTypeRelayRegister) {
                const std::string token(reinterpret_cast<const char*>(payload),
                                        header.payloadSize);
                if (token == sender.udpToken) {
                    mConnections.bind_realtime(sender.sock, {from.sin_addr.s_addr, ntohs(from.sin_port)});
                    const bool wasKnown = sender.udpAddrKnown;
                    const bool endpointChanged =
                        !wasKnown || !same_udp_endpoint(sender.udpAddr, from);
                    sender.udpAddr = from;
                    sender.udpAddrKnown = true;
                    if (!wasKnown) {
                        send_json(sender, {{"type", "udp_ready"}});
                    }
                    if (endpointChanged) {
                        log("udp register client=" + sender.id);
                    }
                }
                continue;
            }

            if (!sender.udpAddrKnown || !same_udp_endpoint(sender.udpAddr, from) ||
                (header.type != kUdpPacketTypePoseJson &&
                 header.type != kUdpPacketTypePoseMsgpack &&
                 header.type != kUdpPacketTypeRemoteObject &&
                 header.type != kUdpPacketTypeMidnaMsgpack &&
                 header.type != kUdpPacketTypePoseAck &&
                 header.type != kUdpPacketTypeSemanticPoseMsgpack &&
                 header.type != kUdpPacketTypeVoiceOpus))
            {
                continue;
            }
            if (header.type == kUdpPacketTypeVoiceOpus &&
                (header.sequence == 0 || header.chunkIndex != 0 || header.chunkCount != 1 ||
                 header.payloadSize <= 21 || header.payloadSize > 421 ||
                 header.compressedSize != header.payloadSize ||
                 header.uncompressedSize != header.payloadSize)) continue;

            const auto now = SteadyClock::now();
            if (now - sender.udpRateWindowStarted >= std::chrono::seconds(1)) {
                sender.udpRateWindowStarted = now;
                sender.udpRateWindowBytes = 0;
            }
            if (packetSize > kMaxUdpBytesPerClientSecond -
                                 std::min(sender.udpRateWindowBytes,
                                          kMaxUdpBytesPerClientSecond))
            {
                continue;
            }
            sender.udpRateWindowBytes += packetSize;

            const auto roomIt = mRooms.find(sender.roomId);
            if (roomIt == mRooms.end()) {
                continue;
            }

            if (header.type == kUdpPacketTypePoseAck) {
                if (header.payloadSize != sizeof(UdpPoseAckPacket)) {
                    continue;
                }
                UdpPoseAckPacket ack{};
                std::memcpy(&ack, payload, sizeof(ack));
                const std::string targetId = udp_acked_sender_id(ack);
                auto targetIt = mClients.find(targetId);
                if (targetIt != mClients.end() &&
                    targetIt->second.roomId == sender.roomId)
                {
                    send_udp_to_client(targetIt->second, packet.data(), packetSize);
                }
                continue;
            }

            for (const std::string& peerId : roomIt->second.clientIds) {
                if (peerId == sender.id) {
                    continue;
                }
                auto peerIt = mClients.find(peerId);
                if (peerIt == mClients.end()) {
                    continue;
                }
                Client& peer = peerIt->second;
                if ((header.type == kUdpPacketTypePoseJson ||
                     header.type == kUdpPacketTypePoseMsgpack ||
                     header.type == kUdpPacketTypeSemanticPoseMsgpack) &&
                    !peer.wantsPuppet)
                {
                    continue;
                }
                if (header.type == kUdpPacketTypeSemanticPoseMsgpack &&
                    !peer.supportsSemanticVisuals)
                {
                    continue;
                }
                if (header.type == kUdpPacketTypeMidnaMsgpack &&
                    (!peer.wantsPuppet || !peer.wantsMidna))
                {
                    continue;
                }
                if ((header.type == kUdpPacketTypePoseJson ||
                     header.type == kUdpPacketTypePoseMsgpack ||
                     header.type == kUdpPacketTypeSemanticPoseMsgpack ||
                     header.type == kUdpPacketTypeMidnaMsgpack) &&
                    !sender.stage.empty() && !peer.stage.empty() &&
                    sender.stage != peer.stage)
                {
                    continue;
                }
                send_udp_to_client(peer, packet.data(), packetSize);
            }
        }
    }

    void route_peer_datagram(const sockaddr_in& from, std::span<const uint8_t> wire,
                             UdpConnection::Id authenticatedPeer) {
        using namespace dusklight_online::net;
        if (!is_peer_tunnel(wire)) return;
        const auto senderId = "client_" + std::to_string(tunnel_read(wire.subspan(4, 8)));
        const auto targetId = "client_" + std::to_string(tunnel_read(wire.subspan(12, 8)));
        auto source = mClients.find(senderId), destination = mClients.find(targetId);
        if (source == mClients.end() || destination == mClients.end() || source == destination ||
            source->second.closeAfterFlush || destination->second.closeAfterFlush) return;
        auto& sender = source->second; auto& target = destination->second;
        if (sender.roomId.empty() || sender.roomId != target.roomId ||
            sender.sock != authenticatedPeer ||
            !sender.udpAddrKnown || !same_udp_endpoint(from, sender.udpAddr)) return;
        const auto now = SteadyClock::now();
        if (now - sender.udpRateWindowStarted >= std::chrono::seconds(1)) {
            sender.udpRateWindowStarted = now; sender.udpRateWindowBytes = 0;
        }
        if (sender.udpRateWindowBytes + wire.size() > kMaxUdpBytesPerClientSecond) return;
        sender.udpRateWindowBytes += wire.size();
        const auto payload = wire.subspan(20);
        if (is_peer_tunnel(payload)) return;
        const auto kind = datagram_kind(payload);
        if (kind == DatagramKind::Invalid) return;
        if (kind == DatagramKind::Realtime) {
            UdpRelayHeader header{};
            std::memcpy(&header, payload.data(), sizeof(header));
            if (header.version != 1 || header.headerSize != sizeof(header) ||
                payload.size() != sizeof(header) + header.payloadSize || udp_sender_id(header) != senderId) return;
            const bool pose = header.type == kUdpPacketTypePoseJson || header.type == kUdpPacketTypePoseMsgpack ||
                header.type == kUdpPacketTypeSemanticPoseMsgpack;
            if (!pose && header.type != kUdpPacketTypeRemoteObject &&
                header.type != kUdpPacketTypePoseAck &&
                header.type != kUdpPacketTypeVoiceOpus) return;
            if (header.type == kUdpPacketTypeVoiceOpus &&
                (header.sequence == 0 || header.chunkIndex != 0 || header.chunkCount != 1 ||
                 header.payloadSize <= 21 || header.payloadSize > 421 ||
                 header.compressedSize != header.payloadSize ||
                 header.uncompressedSize != header.payloadSize)) return;
            if (pose && (!target.wantsPuppet ||
                (!sender.stage.empty() && !target.stage.empty() && sender.stage != target.stage))) return;
            if (header.type == kUdpPacketTypeSemanticPoseMsgpack && !target.supportsSemanticVisuals) return;
            if (header.type == kUdpPacketTypePoseAck) {
                if (header.payloadSize != sizeof(UdpPoseAckPacket)) return;
                UdpPoseAckPacket ack{}; std::memcpy(&ack, payload.data() + sizeof(header), sizeof(ack));
                if (udp_acked_sender_id(ack) != targetId) return;
            }
        }
        send_udp_to_client(target, wire.data(), wire.size());
    }

    bool send_to_client(const Client& sender, const std::string& targetClientId,
                        const json& message) {
        const auto roomIt = mRooms.find(sender.roomId);
        if (roomIt == mRooms.end() ||
            std::find(roomIt->second.clientIds.begin(), roomIt->second.clientIds.end(),
                      targetClientId) == roomIt->second.clientIds.end())
        {
            return false;
        }

        auto peerIt = mClients.find(targetClientId);
        if (peerIt == mClients.end() || peerIt->second.closeAfterFlush) {
            return false;
        }

        if (!send_json(peerIt->second, message)) {
            peerIt->second.disconnectRequested = true;
        }
        return true;
    }

    void remove_client(const std::string& clientId) {
        auto clientIt = mClients.find(clientId);
        if (clientIt == mClients.end()) {
            return;
        }

        const std::string roomId = clientIt->second.roomId;
        log("connection closed client=" + clientId + " peer=" +
            clientIt->second.peerEndpoint + " room=" +
            (roomId.empty() ? std::string("<none>") : roomId));
        Client departed;
        departed.id = clientId;
        departed.roomId = roomId;
        mConnections.disconnect(clientIt->second.sock);
        mClients.erase(clientIt);

        if (!roomId.empty()) {
            auto roomIt = mRooms.find(roomId);
            if (roomIt != mRooms.end()) {
                Room& room = roomIt->second;
                room.clientIds.erase(
                    std::remove(room.clientIds.begin(), room.clientIds.end(), clientId),
                    room.clientIds.end());
                room.settingsWaiting.erase(clientId);
                room.semanticVisualsReady = room_semantic_visuals_ready(room);
                room.snapshotDeltasReady = room_snapshot_deltas_ready(room);
                log("leave room=" + roomId + " client=" + clientId);
                broadcast(departed, {
                    {"type", "peer_left"},
                    {"client_id", clientId},
                    {"semantic_visuals_ready", room.semanticVisualsReady},
                    {"snapshot_deltas_ready", room.snapshotDeltasReady},
                });
                if (room.clientIds.empty()) {
                    mRooms.erase(roomIt);
                } else if (room.ownerClientId == clientId) {
                    room.ownerClientId = room.clientIds.front();
                    log("owner transfer room=" + roomId + " client=" + room.ownerClientId);
                    broadcast(departed, {
                        {"type", "owner_changed"},
                        {"owner_client_id", room.ownerClientId},
                    });
                }
                if (auto remaining=mRooms.find(roomId); remaining!=mRooms.end()) finish_settings(remaining->second);
            }
        }
    }

    void finish_settings(Room& room) {
        if (room.pendingSettings.is_null() || !room.settingsWaiting.empty()) return;
        apply_room_settings(room,room.pendingSettings);
        room.pendingSettings = nullptr;
        ++room.settingsGeneration;
        const json commit = {{"type","room_settings"},{"settings_generation",room.settingsGeneration},
            {"owner_client_id",room.ownerClientId},{"semantic_visuals_ready",room.semanticVisualsReady},
            {"snapshot_deltas_ready",room.snapshotDeltasReady},{"settings",room_settings_json(room)}};
        for (const auto& id : room.clientIds) if (auto it=mClients.find(id); it!=mClients.end()) send_json(it->second,commit);
    }

    bool send_json(Client& client, const json& message) {
        std::string bytes = dusklight_online::net::encode_reliable_json(message,dusklight_online::net::reliablePeerFrameLimit);
        bytes.push_back('\n');
        trace_packet_tx(client.id, message, bytes.size());
        if (bytes.size() > kMaxQueuedBytes ||
            client.txQueuedBytes > kMaxQueuedBytes - bytes.size())
        {
            log("disconnect slow client=" + client.id + " queued_bytes=" +
                std::to_string(client.txQueuedBytes));
            client.disconnectRequested = true;
            return false;
        }
        client.txQueuedBytes += bytes.size();
        client.txQueue.push_back(std::move(bytes));
        return true;
    }

    bool flush_client(Client& client) {
        while (!client.txQueue.empty()) {
            const auto& bytes = client.txQueue.front();
            if (!mConnections.send(client.sock, bytes.data(), bytes.size())) return false;
            client.txQueuedBytes -= bytes.size();
            client.txQueue.pop_front();
        }
        return mConnections.alive(client.sock);
    }

    void send_error(Client& client, const std::string& error) {
        log("client error client=" + client.id + " peer=" + client.peerEndpoint +
            " room=" + (client.roomId.empty() ? std::string("<none>") : client.roomId) +
            " error=" + error);
        send_json(client, {{"type", "error"}, {"error", error}});
    }

    void reject_and_close(Client& client, const std::string& error) {
        send_error(client, error);
        client.closeAfterFlush = true;
        if (client.txQueue.empty()) {
            client.disconnectRequested = true;
        }
    }

    static std::string make_id(std::string_view prefix) {
        static std::mt19937_64 rng{std::random_device{}()};
        const uint64_t value = rng();
        return std::string(prefix) + "_" + std::to_string(value);
    }

    static double now_seconds() {
        using clock = std::chrono::system_clock;
        return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
    }

    static std::string trim(const std::string& value) {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        return first < last ? std::string(first, last) : std::string();
    }

    void log(const std::string& message) const {
        if (mOptions.verbose) {
            std::cout << message << "\n";
        }
    }

    Options mOptions;
    UdpConnection mConnections;
    std::map<std::string, Client> mClients;
    std::map<std::string, Room> mRooms;
#if _WIN32
    bool mWinsockStarted = false;
#endif
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--host") {
            options.host = next_value("--host");
        } else if (arg == "--public-host") {
            options.publicHost = next_value("--public-host");
        } else if (arg == "--port") {
            options.port = std::stoi(next_value("--port"));
        } else if (arg == "--public-port") {
            options.publicPort = std::stoi(next_value("--public-port"));
        } else if (arg == "--hello-timeout-ms") {
            const int timeoutMs = std::stoi(next_value("--hello-timeout-ms"));
            if (timeoutMs < 1) {
                std::cerr << "--hello-timeout-ms must be positive\n";
                std::exit(2);
            }
            options.helloTimeoutSeconds = static_cast<double>(timeoutMs) / 1000.0;
        } else if (arg == "--endpoint-key") {
            options.endpointKey = next_value("--endpoint-key");
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--version") {
            std::cout << "dusklight_online_relay " << DUSKLIGHT_RELAY_VERSION << "\n";
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: dusklight_online_relay [--host 127.0.0.1] "
                         "[--port 34197] [--public-host 127.0.0.1] "
                         "[--public-port 34197] [--hello-timeout-ms 10000] "
                         "[--verbose] [--version]\n";
            std::exit(0);
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            std::exit(2);
        }
    }
    if (options.port < 1 || options.port > 65535) {
        std::cerr << "--port must be between 1 and 65535\n";
        std::exit(2);
    }
    if (options.publicHost.empty()) {
        std::cerr << "--public-host cannot be empty\n";
        std::exit(2);
    }
    if (options.publicPort == 0) {
        options.publicPort = options.port;
    }
    if (options.publicPort < 1 || options.publicPort > 65535) {
        std::cerr << "--public-port must be between 1 and 65535\n";
        std::exit(2);
    }
    if (options.endpointKey.empty()) options.endpointKey = dusk::multiplayer::make_session_token(16);
    if (options.endpointKey.size() < 16) {
        std::cerr << "--endpoint-key must contain at least 16 characters\n";
        std::exit(2);
    }
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        dusk::multiplayer::InviteCodePayload endpoint;
        endpoint.transport = "relay";
        endpoint.host = options.publicHost;
        endpoint.port = options.publicPort;
        endpoint.room = "relay-endpoint";
        endpoint.sessionId = "relay";
        endpoint.sessionKey = options.endpointKey;
        std::cout << "Relay code: " << dusk::multiplayer::create_invite_code(endpoint)
                  << std::endl;

        Relay relay(options);
        return relay.run() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "invalid relay option: " << error.what() << "\n";
        return 2;
    }
}
