#include "dusklight_online/game/ooccoo_sync.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace dusklight_online::game::ooccoo;

namespace {
unsigned checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAILED: " << message << " (check " << checks << ")\n";
        std::exit(1);
    }
}
constexpr Scene forest{"D_MN05", 16, 1, 0};
constexpr Scene mines{"D_MN04", 17, 1, 3};
constexpr Scene city{"D_MN07", 22, 1, 0};
constexpr Scene shop{"D_MN07", 22, 1, 16};
constexpr Scene field{"F_SP108", 3, 0, 0};
constexpr Scene cave{"D_SB02", 25, 1, 0};
constexpr Progress forestReceipt{1, 0, false, false};
constexpr Progress minesReceipt{2, 0, false, false};
constexpr Progress cityReceipt{64, 0, false, false};
constexpr Progress specialReceipt{0, 0, true, false};
constexpr ReturnAnchor forestAnchor{true, 0, 0, 10.0f, 20.0f, 30.0f, 0x1234};

Progress anchored_forest_receipt() {
    Progress receipt = forestReceipt;
    receipt.anchors[0] = forestAnchor;
    return receipt;
}

void scene_identity() {
    for (int stage = -10; stage < 50; ++stage) {
        check(is_dungeon(stage) == (stage >= 16 && stage <= 22), "owner allowlist");
        for (const auto name : {"D_SB02", "D_SB03", "D_SB10", "D_SB00", "D_SB01", "F_SP108",
                                "D_MN08", "D_MN09", "D_MN05fake", ""}) {
            check(scene_dungeon({name, stage, 1, 0}) == -1, "cave/field/unknown cannot own Ooccoo");
            const auto r = receipt_for_grant(Parent, {name, stage, 1, 0});
            check(r.acquired == 0 && r.unboundNote, "unproven randomized reward is Note only");
        }
    }
    for (int stage = 16; stage <= 22; ++stage) {
        const auto name = DungeonNames[static_cast<std::size_t>(stage - 16)];
        const std::string boss = std::string(name) + 'A';
        const std::string miniboss = std::string(name) + 'B';
        check(scene_dungeon({name, stage, 1, 0}) == stage, "primary scene mapping");
        check(scene_dungeon({boss, stage, 3, 50}) == stage, "boss scene belongs to same dungeon");
        check(scene_dungeon({boss, stage, 3, 50}, false) == -1, "no boss-room reunion");
        check(scene_dungeon({miniboss, stage, 1, 51}) == stage, "dungeon-type miniboss scene");
        check(scene_dungeon({miniboss, stage, 3, 51}) == stage, "boss-type miniboss scene");
        check(scene_dungeon({name, stage, 0, 0}) == -1, "stage type checked");
        check(scene_dungeon({name, stage, 1, 64}) == -1, "room upper bound checked");
        check(scene_dungeon({name, stage, 1, -1}) == -1, "room lower bound checked");
        check(scene_dungeon({name, stage + 1, 1, 0}) == -1, "name must match table");
        check(receipt_for_grant(ParentAgain, {miniboss, stage, 3, 51}).acquired == dungeon_bit(stage),
              "subsequent/rando arena grant is canonical ordinary acquisition");
    }
    check(city_shop(shop) && !city_shop(city), "City shop is room-aware");
    check(receipt_for_grant(CityParent, field).citySpecial, "special item ID identifies City, not giver area");
    check(!receipt_for_grant(CityParent, city).acquired, "City special doesn't set normal warp bit");
    check(receipt_for_grant(Junior, forest).unboundNote, "Jr grant never shares a return point");
    check(!is_form(ParentAgain) && is_grant(ParentAgain), "EXIT_2 is not an inventory form");
    check(receipt_for_grant(0x40, forest) == Progress{}, "unrelated item is ignored");
}

void inventory_and_travel() {
    State receiver;
    receiver.merge_remote(forestReceipt);
    check(receiver.reconcile(cave, None, -1, true).item == Note, "remote pickup while in cave gives Note");
    check(receiver.reconcile(mines, Note, -1, true).item == Note, "A acquisition doesn't adapt to unacquired B");
    check(receiver.reconcile(forest, Note, -1, true).item == Parent, "enter owned A restores Sr");
    check(receiver.reconcile(field, Parent, -1, true).item == Note, "walk out converts Sr to Note");
    receiver.merge_remote(minesReceipt);
    check(receiver.reconcile(mines, Note, -1, true).item == Parent, "B's own acquisition enables B");
    check(receiver.reconcile(forest, Note, -1, true).item == Parent, "A remains owned after B");

    const auto beforeTravel = receiver.progress();
    auto projected = receiver.reconcile(cave, Junior, 16, true);
    check(projected.item == Junior && !projected.resetReturn, "local Jr retains local warp in cave");
    receiver.merge_remote(cityReceipt);
    projected = receiver.reconcile(field, Junior, 16, true);
    check(projected.item == Junior && !projected.resetReturn, "peer acquisition cannot replace local Jr outside");
    projected = receiver.reconcile(mines, Junior, 16, true);
    check(projected.item == Parent && projected.resetReturn, "own B reunion supersedes local A return");
    check((receiver.progress().acquired & beforeTravel.acquired) == beforeTravel.acquired,
          "local travel doesn't remove receipts");
    State noReceipt;
    projected = noReceipt.reconcile(cave, Junior, -1, true);
    check(projected.item == Note && projected.resetReturn, "invalid Jr is not a working warp");
    check(noReceipt.reconcile(cave, Parent, -1, true).item == Note, "repair legacy Sr in a cave");
    check(noReceipt.progress() == Progress{}, "repairing an item cannot claim the cave");

    State sameDungeon;
    sameDungeon.merge_remote(forestReceipt);
    check(sameDungeon.reconcile(forest, None, -1, true).item == Parent, "same dungeon recipient gets Sr");
    check(sameDungeon.reconcile({"D_MN05A", 16, 3, 50}, Parent, -1, true).item == Parent,
          "boss room inventory retained; vanilla still controls usability");
    check(sameDungeon.reconcile({"D_MN05B", 16, 3, 51}, Parent, -1, true).item == Parent,
          "miniboss substage is not treated as a cave");
}

void remote_junior_and_completion() {
    State outside;
    outside.merge_remote(forestReceipt);
    check(outside.reconcile(field, None, -1, true).item == Note,
          "remote pickup outside gives Note until an actual warp-out");
    outside.merge_remote(anchored_forest_receipt());
    const auto given = outside.reconcile(field, Note, -1, true);
    check(given.item == Junior && given.installReturnStage == 16,
          "native warp-out upgrades Note to Jr with its return destination");
    check(outside.managed_form(), "remote Jr is tracked separately from native inventory");
    const auto holding = outside.reconcile(field, Junior, 16, true);
    check(holding.item == Junior && holding.installReturnStage == -1,
          "valid Jr return mark is preserved between updates");
    outside.merge_remote({0, 1, false, false});
    check(outside.reconcile(field, Junior, 16, true).item == Junior,
          "remote cached completion alone cannot remove Jr");
    outside.observe_native_completion(1);
    const auto cleared = outside.reconcile(field, Junior, 16, true);
    check(cleared.item == None && cleared.resetReturn && !outside.managed_form(),
          "native boss flag removes Jr and clears the mark");
    outside.observe_native_completion(0);
    outside.merge_remote({0, 1, false, false});
    check(outside.reconcile(field, Junior, 16, true).item == Junior,
          "clearing the boss flag makes the return usable despite an old packet");

    State sameDungeon;
    sameDungeon.merge_remote(anchored_forest_receipt());
    check(sameDungeon.reconcile(forest, None, -1, true).item == Parent,
          "same-dungeon pickup still gives Sr");

    State lateAnchor;
    lateAnchor.merge_remote(forestReceipt);
    check(lateAnchor.reconcile(field, None, -1, true).item == Note,
          "old receipt without a safe anchor cannot create a broken Jr");
    lateAnchor.merge_remote(anchored_forest_receipt());
    check(lateAnchor.reconcile(field, Note, -1, true).item == Junior,
          "late anchor upgrades the provisional Note to Jr");
    lateAnchor.merge_remote({0, 1, false, false});
    check(lateAnchor.reconcile(field, Note, -1, true).item == Junior,
          "remote cached completion cannot remove a managed Note");
    lateAnchor.observe_native_completion(1);
    check(lateAnchor.reconcile(field, Note, -1, true).item == None,
          "managed Note clears after the native boss flag is set");

    State local;
    local.record_local(anchored_forest_receipt());
    check(local.reconcile(field, Parent, -1, true).item == Note,
          "local pickup still follows vanilla outside-dungeon Sr-to-Note behavior");

    State restored;
    restored.restore(anchored_forest_receipt(), 0, true);
    check(restored.reconcile(field, Note, -1, true).item == Junior,
          "saved remote item retains its synthetic return destination");

    auto anotherPickup = anchored_forest_receipt();
    anotherPickup.anchors[0].room = 5;
    check(join(anchored_forest_receipt(), anotherPickup) ==
          join(anotherPickup, anchored_forest_receipt()),
          "different valid pickup marks converge regardless of packet order");
}

void local_return_validation() {
    ReturnMark mark{16, "D_MN05", 0, 1, 2, 3};
    check(return_owner(mark) == 16, "valid local mark");
    mark.stageName = "D_MN05B"; mark.room = 51;
    check(return_owner(mark) == 16, "known miniboss return stays in owner's dungeon");
    mark.stageName = "D_SB02";
    check(return_owner(mark) == -1, "cave return rejected");
    mark.stageName = "D_MN04";
    check(return_owner(mark) == -1, "return name and owner must match");
    mark.stageName = "D_MN05A";
    check(return_owner(mark) == -1, "no synthesized boss warp");
    mark.stageName = "D_MN05"; mark.room = 64;
    check(return_owner(mark) == -1, "return room bounded");
    mark.room = -1;
    check(return_owner(mark) == -1, "uninitialized return room rejected");
    mark.room = 0; mark.owner = 25;
    check(return_owner(mark) == -1, "return owner cannot be cave");
    mark.owner = 16;
    for (auto bad : {std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
        mark.x = bad;
        check(return_owner(mark) == -1, "nonfinite coordinates rejected");
    }
}

void city_variants() {
    State state;
    state.merge_remote(specialReceipt);
    check(state.reconcile(field, None, -1, true).item == Note, "remote City special outside is Note");
    check(state.reconcile(city, Note, -1, true).item == CityParent, "City restores distinct special item");
    check(state.reconcile(field, CityParent, -1, true).item == CityParent,
          "existing special item follows vanilla's distinct playerInit rule");
    state.merge_remote(cityReceipt);
    check(state.reconcile(city, CityParent, -1, true).item == Parent, "normal City acquisition upgrades special");
    const auto junior = state.reconcile(shop, Junior, 22, true);
    check(junior.item == Junior && !junior.resetReturn, "City shop preserves Jr and its local mark");
    check(state.reconcile(city, Junior, 22, true).item == Parent, "City interior reunion restores ordinary Sr");
    state.merge_remote(specialReceipt);
    check(state.reconcile(city, Parent, -1, true).item == Parent, "old special packet cannot downgrade normal City");
    check(state.pending() == 0, "downgrade packet does not redeliver");
}

void lifecycle_and_completion() {
    State state;
    state.merge_remote(forestReceipt);
    check(!state.reconcile(forest, None, -1, false).applied && state.pending() != 0,
          "unsafe frame defers delivery");
    check(!state.reconcile(forest, 0x40, -1, true).applied && state.pending() != 0,
          "another mod's slot content is not clobbered");
    state.reconcile(forest, None, -1, true);
    check(state.pending() == 0, "safe delivery consumes pending entitlement");
    // Simulate vanilla boss-portal/demo cleanup without fabricating a new pickup.
    check(state.reconcile(forest, None, -1, true).item == None, "native deletion not resurrected per tick");
    state.merge_remote(forestReceipt);
    state.merge_remote({});
    check(state.reconcile(forest, None, -1, true).item == None, "old/empty snapshot cannot resurrect or clear");
    state.merge_remote(minesReceipt);
    check(state.reconcile(mines, None, -1, true).item == Parent, "genuinely new dungeon still delivers");

    State complete;
    complete.merge_remote({0, 1, false, false});
    check(complete.pending() == 0 && complete.progress().acquired == 0 &&
          complete.progress().completed == 0,
          "remote boss bit alone is neither collection nor local completion");
    complete.observe_native_completion(1);
    complete.merge_remote(forestReceipt);
    check(complete.pending() == 0, "completed dungeon dominates late acquisition");
    check(complete.reconcile(forest, Parent, -1, true).item == None, "matching completed dungeon removes Sr");
    check(complete.reconcile(field, Junior, 16, true).item == None, "completed return is not usable");
    check(complete.reconcile(field, Note, -1, true).item == Note, "no global deletion of unrelated Note");
    complete.merge_remote(minesReceipt);
    check(complete.reconcile(forest, Parent, -1, true).item == Note,
          "a new B acquisition still delivers when old A completes");
    complete.observe_native_completion(0);
    check(complete.reconcile(forest, Parent, -1, true).item == Parent,
          "native flag unset preserves Ooccoo even with cached completion");

    State delayed;
    delayed.merge_remote({0, 1, false, false});
    delayed.merge_remote(forestReceipt);
    check(delayed.pending() == 1,
          "completion packet arriving first cannot suppress an unfinished pickup");

    State pending;
    pending.merge_remote(forestReceipt);
    State restored;
    restored.restore(pending.progress(), pending.pending());
    check(restored.reconcile(cave, None, -1, true).item == Note, "save/load preserves deferred receipt");
    restored.restore(forestReceipt, 0x1FF);
    check(restored.pending() == 1, "saved pending bits limited to proven active receipts");
    restored.restore({1, 1, false, false}, 0);
    check(restored.progress().completed == 0, "saved completion cache is re-read from native flags");
    restored.reset(); // same operation used at save reset / flag-off / session reset
    check(restored.progress() == Progress{} && restored.pending() == 0, "scope reset clears pending and owner data");
    check(restored.reconcile(mines, None, -1, true).item == None, "new save cannot inherit old Ooccoo");

    State local;
    local.seed(forestReceipt);
    check(local.pending() == 0, "native save hydration is not a new pickup");
    local.merge_remote(minesReceipt);
    local.record_local(minesReceipt);
    check(local.pending() == 0, "local give satisfies a simultaneously queued remote delivery");
    const auto before = local.progress();
    local.reconcile(cave, Junior, 16, true);
    local.reconcile(field, Note, -1, true);
    check(local.progress() == before, "warp/UI state never becomes shared progression");
}

void convergence_properties() {
    // Exhaustive pairwise acquisition masks with legacy completion fields,
    // plus ordering all seven different dungeon receipts.
    for (unsigned a = 0; a < 128; ++a) for (unsigned b = 0; b < 128; ++b) {
        Progress x{static_cast<uint8_t>(a), static_cast<uint8_t>(b), false, false};
        Progress y{static_cast<uint8_t>(b), static_cast<uint8_t>(a), true, true};
        check(join(x, y) == join(y, x), "union commutative");
        check(join(x, x) == x, "union idempotent");
        check((entitlements(x) & b) == 0, "completed acquisitions never entitled");
        State left, right;
        left.merge_remote(x); left.merge_remote(y);
        right.merge_remote(y); right.merge_remote(x);
        check(left.progress() == right.progress() && left.pending() == right.pending() &&
              left.progress().completed == 0,
              "receipt order converges without accepting remote completion");
        check(left.reconcile(cave, None, -1, true).item != Junior,
              "remote data never creates a Jr");
    }
    std::array<int, 7> order{16,17,18,19,20,21,22};
    unsigned permutations = 0;
    do {
        State state;
        for (const int stage : order) {
            state.merge_remote({dungeon_bit(stage), 0, false, false});
            state.merge_remote({}); // old/empty snapshots interleaved
            state.merge_remote({dungeon_bit(stage), 0, false, false}); // retry
        }
        check(state.progress().acquired == 127 && state.pending() == 127,
              "concurrent pickups/retries converge");
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    check(permutations == 5040, "all acquisition permutations covered");
}
}

int main() {
    scene_identity();
    inventory_and_travel();
    remote_junior_and_completion();
    local_return_validation();
    city_variants();
    lifecycle_and_completion();
    convergence_properties();
    std::cout << "Ooccoo production model: " << checks << " checks passed.\n";
}
