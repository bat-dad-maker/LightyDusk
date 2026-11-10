#pragma once

#include <algorithm>
#include <cstddef>

namespace dusklight_online::game {
// Named rewards form a set, not an increment stream. An unidentified existing
// slot may already belong to an incoming reward; never add the two counts.
inline int merged_bottle_count(int local, int remote, std::size_t sourceCount) {
    return std::clamp(std::max({local, remote, static_cast<int>(sourceCount)}), 0, 4);
}

inline bool bottle_sources_exact(bool trusted, std::size_t sources, int bottles) {
    return trusted && static_cast<int>(sources) == bottles;
}
} // namespace dusklight_online::game
