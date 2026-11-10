#include "dusklight_online/game/bomb_bag_sync.hpp"
#include <stdexcept>

using dusklight_online::game::should_publish_bomb_bag;
using dusklight_online::game::save_warp_bag_candidate;
using dusklight_online::game::save_warp_bag_converted;
void require(bool condition) {
    if (!condition) throw std::runtime_error("Bomb bag sync regression");
}
int main() {
    constexpr int normal = 1, water = 2;
    // Death/glitch retains the exact bag contents but removes rental status.
    require(should_publish_bomb_bag(false, true, false, normal, 10, true, normal, 10));
    // Once observed as permanent, it must not repeatedly announce the bag.
    require(!should_publish_bomb_bag(false, true, false, normal, 10, false, normal, 10));
    require(!should_publish_bomb_bag(false, true, true, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(true, true, false, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(false, false, false, normal, 10, true, normal, 10));
    require(!should_publish_bomb_bag(false, true, false, normal, 0, true, normal, 10));
    // Preserve existing acquisition, type-change and empty-refill behaviour.
    require(should_publish_bomb_bag(false, true, false, normal, 10, false, -1, 0));
    require(should_publish_bomb_bag(false, true, false, water, 10, false, normal, 10));
    require(should_publish_bomb_bag(false, true, false, normal, 10, false, normal, 0));
    require(!should_publish_bomb_bag(false, true, false, normal, 20, false, normal, 10));
    require(!should_publish_bomb_bag(false, true, false, normal, 5, false, normal, 10));

    // Save-warp cannot observe a rental-to-permanent edge, but the specific
    // rental of a previously empty slot can be checked after that file loads.
    constexpr int empty = 255;
    require(save_warp_bag_candidate(false, true, 1, empty, empty, 30));
    require(!save_warp_bag_candidate(false, true, 1, normal, empty, 30));
    require(!save_warp_bag_candidate(false, true, 1, empty, empty, 0));
    require(!save_warp_bag_candidate(true, true, 1, empty, empty, 30));
    require(!save_warp_bag_candidate(false, true, 3, empty, empty, 30));
    require(save_warp_bag_converted(false, true, 1, normal, -1, normal, 30));
    require(!save_warp_bag_converted(false, true, 1, normal, 1, normal, 30));
    require(!save_warp_bag_converted(false, true, 1, normal, -1, water, 30));
    require(!save_warp_bag_converted(false, true, 1, normal, -1, normal, 0));
    require(!save_warp_bag_converted(true, true, 1, normal, -1, normal, 30));
}
