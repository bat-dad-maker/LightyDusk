#pragma once

#include <cstdint>
#include <set>

namespace dusklight_online::game {

// Resolve handles each time: the engine recycles emitter addresses. A dead
// remote emitter must never cause an unrelated particle to receive our state.
class ParticleDepthOwners {
public:
    template <typename Resolve>
    void add(uint32_t id, Resolve resolve) {
        contains(nullptr, resolve); // Retire expired handles even between draws.
        if (id != 0 && resolve(id) != nullptr) ids_.insert(id);
    }

    template <typename Resolve>
    bool contains(const void* emitter, Resolve resolve) {
        bool found = false;
        for (auto it = ids_.begin(); it != ids_.end();) {
            const void* current = resolve(*it);
            if (current == nullptr) {
                it = ids_.erase(it);
            } else {
                found |= emitter != nullptr && current == emitter;
                ++it;
            }
        }
        return found;
    }

    void clear() { ids_.clear(); }

private:
    std::set<uint32_t> ids_;
};
} // namespace dusklight_online::game
