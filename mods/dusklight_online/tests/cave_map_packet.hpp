#pragma once

#include <nlohmann/json.hpp>

// A Faron Woods Cave section from the actual allowlisted trigger data.
// Keep every source field: losing one makes the game adapter reject it.
inline nlohmann::json cave_map_packet() {
    return {{"type", "switch_bit"}, {"stage", 2}, {"flag", 51}, {"set", true},
        {"source_actor", 0x225}, // fpcNm_SWC00_e
        {"source_stage", "D_SB10"}, {"source_room", 0},
        {"source_params", 0xFF00FF33U}, {"source_angle_x", 0x01FF}};
}
