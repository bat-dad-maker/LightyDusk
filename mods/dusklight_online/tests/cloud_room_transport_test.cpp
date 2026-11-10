#include "dusklight_online/net/transport.hpp"
#include "dusklight_online/net/auth_code.hpp"
#include "dusklight_online/net/cloud_cipher.hpp"
#include "dusklight_online/net/reliable_json.hpp"
#include "dusklight_online/net/wire_limits.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dusklight_online::net;
using json = nlohmann::json;

namespace {

class Channel;

struct Broker {
    std::vector<std::string> urls, wires;
    enum class Attack { None, Plaintext, Tamper, Replay } attack = Attack::None;
    std::map<std::string, Channel*> members;
    uint32_t nextId = 1;
    bool gameplayForwardingAttempt = false;
    bool voiceSettingsOnRoomChannel = false;
    int roomSettingsMessages = 0;
    bool forwardIce = true;
    bool resumeEnabled = false;
    std::map<std::string, json> suspended;
    json resumePatch = json::object();
    int kickRequests = 0;
    std::string dropJoinNotificationTo;
    RoomSettings roomSettings;
    std::string owner;
    std::optional<auth::Digest> proofKey;
    const std::string authSalt = "00112233445566778899aabbccddeeff";
    void accept(Channel& channel, const json& message);
    void leave(Channel& channel);
};

class Channel final : public RoomChannel {
public:
    explicit Channel(Broker& broker) : broker(broker) {}
    ~Channel() override { close(); }
    bool open(std::string_view url, std::string&) override {
        active = true;
        broker.urls.emplace_back(url);
        cipher.emplace(url.substr(url.rfind('/') + 1), false);
        events.push_back({EventKind::Open, {}});
        nonce = auth::random_nonce();
        events.push_back({EventKind::Message, json({{"type", "secure_hello"}, {"version", 1},
            {"public_key", cipher->public_key()}}).dump()});
        return true;
    }
    bool send(std::string_view text) override {
        if (!active) return false;
        broker.wires.emplace_back(text);
        if (!cipher->established()) {
            const auto ready = json::parse(text);
            if (ready.value("type", "") != "secure_ready" || ready.value("version", 0) != 1 ||
                !cipher->exchange(ready.value("public_key", "")))
                throw std::runtime_error("invalid cloud key exchange");
            const json challenge = {{"type", "auth_challenge"}, {"nonce", nonce}, {"salt", broker.authSalt}};
            if (broker.attack == Broker::Attack::Plaintext)
                events.push_back({EventKind::Message, challenge.dump()});
            else {
                push(challenge);
                if (broker.attack == Broker::Attack::Tamper)
                    events.back().text.back() = events.back().text.back() == '0' ? '1' : '0';
                if (broker.attack == Broker::Attack::Replay) events.push_back(events.back());
            }
        } else {
            const auto plaintext = cipher->open(text);
            if (!plaintext) throw std::runtime_error("invalid encrypted cloud request");
            broker.accept(*this, json::parse(*plaintext));
        }
        return true;
    }
    bool poll(Event& event) override {
        if (holdPoll) return false;
        if (events.empty()) return false;
        event = std::move(events.front()); events.pop_front();
        return true;
    }
    void close() override {
        if (!active) return;
        active = false;
        broker.leave(*this);
        events.clear();
        id.clear();
    }
    void push(const json& value) {
        const auto wire = cipher->seal(value.dump());
        if (wire.empty()) throw std::runtime_error("invalid encrypted cloud reply");
        broker.wires.push_back(wire);
        events.push_back({EventKind::Message, wire});
    }
    void drop() {
        broker.suspended[id] = {{"name", name}, {"token", token}};
        broker.members.erase(id);
        active = false;
        events.clear();
        events.push_back({EventKind::Closed, "WinHTTP WebSocket failed (12030)", 0, 6});
    }
    std::optional<CloudCipher> cipher;
    Broker& broker;
    std::string id;
    std::string name;
    std::string nonce;
    std::deque<Event> events;
    bool active = false;
    bool holdPoll = false;
    std::string token;
};

void Broker::accept(Channel& channel, const json& message) {
    const auto type = message.value("type", "");
    if (type == "hello") {
        if (message.value("settings", json::object()).contains("voice_proximity") ||
            message.value("settings", json::object()).contains("voice_proximity_range"))
            voiceSettingsOnRoomChannel = true;
        if (message.value("protocol_version", 0) != 5 || message.contains("password"))
            throw std::runtime_error("wrong cloud protocol");
        if (!proofKey) {
            const auto verifier = auth::unhex_digest(message.value("code_verifier", ""));
            if (!verifier) throw std::runtime_error("missing code verifier");
            proofKey = auth::room_proof_key(*verifier, authSalt);
        }
        if (!proofKey || !auth::equal(message.value("code_proof", ""),
            auth::room_proof(*proofKey, channel.nonce)))
            throw std::runtime_error("invalid lobby proof");
        if (message.contains("resume_token")) {
            const auto id = message.value("resume_client_id", "");
            auto old = suspended.find(id);
            if (!resumeEnabled || old == suspended.end() ||
                old->second.value("token", "") != message.value("resume_token", "")) {
                channel.push({{"type", "error"}, {"error", "resume_rejected"}}); return;
            }
            channel.id = id;
            channel.name = old->second.value("name", "");
            json peers = json::array();
            for (const auto& [peerId, other] : members)
                peers.push_back({{"client_id", peerId}, {"name", other->name}});
            for (const auto& [peerId, info] : suspended)
                if (peerId != id) peers.push_back({{"client_id", peerId}, {"name", info.at("name")}});
            suspended.erase(old);
            members[id] = &channel;
            json reply = {{"type", "room_resumed"}, {"protocol_version", 5},
                {"client_id", id}, {"owner_client_id", owner}, {"peers", peers},
                {"mesh_key", "fe28b9c93a34c10c697a962e10c70bd32a491f83a6c42d85ef317789663340bf"},
                {"settings", {{"dummy_model", true}, {"sync_flags", true}, {"sync_world", false},
                              {"remote_collision", true}, {"pvp", false}}},
                {"settings_generation", 0}, {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}};
            reply.update(resumePatch);
            channel.push(reply);
            return;
        }
        channel.id = "client_" + std::to_string(nextId++);
        channel.name = message.value("name", "Player");
        if (owner.empty()) owner = channel.id;
        json peers = json::array();
        for (const auto& [id, other] : members)
            peers.push_back({{"client_id", id}, {"name", other->name}, {"want_puppet", true}});
        members[channel.id] = &channel;
        channel.token = resumeEnabled ? auth::hex(auth::lobby_verifier(channel.id, "test", "")) : "";
        channel.push({{"type", "welcome"}, {"protocol_version", 5},
            {"resume_token", channel.token},
            {"mesh_key", "fe28b9c93a34c10c697a962e10c70bd32a491f83a6c42d85ef317789663340bf"},
            {"client_id", channel.id},
            {"owner_client_id", owner}, {"peers", peers},
            {"settings", {{"dummy_model", true}, {"sync_flags", true},
                          {"sync_world", false}, {"remote_collision", true}, {"pvp", false}}},
            {"settings_generation", 0}, {"settings_pending", false},
            {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
        for (const auto& [id, other] : members)
            if (other != &channel && other->name != dropJoinNotificationTo)
                other->push({{"type", "peer_joined"}, {"client_id", channel.id},
                    {"name", channel.name}, {"want_puppet", true},
                    {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
    } else if (type == "ice_signal") {
        if (!forwardIce) return;
        auto it = members.find(message.value("target_client_id", ""));
        if (it != members.end()) it->second->push({{"type", "ice_signal"},
            {"client_id", channel.id}, {"kind", message.at("kind")},
            {"data", message.at("data")}, {"generation", message.at("generation")}});
    } else if (type == "kick") {
        ++kickRequests;
        auto it = members.find(message.value("target_client_id", ""));
        if (it != members.end() && it->second != &channel) {
            it->second->push({{"type", "kicked"}, {"reason", "removed_by_host"}});
            it->second->close();
        }
    } else if (type == "room_settings" || type == "settings_ready") {
        if (type == "room_settings") ++roomSettingsMessages;
        if (type == "room_settings" &&
            (message.value("settings", json::object()).contains("voice_proximity") ||
             message.value("settings", json::object()).contains("voice_proximity_range")))
            voiceSettingsOnRoomChannel = true;
        // Covered by the Worker protocol test. This broker only needs to
        // exercise the real native ICE/gameplay path.
    } else {
        gameplayForwardingAttempt = true;
    }
}

void Broker::leave(Channel& channel) {
    if (channel.id.empty()) return;
    members.erase(channel.id);
    for (const auto& [id, other] : members)
        other->push({{"type", "peer_left"}, {"client_id", channel.id},
            {"semantic_visuals_ready", true}, {"snapshot_deltas_ready", true}});
    if (channel.id == owner) {
        owner = members.empty() ? "" : members.begin()->first;
        for (const auto& [id, other] : members)
            other->push({{"type", "owner_changed"}, {"owner_client_id", owner}});
    }
}

[[noreturn]] void fail(const char* reason) {
    std::cerr << "cloud transport test failed: " << reason << '\n';
    std::exit(1);
}

void check_security(const CloudRoomConfig& configuration) {
    for (const json& hello : std::vector<json>{
        {{"type", "auth_challenge"}, {"nonce", auth::random_nonce()}, {"salt", auth::random_nonce()}},
        {{"type", "secure_hello"}, {"version", 0}, {"public_key", std::string(64, '1')}},
        {{"type", "secure_hello"}, {"version", 1}, {"public_key", std::string(64, '0')}},
        {{"type", "secure_hello"}, {"version", 1}, {"public_key", "invalid"}},
        json::array(), nullptr}) {
        Broker broker;
        Transport transport;
        auto channel = std::make_unique<Channel>(broker);
        auto* wire = channel.get();
        if (!transport.start_cloud_room(configuration, std::move(channel))) fail("security test open");
        wire->events.back().text = hello.dump();
        transport.tick();
        if (transport.status().state != State::Disconnected || transport.status().welcomed ||
            !broker.wires.empty()) fail("invalid handshake leaked credentials or was accepted");
    }
    for (const auto attack : {Broker::Attack::Plaintext, Broker::Attack::Tamper, Broker::Attack::Replay}) {
        Broker broker;
        broker.attack = attack;
        Transport transport;
        if (!transport.start_cloud_room(configuration, std::make_unique<Channel>(broker)))
            fail("encrypted rejection test open");
        transport.tick();
        if (transport.status().state != State::Disconnected || transport.status().welcomed ||
            transport.status().error != "Invalid encrypted cloud room message")
            fail("invalid encrypted service response was accepted");
        for (const auto& message : broker.wires)
            if (!message.starts_with("DCR1:") && json::parse(message).value("type", "") != "secure_ready")
                fail("client fell back to plaintext");
    }
    Broker broker;
    Transport transport;
    auto channel = std::make_unique<Channel>(broker);
    auto* wire = channel.get();
    if (!transport.start_cloud_room(configuration, std::move(channel))) fail("reconnect test open");
    transport.tick();
    if (!transport.status().welcomed) fail("encrypted welcome");
    const auto oldPublic = wire->cipher->public_key();
    const auto oldWelcome = broker.wires.back();
    wire->events.push_back({RoomChannel::EventKind::Closed, "test reconnect"});
    transport.tick();
    bool sawFailureTiming = false, sawCloseReason = false;
    while (transport.has_events()) {
        const auto event = transport.pop_event();
        if (event.kind != EventKind::Diagnostic) continue;
        sawFailureTiming |= event.detail.starts_with("room_session_failed uptime_ms=") &&
                            event.detail.find("reason=test reconnect") != std::string::npos;
        sawCloseReason |= event.detail.starts_with("room_websocket_closed");
    }
    if (!sawFailureTiming || !sawCloseReason) fail("disconnect lost its cause or timing diagnostics");
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    for (int n = 0; n < 40 && !transport.status().welcomed; ++n) transport.tick();
    if (!transport.status().welcomed || wire->cipher->public_key() == oldPublic)
        fail("reconnect did not establish fresh encryption");
    bool sawReconnectTiming = false;
    while (transport.has_events()) {
        const auto event = transport.pop_event();
        const std::string prefix = "room_reconnected downtime_ms=";
        if (event.kind == EventKind::Diagnostic && event.detail.starts_with(prefix)) {
            sawReconnectTiming = true;
            if (std::stoll(event.detail.substr(prefix.size())) < 15)
                fail("reconnect timing did not span the interruption");
        }
    }
    if (!sawReconnectTiming) fail("reconnect did not report its actual duration");
    wire->events.push_back({RoomChannel::EventKind::Message, oldWelcome});
    transport.tick();
    if (transport.status().welcomed || transport.status().state != State::Disconnected)
        fail("reconnect accepted old session ciphertext");
}

void check_room_recovery(CloudRoomConfig configuration) {
    Broker broker;
    broker.resumeEnabled = true;
    Transport host, guest;
    auto a = std::make_unique<Channel>(broker), b = std::make_unique<Channel>(broker);
    auto* hostWire = a.get(); auto* guestWire = b.get();
    configuration.createRoom = true; configuration.name = "Recovery Host";
    if (!host.start_cloud_room(configuration, std::move(a))) fail("recovery host start");
    configuration.createRoom = false; configuration.name = "Recovery Guest";
    if (!guest.start_cloud_room(configuration, std::move(b))) fail("recovery guest start");
    auto tick = [&] { host.tick(); guest.tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline &&
           (host.status().natPeerCount != 1 || guest.status().natPeerCount != 1)) tick();
    if (host.status().natPeerCount != 1 || guest.status().natPeerCount != 1) fail("recovery ICE setup");
    const auto hostId = host.status().clientId, guestId = guest.status().clientId;
    uint64_t hostEpoch = 0, guestEpoch = 0;
    while (host.has_events()) hostEpoch = host.pop_event().ingress.epoch;
    while (guest.has_events()) guestEpoch = guest.pop_event().ingress.epoch;
    const auto publicKey = hostWire->cipher->public_key();
    hostWire->drop(); guestWire->drop(); tick();
    hostWire->holdPoll = guestWire->holdPoll = true;
    int hostReceived = 0, guestReceived = 0, resumes = 0;
    auto drain = [&](Transport& transport, uint64_t epoch, int& received) {
        while (transport.has_events()) {
            const auto event = transport.pop_event();
            if (event.kind == EventKind::Disconnected || event.kind == EventKind::Connected ||
                event.kind == EventKind::PeerLeft || event.kind == EventKind::PeerJoined)
                fail("room-only recovery produced a gameplay/session notification");
            if (event.ingress.epoch != epoch) fail("room recovery changed gameplay epoch");
            if (event.kind == EventKind::Diagnostic && event.detail.starts_with("room_recovery_completed")) ++resumes;
            if (event.message.is_object() && event.message.value("type", "") == "chat" &&
                event.message.contains("recovery_sequence")) {
                if (event.message.at("recovery_sequence").get<int>() != received++)
                    fail("gameplay was lost, reordered or duplicated during recovery");
            }
        }
    };
    // Both room sockets are unavailable while real ICE/KCP carries ordered
    // gameplay in both directions. Deliberately leave journals unacknowledged
    // across the boundary instead of testing only an idle established route.
    for (int n = 0; n < 100; ++n) {
        const json payload = {{"type", "chat"}, {"text", std::string(200, 'x')}, {"recovery_sequence", n}};
        if (!host.send(payload) || !guest.send(payload)) fail("gameplay refused during room recovery");
        tick();
        drain(host, hostEpoch, hostReceived); drain(guest, guestEpoch, guestReceived);
    }
    if (hostReceived == 0 || guestReceived == 0) fail("gameplay stopped while sockets were unavailable");
    hostWire->holdPoll = guestWire->holdPoll = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && (hostReceived != 100 || guestReceived != 100 || resumes != 2)) {
        tick(); drain(host, hostEpoch, hostReceived); drain(guest, guestEpoch, guestReceived);
    }
    if (hostReceived != 100 || guestReceived != 100 || resumes != 2 ||
        host.status().clientId != hostId || guest.status().clientId != guestId ||
        publicKey == hostWire->cipher->public_key()) {
        std::cerr << "recovery hostReceived=" << hostReceived << " guestReceived=" << guestReceived
                  << " resumes=" << resumes << " host=" << host.status().error << " guest=" << guest.status().error << '\n';
        fail("recovery did not preserve session with fresh room encryption");
    }
    if (broker.gameplayForwardingAttempt) fail("recovery forwarded gameplay through Cloudflare");

    // A second outage must use the same recovery identity but another fresh
    // encrypted room channel; no accumulating socket/session state.
    hostWire->drop();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && resumes != 3) {
        tick(); drain(host, hostEpoch, hostReceived); drain(guest, guestEpoch, guestReceived);
    }
    if (resumes != 3 || host.status().clientId != hostId) fail("repeated room recovery failed");

    // A changed authoritative settings generation must never be silently
    // accepted while keeping old gameplay queues and decoder state.
    broker.resumePatch = {{"settings_generation", 1}};
    hostWire->drop();
    bool failedVisibly = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !failedVisibly) {
        tick();
        while (host.has_events()) failedVisibly |= host.pop_event().kind == EventKind::Disconnected;
    }
    if (!failedVisibly) fail("unsafe recovery did not produce a visible full reconnect");
    host.disconnect(); guest.disconnect();

    // Even a responsive gameplay transport cannot keep a stale room lease
    // forever. This also exercises repeated attempts without resetting time.
    Broker timeoutBroker; timeoutBroker.resumeEnabled = true;
    Transport lonely;
    auto c = std::make_unique<Channel>(timeoutBroker); auto* wire = c.get();
    configuration.createRoom = true;
    if (!lonely.start_cloud_room(configuration, std::move(c))) fail("timeout setup");
    lonely.tick();
    while (lonely.has_events()) lonely.pop_event();
    wire->drop(); lonely.tick(); wire->holdPoll = true;
    failedVisibly = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && !failedVisibly) {
        lonely.tick();
        while (lonely.has_events()) failedVisibly |= lonely.pop_event().kind == EventKind::Disconnected;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!failedVisibly) fail("room recovery did not expire");
    std::cout << "room recovery: ordered gameplay preserved, identity stable, unsafe/expired recovery visible\n";
}

}  // namespace

int main() {
    Broker broker;
    Transport host;
    Transport guest;
    CloudRoomConfig configuration;
    configuration.name = "Host";
    configuration.room = "Test Lobby";
    configuration.lobbyCode = "secret-code";
    configuration.serverUrl = "https://rooms.example.test";
    configuration.stunHost.clear(); // host candidates suffice on one machine
    configuration.stunPort = 0;
    configuration.createRoom = true;
    configuration.settings.voiceProximity = false;
    configuration.settings.voiceProximityRange = 125;
    check_security(configuration);
    check_room_recovery(configuration);
    std::string error;
    if (!host.start_cloud_room(configuration, std::make_unique<Channel>(broker), &error))
        fail(error.c_str());
    configuration.createRoom = false;
    configuration.name = "Guest";
    configuration.settings.voiceProximity = true;
    configuration.settings.voiceProximityRange = 50;
    if (!guest.start_cloud_room(configuration, std::make_unique<Channel>(broker), &error))
        fail(error.c_str());
    for (const auto& url : broker.urls)
        if (url != "wss://rooms.example.test/room/v2/" + cloud_room_route(configuration.room))
            fail("lobby name was not replaced with an opaque route");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    bool sent = false, delivered = false, sawIceTx = false, sawIceRx = false;
    while (std::chrono::steady_clock::now() < deadline &&
           !(delivered && host.status().natPeerCount == 1 &&
             guest.status().natPeerCount == 1 && guest.status().voiceSettingsReady)) {
        host.tick();
        guest.tick();
        if (host.status().welcomed && guest.status().welcomed &&
            host.peers().size() == 1 && !sent) {
            if (!host.send({{"type", "chat"}, {"text", "direct only"}}))
                fail("gameplay send was not buffered before direct ICE connection");
            sent = true;
        }
        while (guest.has_events()) {
            const auto event = guest.pop_event();
            if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_tx kind=0"))
                sawIceTx = true;
            if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_rx kind=0") &&
                event.detail.find("accepted=yes") != std::string::npos)
                sawIceRx = true;
            if (event.message.is_object() && event.message.value("type", "") == "chat" &&
                event.message.value("text", "") == "direct only") delivered = true;
        }
        if (broker.gameplayForwardingAttempt) fail("gameplay was sent to the room channel");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!sent || !delivered || !sawIceTx || !sawIceRx || host.status().natPeerCount != 1 ||
        guest.status().natPeerCount != 1 || host.status().relayPeerCount != 0) {
        std::cerr << "sent=" << sent << " delivered=" << delivered
                  << " host_nat=" << host.status().natPeerCount
                  << " guest_nat=" << guest.status().natPeerCount
                  << " host_error=" << host.status().error
                  << " guest_error=" << guest.status().error << '\n';
        fail("direct-only ICE did not deliver the buffered gameplay message");
    }
    std::mt19937 random(42);
    std::string largeText(16000, ' ');
    for (char& c : largeText) c = static_cast<char>('!' + random() % 90);
    const json largeMessage = {{"type", "chat"}, {"text", largeText}};
    if (encode_reliable_json(largeMessage).size() <= kIceDatagramBytes)
        fail("large ICE regression message did not need fragmentation");
    if (!host.send(largeMessage)) fail("large reliable peer message did not queue");
    bool largeDelivered = false;
    const auto largeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < largeDeadline && !largeDelivered) {
        host.tick(); guest.tick();
        while (guest.has_events()) {
            const auto event = guest.pop_event();
            largeDelivered |= event.message.is_object() &&
                event.message.value("type", "") == "chat" &&
                event.message.value("text", "") == largeText;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!largeDelivered || !host.status().error.empty() || !guest.status().error.empty())
        fail("large reliable message stalled on encrypted ICE");
    if (!guest.status().voiceSettingsReady ||
        guest.status().settings.voiceProximity ||
        guest.status().settings.voiceProximityRange != 125) {
        fail("host proximity settings did not arrive over the peer connection");
    }
    const std::string hostId = host.status().clientId;
    const std::string guestId = guest.status().clientId;
    if (!guest.send({{"type", "trade_item"}, {"phase", 0}, {"item", 0x80}}))
        fail("trade item did not use the peer connection");
    bool tradeDelivered = false;
    const auto tradeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < tradeDeadline && !tradeDelivered) {
        host.tick(); guest.tick();
        while (host.has_events()) {
            const auto event = host.pop_event();
            if (event.message.is_object() && event.message.value("type", "") == "trade_item" &&
                event.message.value("phase", -1) == 0 &&
                event.message.value("item", -1) == 0x80)
                tradeDelivered = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!tradeDelivered || broker.gameplayForwardingAttempt ||
        host.status().clientId != hostId || guest.status().clientId != guestId ||
        !host.status().error.empty() || !guest.status().error.empty())
        fail("trade item was not delivered peer-to-peer without reconnecting");
    if (!guest.send({{"type", "voice_settings"}, {"enabled", true},
                     {"range_percent", 1}})) fail("guest spoof send failed");
    for (int i = 0; i < 60; ++i) { host.tick(); guest.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    if (host.status().settings.voiceProximity ||
        host.status().settings.voiceProximityRange != 125)
        fail("non-owner changed peer voice settings");
    if (!host.publish_voice_settings(true, 75)) fail("host peer voice update failed");
    if (!host.publish_save_recovery_setting(false))
        fail("host peer save recovery update failed");
    const auto voiceDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < voiceDeadline &&
           (!guest.status().settings.voiceProximity ||
            guest.status().settings.voiceProximityRange != 75 ||
            !guest.status().saveRecoverySettingsReady ||
            guest.status().settings.saveRecovery)) {
        host.tick(); guest.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!guest.status().settings.voiceProximity ||
        guest.status().settings.voiceProximityRange != 75 ||
        !guest.status().saveRecoverySettingsReady ||
        guest.status().settings.saveRecovery ||
        broker.gameplayForwardingAttempt || broker.voiceSettingsOnRoomChannel ||
        broker.roomSettingsMessages != 0)
        fail("host peer settings did not stay on the peer path");
    Transport late;
    configuration.name = "Late";
    configuration.settings.voiceProximity = false;
    configuration.settings.voiceProximityRange = 50;
    if (!late.start_cloud_room(configuration, std::make_unique<Channel>(broker), &error))
        fail(error.c_str());
    const auto lateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < lateDeadline &&
           (!late.status().voiceSettingsReady || !late.status().saveRecoverySettingsReady ||
            late.status().natPeerCount != 2)) {
        host.tick(); guest.tick(); late.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!late.status().voiceSettingsReady || !late.status().settings.voiceProximity ||
        late.status().settings.voiceProximityRange != 75 ||
        !late.status().saveRecoverySettingsReady || late.status().settings.saveRecovery ||
        broker.voiceSettingsOnRoomChannel || broker.roomSettingsMessages != 0)
        fail("late joiner did not receive host peer settings");
    const std::string lateId = late.status().clientId;
    if (!guest.send_test_peer_message(hostId,
            {{"type", "cloud_join_failure"}, {"unreachable_client_id", lateId}}))
        fail("could not simulate a forged newcomer failure report");
    bool ignoredReport = false;
    const auto forgedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < forgedDeadline && !ignoredReport) {
        host.tick(); guest.tick(); late.tick();
        while (host.has_events()) {
            const auto event = host.pop_event();
            ignoredReport |= event.kind == EventKind::Diagnostic &&
                event.detail == "ignored_untrusted_newcomer_failure";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!ignoredReport || broker.kickRequests != 0 || !broker.members.contains(lateId) ||
        host.status().natPeerCount != 2 || guest.status().natPeerCount != 2 ||
        late.status().natPeerCount != 2)
        fail("guest report evicted a fully connected newcomer");
    host.disconnect();
    guest.tick(); late.tick();
    if (!guest.status().isOwner) fail("remaining peer did not inherit room ownership");
    if (guest.status().settings.saveRecovery)
        fail("new owner discarded the previous save recovery setting");
    if (!guest.publish_voice_settings(false, 90)) fail("new owner voice update failed");
    if (!guest.publish_save_recovery_setting(true)) fail("new owner save recovery update failed");
    const auto transferDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < transferDeadline &&
           (late.status().settings.voiceProximity ||
            late.status().settings.voiceProximityRange != 90 ||
            !late.status().settings.saveRecovery)) {
        guest.tick(); late.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (late.status().settings.voiceProximity ||
        late.status().settings.voiceProximityRange != 90 ||
        !late.status().settings.saveRecovery ||
        broker.voiceSettingsOnRoomChannel || broker.roomSettingsMessages != 0)
        fail("transferred host peer settings did not stay on the peer path");
    late.disconnect();
    guest.disconnect();

    // A joined peer can receive authenticated ICE even if its peer-joined
    // notification was lost. It must recover the link and reply, not discard
    // the first description as an unknown peer.
    {
        Broker missedJoinBroker;
        missedJoinBroker.dropJoinNotificationTo = "Host";
        Transport waitingHost;
        Transport arrivingGuest;
        configuration.createRoom = true;
        configuration.name = "Host";
        if (!waitingHost.start_cloud_room(configuration,
                                          std::make_unique<Channel>(missedJoinBroker), &error))
            fail(error.c_str());
        configuration.createRoom = false;
        configuration.name = "Guest";
        if (!arrivingGuest.start_cloud_room(configuration,
                                            std::make_unique<Channel>(missedJoinBroker), &error))
            fail(error.c_str());
        const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool recovered = false;
        while (std::chrono::steady_clock::now() < recoveryDeadline) {
            waitingHost.tick();
            arrivingGuest.tick();
            recovered = waitingHost.status().natPeerCount == 1 &&
                        arrivingGuest.status().natPeerCount == 1 &&
                        waitingHost.peers().size() == 1;
            if (recovered) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!recovered || missedJoinBroker.gameplayForwardingAttempt)
            fail("missed peer-joined notification did not recover through ICE");
        const std::string departedId = arrivingGuest.status().clientId;
        arrivingGuest.disconnect();
        waitingHost.tick();
        auto* hostChannel = missedJoinBroker.members.at(waitingHost.status().clientId);
        hostChannel->push({{"type", "ice_signal"}, {"client_id", departedId},
                           {"kind", 0}, {"data", "late description"}, {"generation", 0}});
        waitingHost.tick();
        if (!waitingHost.peers().empty())
            fail("late ICE from a departed peer recreated a ghost link");
        waitingHost.disconnect();
    }

    // Grow an established room rather than admitting everyone at once. Each
    // player must have a direct route to every other player, and gameplay must
    // reach every recipient without passing through the room service.
    for (size_t playerCount : {size_t{3}, size_t{4}}) {
        Broker meshBroker;
        std::vector<std::unique_ptr<Transport>> players;
        for (size_t index = 0; index < playerCount; ++index) {
            auto player = std::make_unique<Transport>();
            configuration.createRoom = index == 0;
            configuration.name = "Player " + std::to_string(index);
            if (!player->start_cloud_room(configuration, std::make_unique<Channel>(meshBroker), &error))
                fail(error.c_str());
            players.push_back(std::move(player));

            const auto connectedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
            bool connected = false;
            while (std::chrono::steady_clock::now() < connectedDeadline) {
                for (auto& active : players) active->tick();
                connected = true;
                for (const auto& active : players) {
                    if (!active->status().error.empty()) {
                        std::cerr << "players=" << players.size() << " error=" << active->status().error << '\n';
                        fail("cloud mesh participant disconnected during admission");
                    }
                    connected &= active->status().welcomed &&
                        active->status().natPeerCount == players.size() - 1;
                }
                if (connected) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!connected) {
                for (size_t peer = 0; peer < players.size(); ++peer)
                    std::cerr << "players=" << players.size() << " peer=" << peer
                              << " nat=" << players[peer]->status().natPeerCount
                              << " error=" << players[peer]->status().error << '\n';
                fail("cloud mesh did not fully connect");
            }
        }

        for (size_t sender = 0; sender < players.size(); ++sender)
            if (!players[sender]->send({{"type", "chat"}, {"text", "mesh-" + std::to_string(sender)}}))
                fail("cloud mesh could not queue gameplay");
        std::vector<std::vector<bool>> received(players.size(), std::vector<bool>(players.size()));
        const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool allDelivered = false;
        while (std::chrono::steady_clock::now() < deliveryDeadline) {
            for (size_t recipient = 0; recipient < players.size(); ++recipient) {
                players[recipient]->tick();
                while (players[recipient]->has_events()) {
                    const auto event = players[recipient]->pop_event();
                    if (!event.message.is_object() || event.message.value("type", "") != "chat") continue;
                    for (size_t sender = 0; sender < players.size(); ++sender)
                        if (event.message.value("text", "") == "mesh-" + std::to_string(sender))
                            received[recipient][sender] = true;
                }
            }
            allDelivered = true;
            for (size_t recipient = 0; recipient < players.size(); ++recipient)
                for (size_t sender = 0; sender < players.size(); ++sender)
                    if (recipient != sender) allDelivered &= received[recipient][sender];
            if (allDelivered) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!allDelivered || meshBroker.gameplayForwardingAttempt)
            fail("cloud mesh failed all-to-all direct gameplay delivery");

        // Reuse the same transport after a player leaves and rejoins an
        // established room. Existing peers must admit the new identity and
        // complete a fresh ICE exchange with the returnee.
        const size_t returning = playerCount - 1;
        players[returning]->disconnect();
        const auto departureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool departed = false;
        while (std::chrono::steady_clock::now() < departureDeadline) {
            for (auto& active : players) active->tick();
            departed = true;
            for (size_t peer = 0; peer < returning; ++peer)
                departed &= players[peer]->peers().size() == playerCount - 2;
            if (departed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!departed) fail("remaining players did not observe cloud peer departure");
        configuration.createRoom = false;
        configuration.name = "Returning Player";
        if (!players[returning]->start_cloud_room(
                configuration, std::make_unique<Channel>(meshBroker), &error))
            fail(error.c_str());
        const auto rejoinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        bool rejoined = false;
        while (std::chrono::steady_clock::now() < rejoinDeadline) {
            for (auto& active : players) active->tick();
            rejoined = true;
            for (const auto& active : players)
                rejoined &= active->status().welcomed &&
                    active->status().natPeerCount == playerCount - 1 &&
                    active->status().error.empty();
            if (rejoined) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!rejoined) fail("returning cloud player did not reconnect to every existing peer");
        if (!players[returning]->send({{"type", "chat"}, {"text", "after-rejoin"}}))
            fail("returning cloud player could not send gameplay");
        std::vector<bool> rejoinReceived(returning);
        const auto rejoinDeliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < rejoinDeliveryDeadline) {
            for (size_t peer = 0; peer < playerCount; ++peer) {
                players[peer]->tick();
                while (players[peer]->has_events()) {
                    const auto event = players[peer]->pop_event();
                    if (peer < returning && event.message.is_object() &&
                        event.message.value("type", "") == "chat" &&
                        event.message.value("text", "") == "after-rejoin")
                        rejoinReceived[peer] = true;
                }
            }
            if (std::all_of(rejoinReceived.begin(), rejoinReceived.end(), [](bool received) { return received; }))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!std::all_of(rejoinReceived.begin(), rejoinReceived.end(), [](bool received) { return received; }))
            fail("returning cloud player's gameplay was not delivered to all existing peers");
        for (auto& player : players) player->disconnect();
    }

    // A stalled late joiner must be removed without disconnecting players who
    // already have a healthy direct link to each other.
    {
        Broker admissionBroker;
        Transport establishedHost;
        Transport establishedGuest;
        Transport stalledJoiner;
        configuration.createRoom = true;
        configuration.name = "Established Host";
        if (!establishedHost.start_cloud_room(configuration,
                                             std::make_unique<Channel>(admissionBroker), &error))
            fail(error.c_str());
        configuration.createRoom = false;
        configuration.name = "Established Guest";
        if (!establishedGuest.start_cloud_room(configuration,
                                              std::make_unique<Channel>(admissionBroker), &error))
            fail(error.c_str());
        const auto stableDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < stableDeadline &&
               (establishedHost.status().natPeerCount != 1 ||
                establishedGuest.status().natPeerCount != 1)) {
            establishedHost.tick(); establishedGuest.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (establishedHost.status().natPeerCount != 1 ||
            establishedGuest.status().natPeerCount != 1)
            fail("established cloud players did not connect before admission test");
        const std::string hostId = establishedHost.status().clientId;
        const std::string guestId = establishedGuest.status().clientId;

        admissionBroker.forwardIce = false;
        configuration.name = "Stalled Joiner";
        if (!stalledJoiner.start_cloud_room(configuration,
                                           std::make_unique<Channel>(admissionBroker), &error))
            fail(error.c_str());
        stalledJoiner.tick(); // Join the room, then stop servicing the newcomer's ICE.
        const auto removalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(24);
        bool removed = false;
        while (std::chrono::steady_clock::now() < removalDeadline) {
            establishedHost.tick(); establishedGuest.tick();
            if (!establishedHost.status().error.empty() ||
                !establishedGuest.status().error.empty())
                fail("unreachable newcomer disconnected an established cloud player");
            removed = admissionBroker.kickRequests > 0 &&
                establishedHost.peers().size() == 1 && establishedGuest.peers().size() == 1;
            if (removed) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!removed || establishedHost.status().clientId != hostId ||
            establishedGuest.status().clientId != guestId ||
            establishedHost.status().natPeerCount != 1 ||
            establishedGuest.status().natPeerCount != 1)
            fail("unreachable newcomer was not removed while established peers stayed connected");
        if (!establishedHost.send({{"type", "chat"}, {"text", "still connected"}}))
            fail("established peer could not send after newcomer removal");
        bool stillDelivered = false;
        const auto messageDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < messageDeadline && !stillDelivered) {
            establishedHost.tick(); establishedGuest.tick();
            while (establishedGuest.has_events()) {
                const auto event = establishedGuest.pop_event();
                stillDelivered |= event.message.is_object() &&
                    event.message.value("type", "") == "chat" &&
                    event.message.value("text", "") == "still connected";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!stillDelivered || admissionBroker.gameplayForwardingAttempt)
            fail("established peer gameplay stopped after newcomer removal");
        stalledJoiner.disconnect();
        establishedGuest.disconnect();
        establishedHost.disconnect();
    }

    // A room-service membership is not a peer connection. Without signaling,
    // the Cloud Room must fail promptly and explain which ICE step stalled.
    Broker stalled;
    stalled.forwardIce = false;
    Transport stalledHost;
    Transport stalledGuest;
    configuration.createRoom = true;
    if (!stalledHost.start_cloud_room(configuration, std::make_unique<Channel>(stalled), &error))
        fail(error.c_str());
    configuration.createRoom = false;
    if (!stalledGuest.start_cloud_room(configuration, std::make_unique<Channel>(stalled), &error))
        fail(error.c_str());
    bool timedOut = false, sawTimeoutDiagnostic = false;
    const auto stalledDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    while (std::chrono::steady_clock::now() < stalledDeadline && !timedOut) {
        stalledHost.tick();
        stalledGuest.tick();
        for (Transport* side : {&stalledHost, &stalledGuest}) {
            while (side->has_events()) {
                const auto event = side->pop_event();
                if (event.kind == EventKind::Diagnostic && event.detail.starts_with("ice_timeout"))
                    sawTimeoutDiagnostic = true;
            }
            timedOut |= side->status().error.find("Direct peer connection timed out after 15 seconds.") !=
                std::string::npos;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!timedOut || !sawTimeoutDiagnostic || stalled.gameplayForwardingAttempt)
        fail("stalled Cloud Room ICE did not fail with diagnostics at the 15-second deadline");
    stalledGuest.disconnect();
    stalledHost.disconnect();
    std::cout << "cloud transport test passed\n";
}
