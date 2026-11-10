#pragma once

namespace dusklight_online::game {
// A retained rental becoming permanent is an acquisition even when its
// item and nonzero ammunition count have not changed.
inline bool should_publish_bomb_bag(bool randomizer, bool syncable, bool rental,
        int item, int count, bool previousRental, int previousItem, int previousCount) {
    return !randomizer && syncable && !rental && count > 0 &&
           (previousRental || previousItem != item || previousCount == 0);
}

// Save-warp erases the frame-to-frame rental observation. Record only a rental
// that displaced an empty slot, then require that exact bag type to survive
// reloading as a permanent bag. Other saved inventory is never inferred as a
// new acquisition.
inline bool save_warp_bag_candidate(bool randomizer, bool syncable, int bag,
        int previousItem, int emptyItem, int count) {
    return !randomizer && syncable && bag >= 0 && bag < 3 &&
           previousItem == emptyItem && count > 0 && count <= 99;
}

inline bool save_warp_bag_converted(bool randomizer, bool syncable, int bag,
        int savedItem, int rentalBag, int loadedItem, int loadedCount) {
    return !randomizer && syncable && bag >= 0 && bag < 3 &&
           rentalBag != bag && loadedItem == savedItem &&
           loadedCount > 0 && loadedCount <= 99;
}
} // namespace dusklight_online::game
