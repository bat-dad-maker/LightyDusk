#include "dusklight_online/game/particle_depth.hpp"
#include <map>
#include <stdexcept>

int main() {
    using dusklight_online::game::ParticleDepthOwners;
    ParticleDepthOwners owners;
    int remote, local;
    std::map<uint32_t, void*> live{{1, &remote}, {2, &local}};
    auto resolve = [&](uint32_t id) -> void* {
        auto it = live.find(id);
        return it == live.end() ? nullptr : it->second;
    };
    auto require = [](bool ok) { if (!ok) throw std::runtime_error("particle ownership failure"); };
    owners.add(1, resolve);
    owners.add(0, resolve);
    owners.add(99, resolve);
    require(owners.contains(&remote, resolve));
    require(!owners.contains(&local, resolve));
    require(!owners.contains(nullptr, resolve));
    live.erase(1);
    live[3] = &remote; // Engine reuses the address for an unrelated emitter.
    require(!owners.contains(&remote, resolve));
    owners.add(3, resolve);
    require(owners.contains(&remote, resolve));
    owners.clear(); // Mod teardown must leave no ownership behind.
    require(!owners.contains(&remote, resolve));
}
