#pragma once
#include <algorithm>
#include <cstdint>

namespace dusklight_online::game {
struct WetState {
    int timer = 0;
    int fade = 0;
};
// Native setWaterDropEffect: -1 holds wet; positive ticks count down, then
// alpha returns to zero. RGB is derived locally from this alpha.
constexpr WetState advance_wet_state(WetState state, uint32_t ticks) {
    ticks = std::min(ticks, uint32_t{170});
    while (ticks-- > 0) {
        if (state.timer > 0) --state.timer;
        if (state.timer != 0) state.fade = std::max(-20, state.fade - 2);
        else state.fade = std::min(0, state.fade + 1);
    }
    return state;
}
} // namespace dusklight_online::game
