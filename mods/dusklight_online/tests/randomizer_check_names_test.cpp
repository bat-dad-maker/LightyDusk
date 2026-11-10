#include "dusklight_online/game/randomizer_check_names.hpp"

int main() {
    using dusklight_online::game::randomizer_check_display_name;
    return randomizer_check_display_name("freestanding:F_SP103:128") != "Ordon Bo Cliff Rupee" ||
        randomizer_check_display_name("chest:R_SP01:4") != "Wooden Sword Chest" ||
        randomizer_check_display_name("coro_bottle") != "Coro Bottle" ||
        randomizer_check_display_name("freestanding:F_SP103:255") != "" ||
        randomizer_check_display_name("") != "";
}
