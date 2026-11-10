#include "dusklight_online/net/transport.hpp"
#include "dusklight_online/game/pose_playback.hpp"
#include "cave_map_packet.hpp"

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using dusklight_online::net::DirectHostConfig;
using dusklight_online::net::DirectJoinConfig;
using dusklight_online::net::EventKind;
using dusklight_online::net::Transport;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "transport test failed: " << message << '\n';
    std::exit(1);
}

uint16_t reserve_test_port() {
    const auto socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#if defined(_WIN32)
    if (socketHandle == INVALID_SOCKET) {
#else
    if (socketHandle < 0) {
#endif
        fail("could not create test socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        fail("could not reserve test port");
    }
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        fail("could not read test port");
    }
    const uint16_t port = ntohs(address.sin_port);
#if defined(_WIN32)
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
    return port;
}

bool drain_for_type(Transport& transport, const std::string& type,
                    nlohmann::json* found = nullptr) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.message.is_object() && event.message.value("type", "") == type) {
            matched = true;
            if (found != nullptr) {
                *found = std::move(event.message);
            }
        }
    }
    return matched;
}

bool drain_udp_for_sequence(Transport& transport, uint32_t sequence,
                            nlohmann::json* found = nullptr) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.kind == EventKind::UdpMessage &&
            event.message.value("sequence", 0U) == sequence) {
            matched = true;
            if (found != nullptr) {
                *found = std::move(event.message);
            }
        }
    }
    return matched;
}

bool drain_udp_object(Transport& transport, int32_t objectId) {
    bool matched = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.kind == EventKind::UdpRemoteObject &&
            event.remoteObject.objectId == objectId) {
            matched = true;
        }
    }
    return matched;
}

void pump(Transport& host, Transport& first, Transport& second, int ticks = 300) {
    for (int i = 0; i < ticks; ++i) {
        host.tick();
        first.tick();
        second.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

int main() {
    constexpr dusklight_online::net::RoomSettings defaults{};
    static_assert(defaults.dummyModel && defaults.syncFlags && defaults.saveRecovery &&
                  !defaults.syncWorld &&
                  defaults.remoteCollision && defaults.pvp &&
                  defaults.voiceProximity && defaults.voiceProximityRange == 50);
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fail("WSAStartup failed");
    }
#endif

    const uint16_t port = reserve_test_port();
    Transport host;
    Transport alice;
    Transport bob;

    DirectHostConfig hostConfig;
    hostConfig.name = "Host";
    hostConfig.room = "transport-test";
    hostConfig.bindHost = "127.0.0.1";
    hostConfig.publicHost = "127.0.0.1";
    hostConfig.port = port;
    hostConfig.sessionKey = "exact-test-invite-key";
    hostConfig.settings.syncWorld = true;
    hostConfig.settings.pvp = true;
    hostConfig.settings.voiceProximity = false;
    hostConfig.settings.voiceProximityRange = 125;

    std::string error;
    {
        Transport failedHost;
        auto invalidHost = hostConfig;
        invalidHost.bindHost = "256.0.0.1";
        if (failedHost.start_direct_host(invalidHost, &error) || failedHost.status().enabled) {
            fail("failed direct host remained enabled");
        }
        Transport failedJoin;
        DirectJoinConfig unavailable;
        unavailable.host = "256.0.0.1";
        unavailable.port = port;
        unavailable.sessionKey = hostConfig.sessionKey;
        if (failedJoin.start_direct_join(unavailable, &error) || failedJoin.status().enabled) {
            fail("synchronous join failure remained enabled");
        }
        unavailable.host = "127.0.0.1";
        if (!failedJoin.start_direct_join(unavailable, &error)) {
            fail("unavailable join did not start asynchronously");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
        while (failedJoin.status().enabled && std::chrono::steady_clock::now() < deadline) {
            failedJoin.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (failedJoin.status().enabled || failedJoin.status().reconnecting ||
            failedJoin.status().error.empty()) {
            fail("initial connection failure did not terminate the attempt");
        }
        bool disconnected = false;
        while (failedJoin.has_events()) {
            disconnected |= failedJoin.pop_event().kind == EventKind::Disconnected;
        }
        for (int i = 0; i < 90; ++i) failedJoin.tick();
        if (!disconnected || failedJoin.status().enabled || failedJoin.has_events()) {
            fail("failed join retried silently or omitted its disconnect event");
        }
        if (!failedJoin.start_direct_join(unavailable, &error)) fail("retry start failed");
        failedJoin.disconnect();
        for (int i = 0; i < 90; ++i) failedJoin.tick();
        if (failedJoin.status().enabled || failedJoin.status().reconnecting) {
            fail("cancelled attempt resumed");
        }
    }
    if (!host.start_direct_host(hostConfig, &error)) {
        fail("host start: " + error);
    }

    {
        Transport intruder;
        DirectJoinConfig wrongKey;
        wrongKey.name = "Wrong key";
        wrongKey.host = hostConfig.publicHost;
        wrongKey.port = port;
        wrongKey.sessionKey = "different-invite-key";
        if (!intruder.start_direct_join(wrongKey, &error)) fail("wrong-key join start failed");
        pump(host, intruder, alice, 5400);
        if (intruder.status().welcomed || intruder.status().enabled ||
            host.status().welcomed || !host.peers().empty())
            fail("direct host admitted a wrong session key");
    }

    DirectJoinConfig aliceConfig;
    aliceConfig.name = "Alice";
    aliceConfig.room = hostConfig.room;
    aliceConfig.host = hostConfig.publicHost;
    aliceConfig.port = port;
    aliceConfig.sessionKey = hostConfig.sessionKey;
    if (!alice.start_direct_join(aliceConfig, &error)) {
        fail("Alice start: " + error);
    }

    DirectJoinConfig bobConfig = aliceConfig;
    bobConfig.name = "Bob";
    // Snapshot-delta support remains independently negotiated. Semantic
    // rendering itself is always enabled by current clients.
    bobConfig.supportsSnapshotDeltas = false;
    if (!bob.start_direct_join(bobConfig, &error)) {
        fail("Bob start: " + error);
    }

    pump(host, alice, bob);
    if (!alice.status().welcomed || !bob.status().welcomed || host.peers().size() != 2) {
        fail("three-way direct handshake did not complete");
    }
    if (!alice.status().settings.syncWorld || !alice.status().settings.pvp) {
        fail("host room settings were not applied by joiner");
    }
    if (alice.status().settings.voiceProximity ||
        alice.status().settings.voiceProximityRange != 125 ||
        bob.status().settings.voiceProximityRange != 125) {
        fail("host proximity settings were not applied by joiners");
    }
    if (!host.status().semanticVisualsReady || !alice.status().semanticVisualsReady ||
        !bob.status().semanticVisualsReady) {
        fail("current direct peers did not negotiate semantic visuals");
    }
    if (host.status().snapshotDeltasReady || alice.status().snapshotDeltasReady ||
        bob.status().snapshotDeltasReady) {
        fail("unsupported direct peer did not disable snapshot delta negotiation");
    }

    while (host.has_events()) host.pop_event();
    while (alice.has_events()) alice.pop_event();
    while (bob.has_events()) bob.pop_event();

    // Cave reveals must retain all source fields and reach both the host and
    // other joiners. Each sender produces one event per recipient, without echo.
    const auto cave = cave_map_packet();
    for (Transport* sender : {&host, &alice, &bob}) {
        if (!sender->send(cave)) fail("direct cave reveal send");
        pump(host, alice, bob, 200);
        for (Transport* recipient : {&host, &alice, &bob}) {
            int count = 0;
            while (recipient->has_events()) {
                auto event = recipient->pop_event();
                if (!event.message.is_object() || event.message.value("type", "") != "switch_bit") continue;
                if (event.message.value("client_id", "") != sender->status().clientId)
                    fail("direct cave reveal sender identity");
                event.message.erase("client_id");
                if (event.message != cave) fail("direct cave reveal source data changed");
                ++count;
            }
            if (count != (recipient == sender ? 0 : 1)) fail("direct cave reveal fanout/echo");
        }
    }

    if (!alice.send({{"type", "chat"}, {"text", "Hello\nfrom Alice"},
                     {"client_id", "spoof"}})) {
        fail("direct chat send");
    }
    pump(host, alice, bob, 40);
    for (Transport* recipient : {&host, &bob}) {
        nlohmann::json received;
        if (!drain_for_type(*recipient, "chat", &received) ||
            received.value("text", "") != "Hello\nfrom Alice" ||
            received.value("client_id", "") == "spoof") {
            fail("direct chat delivery or sender attribution");
        }
    }
    if (drain_for_type(alice, "chat")) fail("direct chat echoed to sender");

    // A direct guest must not be able to make a host-only Speedrun command
    // appear to come from the host when the transport fans out its messages.
    for (const char* type : {"speedrun_check", "speedrun_prompt",
                             "speedrun_ready_count", "speedrun_countdown",
                             "speedrun_cancel", "speedrun_start", "speedrun_reset"}) {
        if (!alice.send({{"type", type}, {"request_id", 7U}}))
            fail("guest speedrun command send");
        pump(host, alice, bob, 30);
        if (drain_for_type(host, type) || drain_for_type(bob, type))
            fail("guest speedrun command crossed the host boundary");
    }
    if (!host.send({{"type", "speedrun_check"}, {"request_id", 8U}}))
        fail("host speedrun check send");
    pump(host, alice, bob, 30);
    if (!drain_for_type(alice, "speedrun_check") ||
        !drain_for_type(bob, "speedrun_check"))
        fail("host speedrun check did not reach both players");
    if (!alice.send({{"type", "speedrun_ready"}, {"request_id", 8U},
                     {"ready", true}}))
        fail("guest speedrun readiness send");
    pump(host, alice, bob, 30);
    if (!drain_for_type(host, "speedrun_ready") ||
        drain_for_type(bob, "speedrun_ready"))
        fail("speedrun readiness must go only to the host");
    if (!host.send({{"type", "speedrun_prompt"}, {"request_id", 8U},
                    {"player_count", 3U}}))
        fail("host speedrun prompt send");
    pump(host, alice, bob, 30);
    if (!drain_for_type(alice, "speedrun_prompt") ||
        !drain_for_type(bob, "speedrun_prompt"))
        fail("host speedrun prompt did not reach both players");
    if (!alice.send({{"type", "speedrun_player_ready"}, {"request_id", 8U}}))
        fail("guest speedrun player ready send");
    pump(host, alice, bob, 30);
    if (!drain_for_type(host, "speedrun_player_ready") ||
        drain_for_type(bob, "speedrun_player_ready"))
        fail("speedrun player readiness must go only to the host");

    // Appearance is reliable presence metadata, including the empty default.
    for (const auto& [color, outfit] : {std::pair{"C06030", "204060"},
                                      std::pair{"C06030", ""}, std::pair{"", "204060"},
                                      std::pair{"", ""}}) {
        const char* zora = outfit[0] == '\0' ? "00ABCD" : "";
        alice.send({{"type", "presence"}, {"player_color", color},
                    {"outfit_color", outfit}, {"zora_color", zora}, {"client_id", "spoof"}});
        pump(host, alice, bob, 20);
        nlohmann::json appearance;
        if (!drain_for_type(bob, "presence", &appearance) ||
            appearance.value("outfit_color", "missing") != outfit ||
            appearance.value("zora_color", "missing") != zora ||
            appearance.value("player_color", "missing") != color ||
            appearance.value("client_id", "") == "spoof") fail("direct appearance forwarding");
        if (!drain_for_type(host, "presence", &appearance) ||
            appearance.value("outfit_color", "missing") != outfit ||
            appearance.value("zora_color", "missing") != zora ||
            appearance.value("player_color", "missing") != color) fail("host appearance receipt");
        host.send({{"type", "presence"}, {"player_color", color},
                   {"outfit_color", outfit}, {"zora_color", zora}});
        pump(host, alice, bob, 20);
        if (!drain_for_type(alice, "presence", &appearance) ||
            appearance.value("outfit_color", "missing") != outfit ||
            appearance.value("zora_color", "missing") != zora ||
            appearance.value("player_color", "missing") != color) fail("host appearance publication");
        while (bob.has_events()) bob.pop_event();
    }

    // Bob's first pose registers his observed UDP endpoint with the direct
    // host. Alice's following pose must then be decoded and re-encoded by the
    // host for Bob, retaining Alice's authenticated peer identity.
    if (!bob.send_visual({{"type", "pose"}, {"sequence", 1},
                          {"state", {{"stage", "F_SP103"}, {"x", 1.0f}}}})) {
        fail("Bob UDP registration pose failed");
    }
    pump(host, alice, bob, 30);
    while (host.has_events()) host.pop_event();
    while (alice.has_events()) alice.pop_event();
    while (bob.has_events()) bob.pop_event();

    if (!alice.send_visual({{"type", "pose"}, {"sequence", 2},
                            {"state", {{"stage", "F_SP103"}, {"x", 9.5f}}}})) {
        fail("Alice UDP pose send failed");
    }
    pump(host, alice, bob, 60);
    nlohmann::json udpPose;
    if (!drain_udp_for_sequence(bob, 2, &udpPose) ||
        udpPose.value("client_id", "") != alice.status().clientId ||
        udpPose.value("state", nlohmann::json::object()).value("x", 0.0f) != 9.5f) {
        fail("direct UDP pose was not routed through the host");
    }

    // The packet is encrypted by Alice's connection, but its visual header
    // claims Bob's ID. The host must reject that mismatch before decoding it.
    if (!alice.send_test_visual_as({{"type", "pose"}, {"sequence", 1000},
                                    {"state", {{"stage", "F_SP103"}, {"x", 1000.0f}}}},
                                   bob.status().clientId)) {
        fail("spoofed direct UDP pose could not be sent");
    }
    pump(host, alice, bob, 30);
    if (drain_udp_for_sequence(host, 1000) ||
        drain_udp_for_sequence(alice, 1000) ||
        drain_udp_for_sequence(bob, 1000)) {
        fail("spoofed direct UDP sender was accepted");
    }

    if (!host.send_visual({{"type", "pose"}, {"sequence", 3},
                           {"state", {{"stage", "F_SP103"},
                                      {"matrix_scope", "attachments"}}}},
                          dusklight_online::net::udp::PacketType::SemanticPoseMsgpack)) {
        fail("host semantic UDP pose send failed");
    }
    const auto semanticStats = host.last_visual_send_stats();
    if (semanticStats.sequence != 3 ||
        semanticStats.type != dusklight_online::net::udp::PacketType::SemanticPoseMsgpack ||
        semanticStats.recipients != 2 || semanticStats.datagrams == 0 ||
        semanticStats.wireBytes == 0) {
        fail("semantic UDP wire statistics did not describe all current recipients");
    }
    pump(host, alice, bob, 60);
    if (!drain_udp_for_sequence(alice, 3)) {
        fail("semantic UDP pose did not reach Alice");
    }
    if (!drain_udp_for_sequence(bob, 3)) {
        fail("semantic UDP pose did not reach Bob");
    }

    // Network processing may collect several snapshots before the game consumes
    // its event queue. Preserve that history for the playback buffer.
    for (uint32_t seq = 4; seq <= 9; ++seq) {
        if (!host.send_visual({{"type", "pose"}, {"sequence", seq},
                               {"state", {{"stage", "F_SP103"}, {"x", seq}}}},
                              dusklight_online::net::udp::PacketType::SemanticPoseMsgpack))
            fail("buffered pose send failed");
        pump(host, alice, bob, 40);
    }
    dusklight_online::game::PosePlayback<uint32_t> playback;
    unsigned count = 0;
    while (bob.has_events()) {
        auto event = bob.pop_event();
        if (event.kind != EventKind::UdpMessage) continue;
        if (event.udpSequence != 4 + count) fail("transport discarded a buffered pose");
        playback.push(event.udpSequence, event.udpSequence);
        ++count;
    }
    if (count != 6) fail("transport did not retain six playback snapshots");
    uint32_t expected = 4;
    for (unsigned tick = 0; tick < 12; ++tick) {
        if (auto seq = playback.update()) {
            if (*seq != expected++) fail("transport-to-playback sequence gap");
        }
    }
    if (expected != 10) fail("transport-to-playback lost samples");

    dusklight_online::net::udp::RemoteObjectPacket object;
    object.sequence = 3;
    object.objectId = 9001;
    object.objectKind = 1;
    object.flags = dusklight_online::net::udp::ObjectActive;
    if (!alice.send_remote_object(object)) {
        fail("Alice UDP remote-object send failed");
    }
    pump(host, alice, bob, 60);
    if (!drain_udp_object(bob, 9001)) {
        fail("direct UDP remote object was not routed through the host");
    }

    // Voice is produced in 20 ms frames while the game processes network
    // events at about 30 Hz. Exercise that burst pattern through the host.
    dusklight_online::net::udp::VoicePosition voicePosition;
    std::memcpy(voicePosition.stageName, "F_SP103", 7);
    const std::vector<uint8_t> voiceFrame{0x78, 0x12, 0x34};
    uint32_t voiceSequence = 0;
    uint32_t hostVoiceCount = 0;
    uint32_t bobVoiceCount = 0;
    for (int tick = 0; tick < 36; ++tick) {
        for (int frame = 0; frame < (tick % 3 == 0 ? 1 : 2); ++frame) {
            if (!alice.send_voice(++voiceSequence, voicePosition, voiceFrame))
                fail("direct voice packet send failed");
        }
        host.tick();
        alice.tick();
        bob.tick();
        for (auto [receiver, count] : {std::pair{&host, &hostVoiceCount},
                                       std::pair{&bob, &bobVoiceCount}}) {
            while (receiver->has_events()) {
                auto event = receiver->pop_event();
                if (event.kind != EventKind::UdpVoice) continue;
                if (event.udpSequence != ++*count || event.voice != voiceFrame)
                    fail("direct voice frame was lost or reordered");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    pump(host, alice, bob, 30);
    for (auto [receiver, count] : {std::pair{&host, &hostVoiceCount},
                                   std::pair{&bob, &bobVoiceCount}}) {
        while (receiver->has_events()) {
            auto event = receiver->pop_event();
            if (event.kind != EventKind::UdpVoice) continue;
            if (event.udpSequence != ++*count || event.voice != voiceFrame)
                fail("direct voice frame was lost or reordered");
        }
        if (*count != voiceSequence) fail("direct voice stream did not reach every listener");
    }

    const nlohmann::json eventBit = {
        {"type", "event_bit"}, {"event", 42}, {"value", true}};
    if (!alice.send(eventBit)) {
        fail("Alice gameplay send failed");
    }
    pump(host, alice, bob, 30);
    nlohmann::json received;
    if (!drain_for_type(host, "event_bit", &received) ||
        received.value("client_id", "").empty()) {
        fail("host did not receive authenticated gameplay message");
    }
    if (!drain_for_type(bob, "event_bit", &received) ||
        received.value("event", -1) != 42) {
        fail("direct host did not fan out gameplay message");
    }

    const std::string bobId = bob.status().clientId;
    if (!alice.send({{"type", "sync_request"}, {"target_client_id", bobId}})) {
        fail("targeted sync request send failed");
    }
    pump(host, alice, bob, 30);
    if (!drain_for_type(bob, "sync_request")) {
        fail("targeted direct sync request did not reach Bob");
    }
    if (drain_for_type(host, "sync_request")) {
        fail("targeted sync request leaked into host game handler");
    }

    dusklight_online::net::RoomSettings changed = host.status().settings;
    changed.remoteCollision = false;
    changed.pvp = true;
    if (!host.publish_room_settings(changed)) {
        fail("host room settings publish failed");
    }
    if (!host.publish_voice_settings(true, 75)) {
        fail("host peer voice settings publish failed");
    }
    if (!host.publish_save_recovery_setting(false)) {
        fail("host save recovery setting publish failed");
    }
    pump(host, alice, bob, 30);
    if (alice.status().settings.remoteCollision || alice.status().settings.pvp) {
        fail("joiner did not apply direct settings messages");
    }
    if (host.status().settings.pvp || bob.status().settings.pvp) {
        fail("PvP was not forced off with remote collision");
    }
    if (!alice.status().settings.voiceProximity ||
        alice.status().settings.voiceProximityRange != 75 ||
        bob.status().settings.voiceProximityRange != 75) {
        fail("host proximity changes were not applied by joiners");
    }
    if (alice.status().settings.saveRecovery || bob.status().settings.saveRecovery ||
        !alice.status().saveRecoverySettingsReady) {
        fail("joiners did not apply host save recovery setting");
    }
    alice.send({{"type", "save_recovery_setting"}, {"enabled", true}});
    pump(host, alice, bob, 30);
    if (bob.status().settings.saveRecovery || host.status().settings.saveRecovery) {
        fail("direct joiner changed host-controlled save recovery setting");
    }
    alice.send({{"type", "voice_settings"}, {"enabled", false},
                {"range_percent", 0}});
    pump(host, alice, bob, 30);
    if (bob.status().settings.voiceProximityRange != 75) {
        fail("direct joiner changed host-controlled proximity range");
    }

    error.clear();
    if (alice.kick_peer(bobId, &error) || error.empty()) {
        fail("direct joiner was allowed to kick another player");
    }
    error.clear();
    if (!host.kick_peer(bobId, &error)) {
        fail("direct host kick failed: " + error);
    }
    pump(host, alice, bob, 120);
    if (!drain_for_type(alice, "peer_left")) {
        fail("kicked peer departure was not broadcast");
    }
    bool bobWasKicked = false;
    while (bob.has_events()) {
        const auto event = bob.pop_event();
        bobWasKicked |= event.kind == EventKind::Disconnected &&
            event.detail == "Kicked by lobby host";
    }
    if (!bobWasKicked || bob.status().enabled || bob.status().reconnecting ||
        host.peers().contains(bobId)) {
        fail("direct kick did not terminate the guest without reconnecting");
    }
    if (!host.status().semanticVisualsReady || !alice.status().semanticVisualsReady) {
        fail("semantic visual negotiation changed after peer left");
    }
    if (!host.status().snapshotDeltasReady || !alice.status().snapshotDeltasReady) {
        fail("snapshot delta negotiation did not recover after unsupported peer left");
    }

    host.disconnect();
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    while (!alice.status().reconnecting && std::chrono::steady_clock::now() < recoveryDeadline) {
        alice.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!alice.status().enabled || !alice.status().reconnecting || alice.status().welcomed) {
        fail("established connection loss did not expose reconnecting state");
    }
    if (!host.start_direct_host(hostConfig, &error)) fail("host restart failed");
    const auto welcomeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
    while (!alice.status().welcomed && std::chrono::steady_clock::now() < welcomeDeadline) {
        host.tick();
        alice.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!alice.status().welcomed || alice.status().reconnecting) {
        fail("established session did not recover and clear reconnecting state");
    }
    alice.disconnect();
    host.disconnect();
    std::cout << "transport test passed\n";
    return 0;
}
