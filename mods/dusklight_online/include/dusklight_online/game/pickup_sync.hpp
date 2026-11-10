#pragma once

#include <string_view>
#include <algorithm>
#include <cstdint>

namespace dusklight_online::game {

// 255 is TP's "no saved item bit" sentinel. ItemService still names such
// grants, but that name is shared by unrelated, repeatable pickups in a stage.
inline bool repeatable_pickup_check(std::string_view name) {
    return name.starts_with("freestanding:") && name.ends_with(":255");
}

// These IDs are ordinary health/ammo refills in Randomizer. Missing source
// metadata alone must never suppress equipment, keys or other progression.
inline bool local_only_randomizer_pickup(int item, std::string_view check) {
    if (!check.empty() && !repeatable_pickup_check(check)) return false;
    return item == 0x00 ||                    // Heart
           (item >= 0x0a && item <= 0x12) ||  // Bombs, arrows, slingshot seeds
           (item >= 0x16 && item <= 0x1d);    // Water bombs and bomblings
}

// Vanilla and Randomizer retain these seven currency IDs. Sharing their
// wallet effect and executing their remote item grant must not both add money.
inline int rupee_pickup_amount(int item) {
    constexpr int amounts[] = {0, 1, 5, 10, 20, 50, 100, 200};
    return item >= 1 && item <= 7 ? amounts[item] : 0;
}

enum class WalletTotalAction { Reject, Coalesce, Apply };

inline bool remote_pickup_requires_grant(int item) {
    return item >= 0 && item < 255 && rupee_pickup_amount(item) == 0;
}

// Preserve the AIO's pending-counter coalescing. Only skip the received
// assignment when the meter is already queued to reach that exact total.
inline WalletTotalAction wallet_total_action(int value, int current, int pending, int maximum) {
    if (value < 0 || value > maximum) return WalletTotalAction::Reject;
    const auto projected = std::clamp<int64_t>(int64_t(current) + pending, 0, maximum);
    return pending != 0 && value == projected ? WalletTotalAction::Coalesce :
                                              WalletTotalAction::Apply;
}

}  // namespace dusklight_online::game
