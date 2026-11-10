// Standalone real-transport comparison. Build once with HEAD's transport.cpp
// and once with the working tree. Network impairment is external (isolated netem).
#include "dusklight_online/net/transport.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include <atomic>
#include <cstring>
#include <sys/socket.h>
// Linux benchmark-only accounting; link with --wrap=sendto.
std::atomic<uint64_t> reliableWireBytes{0}, reliablePackets{0};
extern "C" ssize_t __real_sendto(int, const void*, size_t, int, const sockaddr*, socklen_t);
extern "C" ssize_t __wrap_sendto(int fd, const void* data, size_t size, int flags,
    const sockaddr* address, socklen_t length) {
    if (size >= 4 && std::memcmp(data, "DUR1", 4) == 0) {
        reliableWireBytes += size; ++reliablePackets;
    }
    return __real_sendto(fd, data, size, flags, address, length);
}
using namespace dusklight_online::net;
using json = nlohmann::json;
uint64_t ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
int main(int argc, char** argv) {
    try {
        check(argc >= 2, "payload file or none required");
        const bool lifecycle = argc > 2 && std::string(argv[2]) == "lifecycle";
        json save;
        bool bulk = std::string(argv[1]) != "none";
        if (bulk) { std::ifstream file(argv[1]); file >> save; }
        Transport host, client;
        DirectHostConfig hc; hc.bindHost = hc.publicHost = "127.0.0.1"; hc.port = 34197;
        hc.sessionKey = "benchmark-invite-key";
        check(host.start_direct_host(hc), "host failed");
        DirectJoinConfig jc; jc.host = hc.publicHost; jc.port = hc.port;
        jc.sessionKey = hc.sessionKey;
        check(client.start_direct_join(jc), "client failed");
        uint64_t handshake = ms();
        while (!client.status().welcomed || host.peers().empty()) {
            host.tick(); client.tick();
            check(ms() - handshake < 30000, "handshake timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto drain = [](Transport& t) { while (t.has_events()) t.pop_event(); };
        drain(host); drain(client);
        uint64_t start = ms(), next = 0;
        bool sentSave = false, receivedSave = !bulk;
        unsigned id = 0, expected = 0, measuredSent = 0;
        uint64_t saveTime = 0, saveSent = 0, postSent = 0, postLatency = 0;
        bool postQueued = false, postReceived = false;
        std::vector<uint64_t> latency;
        while (ms() - start < 90000) {
            uint64_t now = ms() - start;
            if (now >= 3000 && !sentSave) {
                if (bulk) check(client.send(save), "save rejected");
                saveSent = now; sentSave = true;
            }
            if (bulk && receivedSave && !postQueued) {
                postSent = now; postQueued = true;
                check(client.send({{"type","event_bit"},{"post_sync_probe",true}}), "probe rejected");
                next = now;
            }
            if (now >= next && !(lifecycle && sentSave && !receivedSave)) {
                bool measure = now >= 3000 && measuredSent < 40;
                if (measure) ++measuredSent;
                const char* types[] = {"pvp_hit", "event_bit", "switch_bit"};
                json event = {{"type",types[id%3]}, {"bench_id", id++}, {"sent_ms",now},
                    {"measured",measure}, {"flag",27}, {"stage",0},
                    {"padding",std::string(100,'x')}};
                check(client.send(event), "event rejected");
                json pose = {{"sequence",id},{"x",1.0},{"y",2.0},{"z",3.0},
                    {"stage","F_SP103"},{"room",0}};
                client.send_visual(pose, udp::PacketType::PoseMsgpack);
                host.send_visual(pose, udp::PacketType::PoseMsgpack);
                next = now + 50;
            }
            client.tick(); host.tick();
            while (host.has_events()) {
                auto e = host.pop_event();
                if (!e.message.is_object()) continue;
                if (e.message.value("post_sync_probe",false)) {
                    check(!postReceived, "duplicate probe");
                    postReceived = true; postLatency = ms()-start-postSent;
                } else if (e.message.contains("bench_id")) {
                    check(e.message["bench_id"].get<unsigned>() == expected++, "event order/duplication");
                    if (e.message.value("measured",false)) {
                        check(receivedSave, "event overtook save");
                        latency.push_back(ms()-start-e.message["sent_ms"].get<uint64_t>());
                    }
                } else if (e.message.value("type",std::string()) == "save_snapshot") {
                    check(!receivedSave, "duplicate save");
                    for (auto it=save.begin();it!=save.end();++it)
                        check(e.message.contains(it.key()) && e.message[it.key()]==it.value(), "save content mismatch");
                    receivedSave = true; saveTime = ms()-start-saveSent;
                }
            }
            drain(client);
            check(client.status().welcomed, "connection lost");
            if (measuredSent == 40 && latency.size() == 40 && receivedSave && (!bulk || postReceived)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(receivedSave && latency.size() == 40, "incomplete measurement");
        // Print raw measurements so the runner can pool equal-duration cohorts.
        std::cout << "{\"payload_bytes\":" << (bulk?save.dump().size()+1:0)
            << ",\"save_ms\":" << saveTime << ",\"post_sync_ms\":" << postLatency
            << ",\"reliable_wire_bytes\":" << reliableWireBytes.load()
            << ",\"reliable_packets\":" << reliablePackets.load()
            << ",\"latencies\":" << json(latency).dump() << "}\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
