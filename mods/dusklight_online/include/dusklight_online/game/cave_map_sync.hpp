#pragma once

#include <cstdint>
#include <string_view>

namespace dusklight_online::game {

// These switches gate MPAT geometry inside a single room; the save2 visited
// bit alone does not reveal it. Match the SwAreaS placements, including their
// event-free, set-only trigger settings. Other SWC00 actors remain local.
// Stage names matter: several caves share the same stage save table.
inline bool is_cave_map_reveal(bool areaSwitch, std::string_view stageName,
        int stage, int room, int flag, uint32_t params, int angleX,
        bool wasSet, bool set) {
    if (!areaSwitch || wasSet || !set || room != 0) return false;

    if (stageName == "D_SB10" && stage == 2 && angleX == 0x01FF &&
        params == (0xFF00FF00U | static_cast<uint32_t>(flag))) {
        // Faron Woods Cave: 19 sections, SwAreaS type 1.
        switch (flag) {
        case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38:
        case 0x43: case 0x46: case 0x4F: case 0x50: case 0x51: case 0x52:
        case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x59:
        case 0x7B:
            return true;
        default:
            return false;
        }
    }

    // Lantern caves: SwAreaS type 15 (sets the bit, then deletes itself).
    if (angleX != 0x0FFF || params != (0xFF10FF00U | static_cast<uint32_t>(flag)))
        return false;
    return (stageName == "D_SB02" && stage == 25 && flag >= 0x64 && flag <= 0x7E) ||
           (stageName == "D_SB03" && stage == 26 && flag >= 0x01 && flag <= 0x29);
}

} // namespace dusklight_online::game
