#include "dusklight_online/game/trade_item_sync.hpp"

#include <cassert>

using namespace dusklight_online::game::trade_item;

int main() {
    assert(phase_for_change(kNone, kFirst) == 0);
    for (int item = kFirst; item <= kLast; ++item) {
        const int acquired = (item - kFirst) * 2;
        assert(item_phase(item) == acquired);
        assert(phase_for_change(kNone, item) == acquired);
        assert(phase_for_change(item, kNone) == acquired + 1);
        assert(valid_state(acquired, item));
        assert(valid_state(acquired + 1, kNone));
        assert(!valid_state(acquired + 1, item));
    }
    assert(phase_for_change(kNone, kNone) == -1);
    assert(phase_for_change(kFirst, 0x42) == -1);
    assert(!valid_state(-1, kNone));
    assert(!valid_state(kLastPhase + 1, kNone));

    // A receiver already holding the Invoice ignores a delayed Letter hand-in;
    // another receiver with the Letter accepts it. Equal phases repair a stale
    // slot after a flag reached the receiver before the inventory mutation.
    assert(!should_apply(2, phase_for_change(kFirst, kNone)));
    assert(should_apply(0, phase_for_change(kFirst, kNone)));
    assert(should_apply(5, phase_for_change(kFirst + 2, kNone)));
}
