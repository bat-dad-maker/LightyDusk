#include "dusklight_online/game/floor_switch_sync.hpp"
#include <cstdlib>
#include <iostream>
#include <random>

namespace fs = dusklight_online::game::floor_switch;
static unsigned checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { \
    std::cerr << "line " << __LINE__ << ": " << #__VA_ARGS__ << '\n'; std::exit(1); } } while (0)

int main() {
    using Result = fs::Ledger::Result;
    const fs::Scene scene{20, "D_MN11", 3, 0};
    const fs::Key plate{fs::Push, 0x410F01FF, {100, 200, 300}, 0xFFFF};
    const fs::Pressure none{}, light{1, false}, heavy{1, true}, block{2, false};
    CHECK(fs::supported(fs::Push, 0x400225FF)); // Temple of Time Gold A
    CHECK(fs::supported(fs::Push, 0x4102E0FF)); // Temple of Time entrance Gold B
    CHECK(fs::output_flag(fs::Push, 0x4102E0FF) == 0xE0);
    CHECK(fs::supported(fs::Push, 0x410F01FF)); // Snowpeak Gold B
    CHECK(fs::supported(fs::Push, 0x440002FF)); // Lakebed
    CHECK(!fs::supported(fs::Push, 0x420002FF)); // unused inverted
    CHECK(!fs::supported(fs::Push, 0x430002FF)); // unused toggle
    CHECK(!fs::supported(0x17, 0));             // unused swpush2
    CHECK(!fs::supported(0x7FFE, 0));           // remote player profile
    CHECK(fs::momentary(fs::Push, plate.params));
    CHECK(!fs::momentary(fs::Push, 0x400225FF));
    CHECK(!fs::momentary(fs::Iron, 0x000FF004)); // externally reset, not release-driven
    CHECK(!fs::momentary(fs::Heavy, 2));        // latch, not release-driven
    CHECK(fs::output_flag(fs::Push, 0x410F01FF) == 1);
    CHECK(fs::output_flag(fs::Iron, 0x000FF0EA) == 0xEA);
    CHECK(fs::output_flag(fs::Heavy, 2) == 2);
    for (const auto a : {none, light, heavy, block}) {
        CHECK(fs::combine(a, none) == a);
        CHECK(fs::combine(a, a) == a);
        for (const auto b : {none, light, heavy, block}) {
            CHECK(fs::combine(a, b) == fs::combine(b, a));
            for (const auto c : {none, light, heavy, block})
                CHECK(fs::combine(fs::combine(a, b), c) == fs::combine(a, fs::combine(b, c)));
        }
    }
    fs::Packet a{1, scene, {{plate, light}}}, b{1, scene, {{plate, heavy}}};
    fs::Ledger ledger;
    CHECK(ledger.receive("A", a, 100) == Result::Accepted);
    CHECK(ledger.receive("B", b, 100) == Result::Accepted);
    CHECK(ledger.pressure(scene, plate, 101) == heavy);
    a.sequence++; a.contacts.clear();
    CHECK(ledger.receive("A", a, 150) == Result::Accepted);
    CHECK(ledger.pressure(scene, plate, 151) == heavy); // A leaving does not release B
    b.sequence++; b.contacts.clear();
    CHECK(ledger.receive("B", b, 200) == Result::Accepted);
    CHECK(ledger.pressure(scene, plate, 201) == none);
    a.sequence = 1; a.contacts[plate] = light;
    CHECK(ledger.receive("A", a, 250) == Result::Stale);
    CHECK(ledger.pressure(scene, plate, 251) == none);
    a.sequence = 3;
    CHECK(ledger.receive("A", a, 300) == Result::Accepted);
    CHECK(ledger.pressure(scene, plate, 299) == none); // clock reversal is not a lease
    CHECK(ledger.pressure(scene, plate, 300 + fs::LeaseMs - 1) == light);
    CHECK(ledger.pressure(scene, plate, 300 + fs::LeaseMs) == none);
    CHECK(ledger.receive("A", a, 10000) == Result::Stale); // duplicate cannot renew
    CHECK(ledger.pressure(scene, plate, 10000) == none);
    a.sequence++;
    CHECK(ledger.receive("A", a, 11000) == Result::Accepted);
    auto other = scene; other.room++;
    CHECK(ledger.pressure(other, plate, 11001) == none);
    other = scene; other.stage = "D_MN06";
    CHECK(ledger.pressure(other, plate, 11001) == none);
    other = scene; other.table++;
    CHECK(ledger.pressure(other, plate, 11001) == none);
    other = scene; other.layer++;
    CHECK(ledger.pressure(other, plate, 11001) == none);
    auto key = plate; key.home[0]++;
    CHECK(ledger.pressure(scene, key, 11001) == none);
    key = plate; key.placementZ++;
    CHECK(ledger.pressure(scene, key, 11001) == none);
    key = plate; key.params++;
    CHECK(ledger.pressure(scene, key, 11001) == none);
    ledger.clear_pressure();
    CHECK(ledger.pressure(scene, plate, 11002) == none);
    CHECK(ledger.receive("A", a, 11003) == Result::Stale); // save/toggle safety
    ++a.sequence;
    CHECK(ledger.receive("A", a, 11004) == Result::Accepted);
    ledger.forget("A");
    CHECK(ledger.pressure(scene, plate, 11005) == none);
    ledger.reset_session();
    a.sequence = 1;
    CHECK(ledger.receive("A", a, 11006) == Result::Accepted);

    // A full snapshot replaces the sender's old room and entire contact set.
    ++a.sequence; a.scene = other;
    CHECK(ledger.receive("A", a, 12000) == Result::Accepted);
    CHECK(ledger.pressure(scene, plate, 12001) == none);
    CHECK(ledger.pressure(other, plate, 12001) == light);
    auto bad = a; ++bad.sequence; bad.scene.stage = "../bad";
    CHECK(ledger.receive("A", bad, 12002) == Result::Invalid);
    CHECK(ledger.pressure(other, plate, 12003) == light);
    bad = a; ++bad.sequence; bad.contacts[plate].ride = 0;
    CHECK(ledger.receive("A", bad, 12004) == Result::Invalid);
    CHECK(ledger.pressure(other, plate, 12005) == light);
    bad = a; bad.sequence = 0; CHECK(!fs::valid_packet(bad));
    bad = a; bad.scene.layer = 16; CHECK(!fs::valid_packet(bad));
    bad = a; bad.scene.table = 32; CHECK(!fs::valid_packet(bad));
    bad = a; bad.scene.room = 64; CHECK(!fs::valid_packet(bad));
    key = plate; key.home[1] = 4000001; CHECK(!fs::valid_key(key));
    key = {fs::Heavy, 2, {}, 1}; CHECK(!fs::valid_key(key));
    CHECK(ledger.receive("", a, 1) == Result::Invalid);
    CHECK(ledger.receive(std::string(129, 'x'), a, 1) == Result::Invalid);
    ledger.reset_session();
    for (std::size_t i = 0; i < fs::MaxPeers; ++i)
        CHECK(ledger.receive(std::to_string(i), a, 1) == Result::Accepted);
    CHECK(ledger.receive("overflow", a, 1) == Result::Full);
    CHECK(ledger.peer_count() == fs::MaxPeers);
    a.contacts.clear();
    for (std::size_t i = 0; i < fs::MaxSwitches; ++i) {
        key = plate; key.home[0] = int(i); a.contacts[key] = light;
    }
    CHECK(fs::valid_packet(a));
    key.home[0]++; a.contacts[key] = light;
    CHECK(!fs::valid_packet(a));

    fs::Contacts previous{{plate, light}};
    CHECK(fs::finish_frame(previous, {}, true) == previous);
    CHECK(fs::finish_frame(previous, {}, false).empty());
    CHECK(fs::finish_frame(previous, {{plate, none}}, true).empty());
    CHECK(fs::finish_frame(previous, {{plate, heavy}}, false).at(plate) == heavy);

    // Independent reference model under reordered per-peer snapshots,
    // heartbeats, empty releases, room changes and lease expiry.
    struct Ref { uint64_t seq = 0, at = 0; bool held = false, heavy = false; int room = 3; };
    std::array<Ref, 8> reference{};
    ledger.reset_session();
    std::mt19937 rng(0xF1005);
    uint64_t now = 1;
    for (unsigned step = 0; step < 20000; ++step) {
        now += rng() % 150;
        unsigned index = rng() % reference.size();
        auto& ref = reference[index];
        uint64_t sequence = (rng() % 4 == 0) ? ref.seq : ref.seq + 1;
        bool held = rng() % 2, isHeavy = rng() % 2;
        int room = 3 + int(rng() % 2);
        fs::Packet packet{sequence, {20, "D_MN11", room, 0}, {}};
        if (held) packet.contacts[plate] = {1, isHeavy};
        const auto result = ledger.receive(std::to_string(index), packet, now);
        if (sequence == 0) CHECK(result == Result::Invalid);
        else if (sequence <= ref.seq) CHECK(result == Result::Stale);
        else {
            CHECK(result == Result::Accepted);
            ref = {sequence, now, held, isHeavy, room};
        }
        bool expectedHeld = false, expectedHeavy = false;
        for (const auto& r : reference)
            if (r.seq && r.held && r.room == scene.room && now - r.at < fs::LeaseMs) {
                expectedHeld = true; expectedHeavy |= r.heavy;
            }
        CHECK(ledger.pressure(scene, plate, now) == fs::Pressure{uint8_t(expectedHeld), expectedHeavy});
    }
    std::cout << "floor-switch model: " << checks << " checks passed\n";
}
