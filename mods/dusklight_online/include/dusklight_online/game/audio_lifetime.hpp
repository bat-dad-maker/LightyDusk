#pragma once

#include "dusk/multiplayer/multiplayer.hpp"
#include <algorithm>
#include <array>
#include <span>

namespace dusklight_online::game {

// An active snapshot is presence information, not a request to replay a sample.
// Remember consumed instances even after their local playback has completed.
struct AudioLifetimeTracker {
    std::array<uint32_t, 8> instances{};
    uint32_t lastSequence = 0;
    struct Changes {
        std::array<bool, 8> stop{};
        std::array<int, 8> start;
        Changes() { start.fill(-1); }
    };

    Changes update(std::span<const dusk::multiplayer::RemoteAudioEvent> events) {
        Changes changes;
        for (size_t slot = 0; slot < instances.size(); ++slot) {
            if (instances[slot] == 0) continue;
            const bool present = std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.tracked && event.sequence == instances[slot];
            });
            if (!present) {
                changes.stop[slot] = true;
                instances[slot] = 0;
            }
        }
        const auto previousSequence = lastSequence;
        for (size_t index = 0; index < events.size(); ++index) {
            const auto& event = events[index];
            if (!event.tracked || event.soundId == 0 || event.sequence == 0) continue;
            lastSequence = std::max(lastSequence, event.sequence);
            if (event.sequence <= previousSequence ||
                std::find(instances.begin(), instances.end(), event.sequence) != instances.end()) continue;
            auto free = std::find(instances.begin(), instances.end(), 0U);
            if (free == instances.end()) continue;
            const auto slot = static_cast<size_t>(free - instances.begin());
            *free = event.sequence;
            changes.start[slot] = static_cast<int>(index);
        }
        return changes;
    }

    void clear() { instances.fill(0); } // Keep replay protection across visibility changes.
};

} // namespace dusklight_online::game
