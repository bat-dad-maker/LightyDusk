#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

namespace dusklight_online::game {
struct PoePickup {
    int previous;
    int value;
    uint32_t sequence;
};

// Called at the actual item grant, never from a sampled save total. Network
// hydration and randomizer grants must not become new vanilla pickups.
inline std::optional<PoePickup> local_poe_pickup(int previous, int value,
        bool remote, bool randomizer, uint32_t& sequence, int maximum) {
    if (remote || randomizer || previous < 0 || value <= previous || value > maximum)
        return std::nullopt;
    if (++sequence == 0) ++sequence;
    return PoePickup{previous, value, sequence};
}

inline int merge_poe_pickup(int current, const PoePickup& pickup, int maximum) {
    if (pickup.sequence != 0 && pickup.previous >= 0 && current > pickup.previous)
        return std::min(current + (pickup.value - pickup.previous), maximum);
    return std::max(current, pickup.value);
}
} // namespace dusklight_online::game
