#pragma once

#include <cstdint>
#include <string_view>

namespace dusklight_online::game {

// Mirrors the display names indexed by item ID in the randomizer's embedded
// generator/data/items.yaml. This is display-only; rewards still travel as
// their already-resolved numeric item IDs.
std::string_view randomizer_item_name(uint8_t itemId);

} // namespace dusklight_online::game
