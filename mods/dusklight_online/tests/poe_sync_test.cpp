#include "dusklight_online/game/poe_sync.hpp"
#include <array>
#include <stdexcept>

using namespace dusklight_online::game;
void require(bool ok) { if (!ok) throw std::runtime_error("Poe synchronization regression"); }

int main() {
    constexpr int maximum = 60;
    uint32_t sequence = 0;
    const auto one = local_poe_pickup(0, 1, false, false, sequence, maximum);
    require(one && one->sequence == 1);

    // One genuine grant, six recipients. Readiness cannot create pickups:
    // incoming count changes have no item-grant notification to broadcast.
    std::array<int, 6> counts{1, 0, 0, 0, 0, 0};
    for (size_t i = 1; i < counts.size(); ++i) {
        const int previous = counts[i];
        counts[i] = merge_poe_pickup(counts[i], *one, maximum);
        // Even a remote operation that executes an item grant is suppressed.
        require(!local_poe_pickup(previous, counts[i], true, false, sequence, maximum));
    }
    for (int value : counts) require(value == 1);
    require(sequence == 1);

    // Independent players collecting distinct souls concurrently still add.
    uint32_t otherSequence = 0;
    const auto other = local_poe_pickup(0, 1, false, false, otherSequence, maximum);
    require(other.has_value());
    require(merge_poe_pickup(1, *other, maximum) == 2);
    require(merge_poe_pickup(1, *one, maximum) == 2);

    // A genuine pickup following remote catch-up is not swallowed by baseline
    // repair (including when both happen during the same cutscene).
    const auto afterCatchup = local_poe_pickup(1, 2, false, false, sequence, maximum);
    require(afterCatchup && afterCatchup->previous == 1 && afterCatchup->value == 2);
    require(merge_poe_pickup(1, *afterCatchup, maximum) == 2);
    require(merge_poe_pickup(20, {0, 1, 0}, maximum) == 20); // Legacy absolute repair.
    require(merge_poe_pickup(59, {0, 2, 1}, maximum) == 60);
    require(!local_poe_pickup(1, 1, false, false, sequence, maximum));
    require(!local_poe_pickup(2, 1, false, false, sequence, maximum));
    require(!local_poe_pickup(0, 1, false, true, sequence, maximum));
    require(!local_poe_pickup(60, 61, false, false, sequence, maximum));
}
