#include "dusklight_online/net/reliable_udp.hpp"
#include "dusklight_online/net/datagram_scheduler.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace dusklight_online::net;
using Bytes = std::vector<uint8_t>;
void check(bool ok) { if (!ok) throw std::runtime_error("benchmark integrity failure"); }
struct Wire : DatagramTransport {
    struct Packet { uint32_t due; Bytes bytes; };
    std::deque<Packet> packets;
    std::mt19937 random;
    unsigned loss;
    uint32_t now = 0;
    Wire(unsigned seed, unsigned loss) : random(seed), loss(loss) {}
    bool send(LogicalPeerId peer, std::span<const uint8_t> bytes) override {
        check(peer == 7);
        // Independent loss in both directions, including ACK and realtime traffic.
        if (random() % 10000 < loss * 100) return true;
        packets.push_back({now + 25, {bytes.begin(), bytes.end()}});
        return true;
    }
    void deliver(ReliableUdp& target) {
        while (!packets.empty() && packets.front().due <= now) {
            check(dispatch_datagram(7, packets.front().bytes, target,
                [](auto, auto) { return true; }));
            packets.pop_front();
        }
    }
};
uint32_t percentile(std::vector<uint32_t> values, unsigned p) {
    check(!values.empty());
    std::sort(values.begin(), values.end());
    return values[(values.size() * p + 99) / 100 - 1];
}
struct Result { std::vector<uint32_t> latency, saveTime; };
void run(unsigned seed, unsigned loss, size_t saveSize, Result& result) {
    Wire ab(seed, loss), ba(seed + 1000, loss);
    DatagramScheduler sa(ab), sb(ba); // Unmodified production budgets.
    ReliableUdp a(sa), b(sb);
    check(sa.add_peer(7) && sb.add_peer(7));
    check(a.add_peer(7, 42) && b.add_peer(7, 42));
    Bytes pose(400); std::copy_n("DMPU", 4, pose.begin());
    Bytes save(saveSize);
    for (size_t i = 0; i < save.size(); ++i) save[i] = uint8_t(i * 31 + 17);
    std::deque<std::pair<uint32_t, Bytes>> events;
    bool saveReceived = !saveSize;
    unsigned measured = 0;
    // Warm up for 5 s. Measure the SAME 40-event cohort in each scenario:
    // 20 Hz events generated in the first 2 s after the bulk send is queued.
    // Continue generating events afterward so drain time is not artificially idle.
    for (uint32_t now = 0; now < 120000; now += 5) {
        ab.now = ba.now = now;
        ab.deliver(b); ba.deliver(a);
        if (now == 5000 && saveSize) check(a.send(7, save));
        if (now % 50 == 0) {
            Bytes event(256, 0xA5);
            for (unsigned i = 0; i < 4; ++i) event[i] = uint8_t(now >> (8 * i));
            check(a.send(7, event));
            events.emplace_back(now, event);
            check(sa.send(7, pose) && sb.send(7, pose));
        }
        a.update(now); b.update(now);
        sa.update(now); sb.update(now);
        check(!a.failed(7) && !b.failed(7));
        ReliableUdp::Message message;
        while (b.pop(message)) {
            if (saveSize && message.bytes.size() == saveSize) {
                check(!saveReceived && message.bytes == save);
                saveReceived = true;
                result.saveTime.push_back(now - 5000);
            } else {
                check(!events.empty() && message.bytes == events.front().second);
                auto sent = events.front().first;
                if (sent >= 5000 && sent < 7000) {
                    check(saveReceived);
                    result.latency.push_back(now - sent);
                    ++measured;
                }
                events.pop_front();
            }
        }
        if (measured == 40 && saveReceived) return;
    }
    throw std::runtime_error("benchmark exceeded simulated 120 s");
}
int main() {
    try {
        std::cout << "loss_pct,save_kib,event_samples,median_ms,p95_ms,max_ms,median_save_ms\n";
        for (unsigned loss : {0, 2, 5, 10}) for (size_t size : {0, 128 * 1024, 512 * 1024}) {
            Result result;
            for (unsigned seed = 1; seed <= 10; ++seed) run(seed, loss, size, result);
            std::cout << loss << ',' << size / 1024 << ',' << result.latency.size()
                << ',' << percentile(result.latency, 50) << ',' << percentile(result.latency, 95)
                << ',' << percentile(result.latency, 100) << ','
                << (size ? percentile(result.saveTime, 50) : 0) << '\n';
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
