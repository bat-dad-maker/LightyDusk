#include "dusklight_online/game/randomizer_item_names.hpp"

#include <cassert>

int main() {
    using dusklight_online::game::randomizer_item_name;

    assert(randomizer_item_name(0x01) == "Green Rupee");
    assert(randomizer_item_name(0x3F) == "Progressive Sword");
    assert(randomizer_item_name(0x85) == "Forest Temple Small Key");
    assert(randomizer_item_name(0xA5) == "Progressive Mirror Shard");
    assert(randomizer_item_name(0xDB) == "Progressive Mirror Shard");
    assert(randomizer_item_name(0xD8) == "Progressive Fused Shadow");
    assert(randomizer_item_name(0xE1) == "Progressive Hidden Skill");
    assert(randomizer_item_name(0xE7) == "Progressive Hidden Skill");
    assert(randomizer_item_name(0xE9) == "Progressive Sky Book");
    assert(randomizer_item_name(0xEA) == "Progressive Sky Book");
    assert(randomizer_item_name(0xEB) == "Progressive Sky Book");
    assert(randomizer_item_name(0xFA) == "Goron Mines Key Shard");
    assert(randomizer_item_name(0xFD) == "Goron Mines Key Shard");
    assert(randomizer_item_name(0xFE) == "Faron Woods Coro Key");
    assert(randomizer_item_name(0xFF) == "Unknown Item");
}
