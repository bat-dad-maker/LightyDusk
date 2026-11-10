#include "dusklight_online/game/cave_map_sync.hpp"

#include <array>
#include <stdexcept>

using dusklight_online::game::is_cave_map_reveal;

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("Cave map sync regression");
}

struct Cave {
    const char* name;
    int table;
    uint32_t params;
    int angle;
};
constexpr Cave faron{"D_SB10", 2, 0xFF00FF00U, 0x01FF};
constexpr Cave gorge{"D_SB02", 25, 0xFF10FF00U, 0x0FFF};
constexpr Cave lake{"D_SB03", 26, 0xFF10FF00U, 0x0FFF};

bool reveal(const Cave& cave, int flag, bool wasSet = false, bool set = true) {
    return is_cave_map_reveal(true, cave.name, cave.table, 0, flag,
        cave.params | static_cast<uint32_t>(flag), cave.angle, wasSet, set);
}
} // namespace

int main() {
    // Independent map-section fixtures from the three cave layouts. Check all
    // other stage switches too: a range expansion must not admit a story flag.
    constexpr std::array faronSections{
        51, 52, 53, 54, 55, 56, 67, 70, 79, 80, 81, 82, 83, 84, 85, 86, 87, 89, 123};
    for (int flag = -1; flag <= 256; ++flag) {
        bool expected = false;
        for (int section : faronSections) expected |= section == flag;
        require(reveal(faron, flag) == expected);
        require(reveal(gorge, flag) == (flag >= 100 && flag <= 126));
        require(reveal(lake, flag) == (flag >= 1 && flag <= 41));
    }

    for (const Cave& cave : {faron, gorge, lake}) {
        const int flag = cave.table == 2 ? 51 : cave.table == 25 ? 100 : 1;
        const uint32_t params = cave.params | static_cast<uint32_t>(flag);

        // Faron's type-1 area trigger keeps calling onSwitch while Link is
        // inside it. Ten seconds of setter calls must yield just one message.
        int sent = 0;
        bool localBit = false;
        for (int frame = 0; frame < 600; ++frame) {
            const bool before = localBit;
            localBit = true;
            sent += reveal(cave, flag, before, localBit);
        }
        require(sent == 1);
        // Applying a remote reveal sets the same native bit; walking through
        // that trigger afterward must not echo it back.
        require(!reveal(cave, flag, true, true));
        require(!reveal(cave, flag, true, false));
        require(!reveal(cave, flag, false, false));

        require(!is_cave_map_reveal(false, cave.name, cave.table, 0, flag,
            params, cave.angle, false, true));
        require(!is_cave_map_reveal(true, cave.name, cave.table + 1, 0, flag,
            params, cave.angle, false, true));
        for (int room : {-1, 1, 10, 63, 64}) {
            require(!is_cave_map_reveal(true, cave.name, cave.table, room, flag,
                params, cave.angle, false, true));
        }
        for (const char* name : {"", "F_SP108", "D_SB00", "D_SB01", "D_SB04"}) {
            require(!is_cave_map_reveal(true, name, cave.table, 0, flag,
                params, cave.angle, false, true));
        }
        // Reject event-bearing, conditional, mismatched-switch and changed
        // trigger placements, even if they touch an allowed map bit.
        for (uint32_t changed : {params ^ 0x01000000U, params ^ 0x100U,
                                params ^ 1U, params ^ 0x40000U}) {
            require(!is_cave_map_reveal(true, cave.name, cave.table, 0, flag,
                changed, cave.angle, false, true));
        }
        for (int angle : {-1, 0x00FF, 0x02FF, 0x01FE, 0x0FFE}) {
            require(!is_cave_map_reveal(true, cave.name, cave.table, 0, flag,
                params, angle, false, true));
        }
    }
}
