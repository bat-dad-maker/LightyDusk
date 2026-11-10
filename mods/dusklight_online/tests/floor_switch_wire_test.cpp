#include "dusklight_online/game/floor_switch_wire.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace fs = dusklight_online::game::floor_switch;
using nlohmann::json;
static unsigned checks = 0;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { \
    std::cerr << "line " << __LINE__ << ": " << #__VA_ARGS__ << '\n'; std::exit(1); } } while (0)

int main() {
    const fs::Key key{fs::Push, 0x410F01FF, {100, 200, 300}, 65535};
    const fs::Packet packet{1, {20, "D_MN11", 3, 0}, {{key, {1, false}}}};
    const auto good = fs::encode(packet);
    const auto parsed = fs::decode(json::parse(good.dump()));
    CHECK(parsed && parsed->sequence == packet.sequence && parsed->scene == packet.scene &&
          parsed->contacts == packet.contacts);
    auto j = good; j["contacts"] = json::array(); CHECK(fs::decode(j)->contacts.empty());
    j = good; j["client_id"] = "relay-authenticated-peer"; j["target_client_id"] = "destination";
    CHECK(fs::decode(j)); // transport envelope fields are allowed
    for (auto name : {"type", "version", "sequence", "stage", "source_stage", "room", "layer", "contacts"}) {
        j = good; j.erase(name); CHECK(!fs::decode(j));
    }
    const json badNumbers[] = {nullptr, false, true, "1", 1.0, json::array(), json::object(), -999,
                               std::numeric_limits<uint64_t>::max()};
    for (auto name : {"version", "sequence", "stage", "room", "layer"})
        for (const auto& bad : badNumbers) {
            j = good; j[name] = bad; CHECK(!fs::decode(j));
        }
    j = good; j["version"] = 2; CHECK(!fs::decode(j));
    j = good; j["sequence"] = 0; CHECK(!fs::decode(j));
    j = good; j["type"] = "room_actor_action"; CHECK(!fs::decode(j));
    j = good; j["room"] = 64; CHECK(!fs::decode(j));
    j = good; j["stage"] = 32; CHECK(!fs::decode(j));
    j = good; j["layer"] = 16; CHECK(!fs::decode(j));
    j = good; j["layer"] = -1; CHECK(fs::decode(j));
    for (auto bad : {"", "../evil", "D_MN11\n", "D_mn11", "TOO_LONG_STAGE"}) {
        j = good; j["source_stage"] = bad; CHECK(!fs::decode(j));
    }
    for (const json bad : {json(nullptr), json(1), json(false), json::array(), json::object()}) {
        j = good; j["source_stage"] = bad; CHECK(!fs::decode(j));
        CHECK(!fs::decode(bad));
    }
    for (auto name : {"actor", "params", "home", "placement_z", "ride", "heavy"}) {
        j = good; j["contacts"][0].erase(name); CHECK(!fs::decode(j));
    }
    for (auto name : {"actor", "params", "placement_z", "ride"})
        for (const auto& bad : badNumbers) {
            j = good; j["contacts"][0][name] = bad; CHECK(!fs::decode(j));
        }
    j = good; j["contacts"][0]["params"] = uint64_t(0x100000000ULL); CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["placement_z"] = 65536; CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["ride"] = 0; CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["ride"] = 3; CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["ride"] = 2; CHECK(fs::decode(j));
    j = good; j["contacts"][0]["heavy"] = 1; CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["heavy"] = true; CHECK(fs::decode(j));
    j = good; j["contacts"][0]["extra"] = 1; CHECK(!fs::decode(j));
    j = good; j["contacts"].push_back(j["contacts"][0]); CHECK(!fs::decode(j));
    for (int coordinate = 0; coordinate < 3; ++coordinate) {
        for (const auto& bad : badNumbers) {
            // -999 is a legal coordinate; test the lower bound separately.
            if (bad == json(-999)) continue;
            j = good; j["contacts"][0]["home"][coordinate] = bad; CHECK(!fs::decode(j));
        }
        j = good; j["contacts"][0]["home"][coordinate] = -4000001; CHECK(!fs::decode(j));
        j = good; j["contacts"][0]["home"][coordinate] = 4000001; CHECK(!fs::decode(j));
    }
    j = good; j["contacts"][0]["home"] = json::array({1, 2}); CHECK(!fs::decode(j));
    j = good; j["contacts"][0]["home"] = json::array({1, 2, 3, 4}); CHECK(!fs::decode(j));
    for (int actor : {0, 0x17, 0x7FFE, 0x7FFF}) {
        j = good; j["contacts"][0]["actor"] = actor; CHECK(!fs::decode(j));
    }
    for (int type : {2, 3, 5, 6, 7}) {
        j = good; j["contacts"][0]["params"] = uint32_t((type << 24) | 0x0F01FF);
        CHECK(!fs::decode(j));
    }
    for (int actor : {fs::Iron, fs::Heavy}) {
        j = good; j["contacts"][0]["actor"] = actor;
        j["contacts"][0]["params"] = uint32_t(0xFFFFFFFF);
        CHECK(!fs::decode(j)); // placement_z only belongs to obj_swpush
        j["contacts"][0]["placement_z"] = 0; CHECK(fs::decode(j));
        if (actor == fs::Heavy) {
            j["contacts"][0]["ride"] = 2; CHECK(!fs::decode(j));
        }
    }
    j = good; j["contacts"] = json::array();
    for (std::size_t i = 0; i < fs::MaxSwitches; ++i) {
        auto entry = good["contacts"][0]; entry["home"][0] = i; j["contacts"].push_back(entry);
    }
    CHECK(fs::decode(j));
    auto entry = good["contacts"][0]; entry["home"][0] = fs::MaxSwitches;
    j["contacts"].push_back(entry); CHECK(!fs::decode(j));
    std::cout << "floor-switch JSON boundary: " << checks << " checks passed\n";
}
