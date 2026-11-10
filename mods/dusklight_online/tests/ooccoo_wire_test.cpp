#include "dusklight_online/game/ooccoo_wire.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace dusklight_online::game::ooccoo;
using nlohmann::json;
namespace {
unsigned checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
}
int main() {
    for (unsigned a = 0; a < 128; ++a) for (unsigned c = 0; c < 128; ++c) {
        Progress state{static_cast<uint8_t>(a), static_cast<uint8_t>(c), bool(a & 1), bool(c & 1)};
        check(decode(json::parse(encode(state).dump())) == state, "wire round trip");
    }
    const json valid = encode({1, 2, true, false});
    for (const auto& bad : {json(-1), json(128), json(1.0), json(true), json("1"), json(),
                           json(std::numeric_limits<uint64_t>::max()),
                           json(std::numeric_limits<int64_t>::min()), json::array()}) {
        for (const auto* field : {"acquired", "completed"}) {
            auto state = valid; state[field] = bad;
            check(!decode(state), "invalid mask rejected without narrowing");
        }
    }
    for (const auto* field : {"version", "acquired", "completed", "city_special",
                              "unbound_note", "anchors"}) {
        auto state = valid; state.erase(field);
        check(!decode(state), "incomplete state rejected");
    }
    for (const auto& version : {json(1), json(5), json("4"), json(4.0), json(true)}) {
        auto state = valid; state["version"] = version;
        check(!decode(state), "version must be exact integer 4 or legacy 2/3");
    }
    for (const auto* field : {"city_special", "unbound_note"}) {
        auto state = valid; state[field] = 1;
        check(!decode(state), "bool is not an integer field");
    }
    for (const auto* field : {"owner_stage", "return_stage", "return_x", "has_return_mark", "clear_stage"}) {
        auto state = valid; state[field] = 16;
        check(!decode(state), "legacy/travel/unknown fields rejected");
    }
    check(!decode(json{{"exists", false}}), "legacy empty state is not a clear");
    check(!decode(json{{"exists", true}, {"owner_stage", 25}}), "legacy cave owner rejected");
    check(!decode(json::array()) && !decode(nullptr), "nonobjects rejected");
    Progress anchored{1, 0, false, false};
    anchored.anchors[0] = {true, 0, 0, 10.0f, 20.0f, 30.0f, 123};
    check(decode(encode(anchored)) == anchored, "return anchor wire round trip");
    auto preWarpBuild = encode(anchored);
    preWarpBuild["version"] = 3;
    check(decode(preWarpBuild) == Progress{1, 0, false, false},
          "previous pickup-time return points cannot create Jr");
    auto legacy = valid;
    legacy["version"] = 2;
    legacy.erase("anchors");
    check(decode(legacy) == Progress{1, 2, true, false}, "existing version 2 save migrates");
    auto wrongStage = encode(anchored);
    wrongStage["anchors"][1] = wrongStage["anchors"][0];
    check(!decode(wrongStage), "anchor cannot claim a dungeon without acquisition");
    for (const auto& bad : {json(64), json(-1), json(1.5), json("0")}) {
        auto packet = encode(anchored);
        packet["anchors"][0]["room"] = bad;
        check(!decode(packet), "invalid return room rejected");
    }
    auto nonfinite = encode(anchored);
    nonfinite["anchors"][0]["x"] = 1.0e30;
    check(!decode(nonfinite), "out-of-bounds return position rejected");
    auto hugeAngle = encode(anchored);
    hugeAngle["anchors"][0]["angle"] = std::numeric_limits<uint64_t>::max();
    check(!decode(hugeAngle), "unsigned angle overflow rejected before narrowing");
    check(bounded_integer(json(0x1FF), 0x1FF) == 0x1FF, "pending upper bound valid");
    check(!bounded_integer(json(0x200), 0x1FF), "pending overflow invalid");
    State live; live.merge_remote({1, 0, false, false});
    const auto before = live.progress(); const auto pending = live.pending();
    auto malformed = valid; malformed["return_x"] = 10;
    if (const auto parsed = decode(malformed)) live.merge_remote(*parsed);
    check(live.progress() == before && live.pending() == pending, "failed decode has no partial effects");
    std::cout << "Ooccoo JSON wire: " << checks << " checks passed.\n";
}
