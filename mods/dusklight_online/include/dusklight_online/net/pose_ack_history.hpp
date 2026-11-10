#pragma once

#include <cstdint>
#include <set>
#include <span>

namespace dusklight_online::net {

using PoseAckHistory = std::set<uint32_t>;

inline void remember_pose_ack(PoseAckHistory& history, uint32_t sequence) {
    if (!sequence) return;
    history.insert(sequence);
    while (history.size() > 300) history.erase(history.begin());
}

// Pose ACKs acknowledge one decoded snapshot, not every preceding sequence.
// A broadcast delta must reference an exact snapshot held by every recipient.
inline uint32_t common_pose_ack(std::span<const PoseAckHistory* const> histories) {
    if (histories.empty()) return 0;
    const PoseAckHistory* smallest = nullptr;
    for (const auto* history : histories) {
        if (!history || history->empty()) return 0;
        if (!smallest || history->size() < smallest->size()) smallest = history;
    }
    for (auto it = smallest->rbegin(); it != smallest->rend(); ++it) {
        bool shared = true;
        for (const auto* history : histories) {
            if (!history->contains(*it)) { shared = false; break; }
        }
        if (shared) return *it;
    }
    return 0;
}

} // namespace dusklight_online::net
