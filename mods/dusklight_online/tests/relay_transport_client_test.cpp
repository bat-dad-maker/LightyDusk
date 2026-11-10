#include <fstream>
#include "dusklight_online/net/transport.hpp"
#include "cave_map_packet.hpp"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <algorithm>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

using dusklight_online::net::RelayConfig;
using dusklight_online::net::Transport;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "relay transport client test failed: " << message << '\n';
    std::exit(1);
}

bool wait_until(Transport& first, Transport& second, const auto& predicate,
                int timeoutMilliseconds = 10000) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        first.tick();
        second.tick();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::getenv("DUSKLIGHT_TEST_WAN") ? 33 : 1));
    }
    return false;
}

bool consume_type(Transport& transport, const std::string& type,
                  nlohmann::json* message = nullptr) {
    bool found = false;
    while (transport.has_events()) {
        auto event = transport.pop_event();
        if (event.message.is_object() && event.message.value("type", "") == type) {
            found = true;
            if (message != nullptr) {
                *message = std::move(event.message);
            }
        }
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Model a game process with a fine timer cadence; relay timing is separate.
    timeBeginPeriod(1);
#endif
    if (argc != 3) {
        fail("expected relay TCP port argument");
    }
    const int parsedPort = std::stoi(argv[1]);
    if (parsedPort < 1 || parsedPort > 65535) {
        fail("invalid relay port argument");
    }

    Transport owner;
    Transport joiner;
    RelayConfig ownerConfig;
    ownerConfig.name = "Owner";
    ownerConfig.room = "transport-client-test";
    ownerConfig.lobbyCode = "testing-only";
    ownerConfig.sessionKey = "integration-endpoint-key";
    ownerConfig.host = "127.0.0.1";
    ownerConfig.port = static_cast<uint16_t>(parsedPort);
    ownerConfig.createRoom = true;
    ownerConfig.settings.syncWorld = true;
    ownerConfig.settings.pvp = true;
    ownerConfig.settings.voiceProximity = false;
    ownerConfig.settings.voiceProximityRange = 125;

    std::string error;
    if (!owner.start_relay(ownerConfig, &error)) {
        fail("owner start: " + error);
    }
    if (!wait_until(owner, joiner, [&] { return owner.status().welcomed; })) {
        fail("owner welcome timeout: " + owner.status().error);
    }

    RelayConfig joinConfig = ownerConfig;
    joinConfig.name = "Joiner";
    joinConfig.createRoom = false;
    joinConfig.settings.voiceProximity = true;
    joinConfig.settings.voiceProximityRange = 50;
    auto rejectedConfig = joinConfig;
    rejectedConfig.lobbyCode = "incorrect-code";
    if (!joiner.start_relay(rejectedConfig, &error) ||
        !wait_until(owner, joiner, [&] { return !joiner.status().enabled; })) {
        fail("rejected lobby join remained enabled");
    }
    if (joiner.status().reconnecting || joiner.status().error.empty()) {
        fail("rejected lobby join did not preserve a terminal failure");
    }
    for (int i = 0; i < 90; ++i) joiner.tick();
    if (joiner.status().enabled) fail("rejected lobby join retried silently");
    if (!joiner.start_relay(joinConfig, &error)) {
        fail("joiner start: " + error);
    }
    if (!wait_until(owner, joiner, [&] {
            return joiner.status().welcomed && owner.peers().size() == 1 &&
                   owner.status().udpReady && joiner.status().udpReady;
        })) {
        fail("joiner/UDP welcome timeout: " + joiner.status().error);
    }
    if (!owner.status().isOwner || joiner.status().isOwner ||
        !joiner.status().settings.syncWorld || !joiner.status().settings.pvp ||
        !owner.status().semanticVisualsReady || !joiner.status().semanticVisualsReady ||
        !owner.status().snapshotDeltasReady || !joiner.status().snapshotDeltasReady) {
        fail("relay owner/settings state was not normalized from welcome");
    }
    if (!wait_until(owner, joiner, [&] { return joiner.status().voiceSettingsReady; }) ||
        joiner.status().settings.voiceProximity ||
        joiner.status().settings.voiceProximityRange != 125)
        fail("relay joiner did not receive peer voice settings from owner");
    if (!wait_until(owner, joiner, [&] { return joiner.status().saveRecoverySettingsReady; }) ||
        !joiner.status().settings.saveRecovery)
        fail("relay joiner did not receive the host save recovery setting");
    if (!owner.publish_voice_settings(true, 75) ||
        !wait_until(owner, joiner, [&] {
            return joiner.status().settings.voiceProximity &&
                   joiner.status().settings.voiceProximityRange == 75;
        })) fail("relay peer voice setting update failed");
    if (!owner.publish_save_recovery_setting(false) ||
        !wait_until(owner, joiner, [&] { return !joiner.status().settings.saveRecovery; }) ||
        !owner.publish_save_recovery_setting(true) ||
        !wait_until(owner, joiner, [&] { return joiner.status().settings.saveRecovery; }))
        fail("relay peer save recovery setting update failed");

    bool ownerDirect = false, joinerDirect = false;
    if (!std::getenv("DUSKLIGHT_TEST_RELAY_ONLY") && !wait_until(owner, joiner, [&] {
        for (auto* client : {&owner, &joiner}) while (client->has_events()) {
            const auto event = client->pop_event();
            if (event.kind == dusklight_online::net::EventKind::RouteChanged && event.detail == "direct")
                (client == &owner ? ownerDirect : joinerDirect) = true;
        }
        return ownerDirect && joinerDirect;
    }, 10000)) fail("real relay signaling did not establish bidirectional ICE paths");

    for (Transport* client : {&owner, &joiner}) {
        const auto status = client->status();
        const bool relayOnly = std::getenv("DUSKLIGHT_TEST_RELAY_ONLY") != nullptr;
        if (status.natPeerCount != (relayOnly ? 0u : 1u) ||
            status.relayPeerCount != (relayOnly ? 1u : 0u)) {
            fail("connection status did not reflect the active peer route");
        }
    }

    // Run in every carrier configuration, including forced relay, ICE direct,
    // loss and asymmetric delay. The game needs these exact source fields.
    const auto checkCaveDelivery = [&] {
        const auto cave = cave_map_packet();
        for (Transport* sender : {&owner, &joiner}) {
            Transport& recipient = sender == &owner ? joiner : owner;
            if (!sender->send(cave)) fail("cave reveal send");
            nlohmann::json received;
            if (!wait_until(owner, joiner, [&] {
                return consume_type(recipient, "switch_bit", &received);
            }) || received.value("client_id", "") != sender->status().clientId)
                fail("cave reveal missing or wrong sender identity");
            received.erase("client_id");
            if (received != cave) fail("cave reveal source data changed");
        }
    };
    checkCaveDelivery();

    if (!owner.send({{"type", "chat"}, {"text", "Hello through\nthe relay"},
                     {"client_id", "spoof"}})) {
        fail("chat send");
    }
    nlohmann::json chat;
    if (!wait_until(owner, joiner, [&] { return consume_type(joiner, "chat", &chat); }) ||
        chat.value("text", "") != "Hello through\nthe relay" ||
        chat.value("client_id", "") != owner.status().clientId) {
        fail("chat delivery or sender attribution");
    }

    if (ownerDirect && joinerDirect) {
        const auto start = std::chrono::steady_clock::now();
        if (!owner.send({{"type","pvp_hit"},{"damage",1},{"direct_latency_probe",true}})) fail("direct hit send");
        nlohmann::json hit;
        if (!wait_until(owner,joiner,[&]{return consume_type(joiner,"pvp_hit",&hit);})) fail("direct hit missing");
        const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if (std::getenv("DUSKLIGHT_TEST_WAN") && ms >= 200) fail("direct hit inherited delayed relay latency");
        std::cout << "direct reliable hit ms=" << ms << " (relay-independent)\n";
    }

    // Run the actual upper-bound sync+warp/progression fixture through the
    // production peer path, bracketed by authority settings in one send batch.
    std::ifstream fixtureFile(argv[2]);
    nlohmann::json fixture; fixtureFile >> fixture;
    if (ownerDirect && joinerDirect) {
        auto forceFallback = [](bool enabled) {
#if defined(_WIN32)
            _putenv_s("DUSKLIGHT_TEST_FORCE_FALLBACK",enabled ? "1" : "");
#else
            if (enabled) setenv("DUSKLIGHT_TEST_FORCE_FALLBACK","1",1);
            else unsetenv("DUSKLIGHT_TEST_FORCE_FALLBACK");
#endif
        };
        // Switch with a full sync AND a cave reveal in flight. Relay copies may
        // arrive after their direct duplicates; neither order nor exactly-once
        // application may depend on which carrier wins that race.
        for (bool initiallyFallback : {true,false}) {
            forceFallback(initiallyFallback); owner.tick(); joiner.tick();
            auto sync = fixture; sync["handover_test"] = 1;
            auto cave = cave_map_packet(); cave["handover_test"] = 2;
            if (!owner.send(sync) || !owner.send(cave))
                fail("handover enqueue");
            forceFallback(!initiallyFallback); owner.tick();
            if (!owner.send({{"type","pvp_hit"},{"handover_test",3},{"damage",1}})) fail("handover post hit");
            int expected=1;
            const auto started=std::chrono::steady_clock::now();
            if (!wait_until(owner,joiner,[&] {
                while (joiner.has_events()) {
                    auto event=joiner.pop_event();
                    if (!event.message.is_object() || !event.message.contains("handover_test")) continue;
                    const int value=event.message.at("handover_test");
                    if (value!=expected++) fail("handover lost, duplicated or reordered gameplay");
                    if (value==1) for(auto it=sync.begin();it!=sync.end();++it)
                        if(!event.message.contains(it.key()) || event.message[it.key()]!=it.value()) fail("handover sync corrupted");
                    if (value==2) for(auto it=cave.begin();it!=cave.end();++it)
                        if(!event.message.contains(it.key()) || event.message[it.key()]!=it.value()) fail("handover cave reveal corrupted");
                }
                return expected==4 && std::chrono::steady_clock::now()-started>std::chrono::seconds(3);
            },15000)) fail("handover timeout");
        }
        forceFallback(false); owner.tick(); joiner.tick();
        std::cout << "in-flight sync/cave reveal/hit survived both carrier changes exactly once and in order\n";
    }
    fixture["target_client_id"] = joiner.status().clientId;
    {
        nlohmann::json boundary={{"type","save_snapshot"},{"boundary_test",true},{"padding",""}};
        boundary["padding"]=std::string(512*1024-boundary.dump().size(),'x');
        if (!owner.send(boundary)) fail("maximum-size gameplay payload rejected by routing envelope");
        nlohmann::json received;
        if (!wait_until(owner,joiner,[&]{return consume_type(joiner,"save_snapshot",&received);},15000))
            fail("maximum-size gameplay payload timeout");
        for(auto it=boundary.begin();it!=boundary.end();++it)
            if(!received.contains(it.key()) || received[it.key()]!=it.value()) fail("maximum-size payload mismatch");
    }
    fixture["delivery_test"] = 1;
    auto off = owner.status().settings; off.syncFlags = false;
    auto on = off; on.syncFlags = true;
    const auto deliveryStarted = std::chrono::steady_clock::now();
    if (!owner.publish_room_settings(off) || !owner.send(fixture) ||
        !owner.send({{"type","item_get"},{"item",7},{"delivery_test",2}}) ||
        !owner.publish_room_settings(on) ||
        !owner.send({{"type","pvp_hit"},{"damage",4},{"delivery_test",3}})) fail("ordered delivery send");
    int expectedDelivery = 1;
    if (!wait_until(owner,joiner,[&] {
        while(joiner.has_events()) {
            auto event=joiner.pop_event();
            if(!event.message.is_object() || !event.message.contains("delivery_test")) continue;
            const int marker=event.message["delivery_test"];
            if(marker!=expectedDelivery++ || event.ingress.settings.syncFlags!=(marker==3))
                fail("peer payload changed off/event/on order or duplicated delivery");
            if(marker==1) {
                auto received=event.message; received.erase("client_id");
                if(received!=fixture) fail("full sync/progression payload changed");
            }
        }
        return expectedDelivery==4;
    },10000)) fail("full progression delivery timeout");
    std::cout << "sync and following events ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-deliveryStarted).count() << "\n";
    std::cout << "full progression bytes=" << fixture.dump().size() << " exact; off/event/on ordering passed\n";

    for (int i=0;i<6;++i) {
        auto settings = owner.status().settings; settings.syncFlags = (i%2)==1;
        if (!owner.publish_room_settings(settings) ||
            !owner.send({{"type","pvp_hit"},{"damage",1},{"settings_stress",i}})) fail("settings stress send");
    }
    int stressExpected=0;
    if (!wait_until(owner,joiner,[&] {
        while(joiner.has_events()) {
            auto event=joiner.pop_event();
            if(!event.message.is_object() || !event.message.contains("settings_stress")) continue;
            const int value=event.message["settings_stress"];
            if(value!=stressExpected++ || event.ingress.settings.syncFlags!=((value%2)==1))
                fail("rapid settings generations reordered gameplay");
        }
        return stressExpected==6;
    },20000)) fail("settings stress timeout");
    std::cout << "six consecutive settings generations preserved gameplay order\n";

    if (!owner.send_visual({{"type", "pose"}, {"sequence", 5},
                            {"state", {{"stage", "F_SP103"}, {"x", 12.25f}}}})) {
        fail("relay UDP pose send failed");
    }
    nlohmann::json udpPose;
    if (!wait_until(owner, joiner, [&] {
            if (std::getenv("DUSKLIGHT_TEST_WAN")) owner.send_visual({{"type","pose"},{"sequence",5},{"state",{{"stage","F_SP103"},{"x",12.25f}}}});
            return consume_type(joiner, "pose", &udpPose);
        }) || udpPose.value("client_id", "") != owner.status().clientId ||
        udpPose.value("sequence", 0U) != 5) {
        fail("relay did not route the mod client's UDP pose");
    }

    // Exercise the game cadence on independently timed clients. Fast polling
    // hides repeated/skipped poses when the carrier batches across frame edges.
    // Padding models roughly one full semantic pose datagram without a game.
    std::string padding; uint32_t random=12345;
    for(int n=0;n<1050;++n) {random^=random<<13;random^=random>>17;random^=random<<5;padding.push_back(char(33+random%90));}
    using Clock=std::chrono::steady_clock;
    uint32_t base=100;
    for (int phase : (std::getenv("DUSKLIGHT_TEST_WAN") ? std::vector<int>{} : std::vector<int>{0,2,4,8,16,25})) {
        auto begin=Clock::now()+std::chrono::milliseconds(100);
        struct Stats { int holds=0, skips=0, received=0, errors=0; uint32_t last=0; double maxGap=0; std::vector<double> age; } stats[2];
        auto run=[&](Transport& t,int side) {
            auto next=begin+std::chrono::milliseconds(side?phase:0);
            auto previous=next;
            for(int tick=0; tick<120; ++tick) {
                std::this_thread::sleep_until(next);
                auto now=Clock::now();
                stats[side].maxGap=std::max(stats[side].maxGap,std::chrono::duration<double,std::milli>(now-previous).count()); previous=now;
                t.tick(); bool got=false;
                while(t.has_events()) {
                    auto e=t.pop_event();
                    if(e.kind==dusklight_online::net::EventKind::Error) ++stats[side].errors;
                    if(!e.message.is_object() || !e.message.contains("sent_ms")) continue;
                    auto seq=e.message.value("sequence",0u);
                    if(tick>=10) {
                        got=true; ++stats[side].received;
                        if(stats[side].last && seq>stats[side].last) stats[side].skips+=seq-stats[side].last-1;
                        stats[side].age.push_back(std::chrono::duration<double,std::milli>(Clock::now()-begin).count()-e.message["sent_ms"].get<double>());
                    }
                    stats[side].last=seq;
                }
                if(tick>=10 && !got) ++stats[side].holds;
                double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
                if (!t.send_visual({{"type","pose"},{"sequence",base+tick},{"sent_ms",ms},{"padding",padding},{"state",{{"stage","F_SP103"},{"x",ms}}}})) fail("cadence pose send failed");
                next+=std::chrono::nanoseconds(33333333);
            }
        };
        std::thread a(run,std::ref(owner),0), b(run,std::ref(joiner),1);a.join();b.join();
        for(int side=0;side<2;++side) {
            auto& s=stats[side];std::sort(s.age.begin(),s.age.end());
            std::cout<<"phase="<<phase<<" side="<<side<<" received="<<s.received<<" holds="<<s.holds<<" skips="<<s.skips<<" errors="<<s.errors<<" max_tick_ms="<<s.maxGap<<" p95_age="<<(s.age.empty()?0:s.age[s.age.size()*95/100])<<std::endl;
        }
        // At phase zero either client's send can straddle the other's update
        // even on the old transport. Report it, but check separated phases.
        for (int side=0;side<2;++side) {
            if (stats[side].errors || stats[side].received == 0)
                fail("cadence transport error or no poses");
            if (phase != 0 && stats[side].holds > 11)
                fail("more than 10 percent of cadence updates lacked a new pose");
            std::cout << "cadence wire_bytes=" << (side ? joiner : owner).last_visual_send_stats().wireBytes << '\n';
        }
        base+=200;
        while(owner.has_events()) owner.pop_event();while(joiner.has_events()) joiner.pop_event();
    }

    dusklight_online::net::udp::RemoteObjectPacket object;
    object.sequence = 6;
    object.objectId = 611;
    object.objectKind = 1;
    object.flags = dusklight_online::net::udp::ObjectActive;
    if (!owner.send_remote_object(object)) {
        fail("relay UDP object send failed");
    }
    bool receivedObject = false;
    if (!wait_until(owner, joiner, [&] {
            if (std::getenv("DUSKLIGHT_TEST_WAN")) owner.send_remote_object(object);
            while (joiner.has_events()) {
                auto event = joiner.pop_event();
                receivedObject |= event.kind == dusklight_online::net::EventKind::UdpRemoteObject &&
                                  event.remoteObject.objectId == 611;
            }
            return receivedObject;
        })) {
        fail("relay did not route the mod client's UDP remote object");
    }

    dusklight_online::net::udp::VoicePosition voicePosition;
    std::memcpy(voicePosition.stageName, "F_SP103", 7);
    voicePosition.x = 14.0f;
    voicePosition.room = 2;
    const std::vector<uint8_t> voiceFrame{0x78, 0x12, 0x34};
    if (!owner.send_voice(1, voicePosition, voiceFrame))
        fail("relay UDP voice send failed");
    bool receivedVoice = false;
    if (!wait_until(owner, joiner, [&] {
            if (std::getenv("DUSKLIGHT_TEST_WAN")) owner.send_voice(1, voicePosition, voiceFrame);
            while (joiner.has_events()) {
                auto event = joiner.pop_event();
                receivedVoice |= event.kind == dusklight_online::net::EventKind::UdpVoice &&
                                 event.voice == voiceFrame && event.voicePosition.x == 14.0f;
            }
            return receivedVoice;
        })) {
        fail("relay did not route the mod client's UDP voice packet");
    }

    // This optional MFB/randomizer lane must remain wire-compatible even when
    // its game adapter is absent from a particular Dusklight build.
    if (!owner.send({{"type", "rando_item_get"}, {"item", 0x48}})) {
        fail("rando_item_get send failed");
    }
    nlohmann::json received;
    if (!wait_until(owner, joiner, [&] {
            return consume_type(joiner, "rando_item_get", &received);
        }) || received.value("item", -1) != 0x48 ||
        received.value("client_id", "").empty()) {
        fail("rando_item_get was not relay-routed with authenticated identity");
    }

    if (!joiner.send({{"type", "sync_request"},
                      {"target_client_id", owner.status().clientId},
                      {"flags_only", true}})) {
        fail("targeted sync_request send failed");
    }
    if (!wait_until(owner, joiner, [&] {
            return consume_type(owner, "sync_request", &received);
        }) || received.value("client_id", "") != joiner.status().clientId) {
        fail("targeted sync_request did not reach the owner");
    }

    if (!std::getenv("DUSKLIGHT_TEST_WAN") && !std::getenv("DUSKLIGHT_TEST_RELAY_ONLY")) {
        auto environment = [](const char* name, const char* value) {
#if defined(_WIN32)
            _putenv_s(name,value);
#else
            if (*value) setenv(name,value,1); else unsetenv(name);
#endif
        };
        environment("DUSKLIGHT_TEST_RELAY_ONLY","1");
        environment("DUSKLIGHT_TEST_RESTART_ICE","1");
        owner.tick(); joiner.tick();
        fixture["delivery_test"] = 4;
        if (!owner.send(fixture)) fail("fallback sync send");
        if (!wait_until(owner,joiner,[&] { return consume_type(joiner,"save_snapshot",&received); },10000))
            fail("restarted ICE did not deliver reliable payload by relay fallback");
        received.erase("client_id");
        if(received!=fixture) fail("fallback changed full sync payload");
        checkCaveDelivery();
        environment("DUSKLIGHT_TEST_RELAY_ONLY","");
        environment("DUSKLIGHT_TEST_RESTART_ICE","");
        ownerDirect=joinerDirect=false;
        if(!wait_until(owner,joiner,[&] {
            for(auto* client:{&owner,&joiner}) while(client->has_events()) {
                auto event=client->pop_event();
                if(event.kind==dusklight_online::net::EventKind::RouteChanged && event.detail=="direct")
                    (client==&owner?ownerDirect:joinerDirect)=true;
            }
            return ownerDirect&&joinerDirect;
        },10000)) fail("production direct route did not recover after fallback");
        checkCaveDelivery();
        std::cout << "production reliable ICE restart/fallback/recovery passed\n";
    }

    // Let a third member join without servicing the sender's game tick. The
    // authoritative dispatch must supply a body to this newly admitted peer.
    Transport third;
    auto thirdConfig = joinConfig; thirdConfig.name = "Third";
    if(!third.start_relay(thirdConfig,&error)) fail("third member start");
    if(!wait_until(joiner,third,[&] { return third.status().welcomed; })) fail("third member welcome");
    if(!wait_until(owner,third,[&]{ joiner.tick(); return owner.peers().size()==2; })) fail("sender roster admission");
    if(!owner.send({{"type","item_get"},{"item",8},{"membership_race",true}})) fail("membership race send");
    bool secondGot=false,thirdGot=false;
    if(!wait_until(owner,third,[&] {
        joiner.tick();
        secondGot |= consume_type(joiner,"item_get");
        thirdGot |= consume_type(third,"item_get");
        return secondGot&&thirdGot;
    },10000)) fail("membership race lost authorized recipient");
    const std::string thirdId = third.status().clientId;
    error.clear();
    if (joiner.kick_peer(thirdId, &error) || error.empty())
        fail("relay non-owner was allowed to request a kick");
    error.clear();
    if (!owner.kick_peer(thirdId, &error)) fail("relay owner kick: " + error);
    if(!wait_until(owner,third,[&] {
        joiner.tick();
        return owner.peers().size()==1 && !third.status().enabled;
    },20000)) fail("kicked relay member departure");
    if (third.status().reconnecting) fail("kicked relay member attempted to reconnect");
    std::cout << "concurrent membership admission and owner kick passed\n";

    auto changed = owner.status().settings;
    changed.remoteCollision = false;
    changed.pvp = true;
    if (!owner.publish_room_settings(changed)) {
        fail("owner room_settings send failed");
    }
    if (!wait_until(owner, joiner, [&] {
        return !joiner.status().settings.remoteCollision;
    }) || joiner.status().settings.pvp) {
        fail("relay did not normalize and publish room settings");
    }

    owner.disconnect();
    if (!wait_until(owner, joiner, [&] { return joiner.status().isOwner; },20000)) {
        fail("relay ownership did not transfer after owner disconnect");
    }
    bool sawOwnerChanged = false;
    while (joiner.has_events()) {
        const auto event = joiner.pop_event();
        if (event.kind == dusklight_online::net::EventKind::Message &&
            event.message.value("type", std::string()) == "owner_changed" &&
            event.message.value("owner_client_id", std::string()) ==
                joiner.status().clientId) {
            sawOwnerChanged = true;
        }
    }
    if (!sawOwnerChanged) {
        fail("relay ownership transfer did not deliver owner_changed");
    }

    joiner.disconnect();
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
    std::cout << "relay transport client test passed\n";
    return 0;
}
