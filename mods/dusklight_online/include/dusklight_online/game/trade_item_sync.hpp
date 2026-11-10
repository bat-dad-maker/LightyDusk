#pragma once

namespace dusklight_online::game::trade_item {

// The five vanilla Ilia quest items share one inventory slot. An empty slot
// after an item is ordered before the next acquisition.
inline constexpr int kNone = 0xFF;
inline constexpr int kFirst = 0x80;
inline constexpr int kLast = 0x84;
inline constexpr int kLastPhase = (kLast - kFirst) * 2 + 1;

constexpr int item_phase(int item) {
    return item >= kFirst && item <= kLast ? (item - kFirst) * 2 : -1;
}

constexpr int phase_for_change(int previous, int current) {
    if (item_phase(current) >= 0) return item_phase(current);
    if (current == kNone && item_phase(previous) >= 0)
        return item_phase(previous) + 1;
    return -1;
}

constexpr int item_for_phase(int phase) {
    return phase >= 0 && phase <= kLastPhase && phase % 2 == 0
        ? kFirst + phase / 2 : kNone;
}

constexpr bool valid_state(int phase, int item) {
    return phase >= 0 && phase <= kLastPhase && item == item_for_phase(phase);
}

// Equal phases repair a stale slot; an earlier player's delayed transition
// must never replace a later quest item or undo a completed hand-in.
constexpr bool should_apply(int localPhase, int remotePhase) {
    return remotePhase >= localPhase;
}

}  // namespace dusklight_online::game::trade_item
