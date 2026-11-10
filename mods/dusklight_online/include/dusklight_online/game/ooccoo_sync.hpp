#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dusklight_online::game::ooccoo {

// Values are checked against the game's enums in game_adapter.cpp. 0x33 is a
// grant operation (EXIT_2), NOT a fifth inventory form.
inline constexpr int Parent = 0x25;
inline constexpr int Junior = 0x27;
inline constexpr int Note = 0x2D;
inline constexpr int ParentAgain = 0x33;
inline constexpr int CityParent = 0xEC;
inline constexpr int None = 0xFF;
inline constexpr int FirstDungeon = 16;
inline constexpr int CityDungeon = 22;
inline constexpr uint8_t DungeonMask = 0x7F;
inline constexpr uint16_t CityReceipt = 0x80;
inline constexpr uint16_t NoteReceipt = 0x100;
inline constexpr std::array<std::string_view, 7> DungeonNames = {
    "D_MN05", "D_MN04", "D_MN01", "D_MN10", "D_MN11", "D_MN06", "D_MN07",
};

constexpr bool is_dungeon(int stage) {
    return stage >= FirstDungeon && stage <= CityDungeon;
}
constexpr uint8_t dungeon_bit(int stage) {
    return is_dungeon(stage) ? static_cast<uint8_t>(1U << (stage - FirstDungeon)) : 0;
}
constexpr bool is_form(int item) {
    return item == Parent || item == Junior || item == Note || item == CityParent;
}
constexpr bool is_grant(int item) {
    return is_form(item) || item == ParentAgain;
}

struct Scene {
    std::string_view name;
    int saveTable = -1;
    int type = -1; // ST_DUNGEON = 1, ST_BOSS_ROOM = 3; caves can also have type 1.
    int room = -1;
};

// Save-table equality or ST_DUNGEON alone is insufficient: caves and shops
// require different behavior. Unknown stages fail closed, even with a valid ID.
constexpr int scene_dungeon(const Scene& scene, bool allowBoss = true) {
    if (!is_dungeon(scene.saveTable) || scene.room < 0 || scene.room > 63) return -1;
    const auto name = DungeonNames[static_cast<std::size_t>(scene.saveTable - FirstDungeon)];
    const bool arena = scene.name.size() == name.size() + 1 &&
        scene.name.substr(0, name.size()) == name;
    const bool boss = arena && scene.name.back() == 'A';
    const bool miniboss = arena && scene.name.back() == 'B';
    if (scene.type == 1 && (scene.name == name || miniboss)) return scene.saveTable;
    if (allowBoss && scene.type == 3 && (boss || miniboss)) return scene.saveTable;
    return -1;
}
constexpr bool city_shop(const Scene& scene) {
    return scene_dungeon(scene, false) == CityDungeon && scene.room == 16;
}

// This is a LOCAL validation input, never a wire/save-companion structure.
struct ReturnMark {
    int owner = -1;
    std::string_view stageName;
    int room = -1;
    double x = 0, y = 0, z = 0;
};
inline int return_owner(const ReturnMark& mark) {
    if (!is_dungeon(mark.owner) || mark.room < 0 || mark.room > 63 ||
        !std::isfinite(mark.x) || !std::isfinite(mark.y) || !std::isfinite(mark.z)) {
        return -1;
    }
    const auto name = DungeonNames[static_cast<std::size_t>(mark.owner - FirstDungeon)];
    const bool miniboss = mark.stageName.size() == name.size() + 1 &&
        mark.stageName.substr(0, name.size()) == name && mark.stageName.back() == 'B';
    return mark.stageName == name || miniboss ? mark.owner : -1;
}

// A validated native return mark captured only after Ooccoo's warpOutProc.
// The receiving player did not perform that warp, so vanilla has no return
// mark to pair with Jr. Stage names are reconstructed from the
// allowlist; the wire never supplies an arbitrary warp destination name.
struct ReturnAnchor {
    bool valid = false;
    uint8_t variant = 0; // 0 = dungeon, 1 = known miniboss substage B.
    uint8_t room = 0;
    float x = 0, y = 0, z = 0;
    int16_t angle = 0;
    bool operator==(const ReturnAnchor&) const = default;
};

constexpr bool valid_anchor(int owner, const ReturnAnchor& anchor) {
    return is_dungeon(owner) && anchor.valid && anchor.variant <= 1 && anchor.room <= 63 &&
        std::isfinite(anchor.x) && std::isfinite(anchor.y) && std::isfinite(anchor.z) &&
        std::abs(anchor.x) <= 10000000.0f && std::abs(anchor.y) <= 10000000.0f &&
        std::abs(anchor.z) <= 10000000.0f;
}

constexpr ReturnAnchor choose_anchor(int owner, ReturnAnchor a, ReturnAnchor b) {
    if (!valid_anchor(owner, a)) return valid_anchor(owner, b) ? b : ReturnAnchor{};
    if (!valid_anchor(owner, b)) return a;
    // Concurrent native warps must converge regardless of packet order.
    if (a.variant != b.variant) return a.variant < b.variant ? a : b;
    if (a.room != b.room) return a.room < b.room ? a : b;
    if (a.x != b.x) return a.x < b.x ? a : b;
    if (a.y != b.y) return a.y < b.y ? a : b;
    if (a.z != b.z) return a.z < b.z ? a : b;
    return a.angle <= b.angle ? a : b;
}

// Acquisition/completion and actual warp-out return anchors cross the network.
// Acquisition receipts and return anchors are shared. Completion is carried on
// the wire for compatibility, but local boss flags decide whether it applies.
// The special City item does not set vanilla's ordinary OOCCOO_NOTE flag.
struct Progress {
    uint8_t acquired = 0;
    uint8_t completed = 0;
    bool citySpecial = false;
    bool unboundNote = false; // A randomized/legacy item without a proven owner.
    std::array<ReturnAnchor, 7> anchors{};
    bool operator==(const Progress&) const = default;
};
constexpr Progress join(Progress a, Progress b) {
    Progress result{static_cast<uint8_t>(a.acquired | b.acquired),
                    static_cast<uint8_t>(a.completed | b.completed),
                    a.citySpecial || b.citySpecial, a.unboundNote || b.unboundNote};
    for (int stage = FirstDungeon; stage <= CityDungeon; ++stage) {
        const auto index = static_cast<std::size_t>(stage - FirstDungeon);
        if (result.acquired & dungeon_bit(stage))
            result.anchors[index] = choose_anchor(stage, a.anchors[index], b.anchors[index]);
    }
    return result;
}
constexpr uint16_t entitlements(Progress p) {
    const uint8_t active = static_cast<uint8_t>(p.acquired & ~p.completed & DungeonMask);
    return active |
        ((p.citySpecial && !(p.completed & dungeon_bit(CityDungeon)) &&
          !(active & dungeon_bit(CityDungeon))) ? CityReceipt : 0) |
        (p.unboundNote ? NoteReceipt : 0);
}

// Called at a real ItemService give, not when a player changes rooms, uses
// Ooccoo, opens the ring, dies, or receives a raw item/flag packet.
constexpr Progress receipt_for_grant(int item, const Scene& scene) {
    Progress result;
    if (!is_grant(item)) return result;
    const int owner = scene_dungeon(scene);
    if ((item == Parent || item == ParentAgain) && is_dungeon(owner)) {
        result.acquired = dungeon_bit(owner);
    } else if (item == CityParent) {
        result.citySpecial = true;
    } else {
        // No cave ownership and no invented warp destination. In particular,
        // a randomized Jr. cannot transfer the giver's personal return point.
        result.unboundNote = true;
    }
    return result;
}

struct Projection {
    int item;
    bool resetReturn = false;
    bool applied = false; // false keeps pending delivery across unsafe frames.
    int installReturnStage = -1;
};

class State {
public:
    [[nodiscard]] Progress progress() const { return progress_; }
    [[nodiscard]] uint16_t pending() const { return pending_; }
    [[nodiscard]] bool managed_form() const { return managedForm_; }
    void reset() { progress_ = {}; pending_ = 0; managedForm_ = false; }

    // Save hydration and actual local grants don't redeliver an already owned
    // item. The engine has performed the local give itself.
    void observe_native_completion(uint8_t completed) {
        progress_.completed = static_cast<uint8_t>(completed & DungeonMask);
        pending_ &= entitlements(progress_);
    }
    void seed(Progress facts) {
        const auto nativeCompleted = progress_.completed;
        progress_ = join(progress_, facts);
        progress_.completed = nativeCompleted;
        pending_ &= entitlements(progress_);
    }
    void record_local(Progress facts) {
        seed(facts);
        pending_ &= static_cast<uint16_t>(~entitlements(facts));
        if (entitlements(facts) != 0) managedForm_ = false;
    }
    bool merge_remote(Progress facts) {
        const auto before = progress_;
        const auto oldEntitlements = entitlements(before);
        progress_ = join(progress_, facts);
        // A peer's cached completion is not evidence about this save's boss flag.
        progress_.completed = before.completed;
        const auto available = entitlements(progress_);
        pending_ = static_cast<uint16_t>((pending_ | (available & ~oldEntitlements)) & available);
        return before != progress_;
    }
    void restore(Progress facts, uint16_t pending, bool managedForm = false) {
        progress_ = facts;
        progress_.completed = 0; // Re-read from the selected game's native flags.
        pending_ = static_cast<uint16_t>(pending & entitlements(progress_));
        managedForm_ = managedForm;
    }

    // The caller owns the current inventory and validated local return mark.
    // An outside recipient gets Note on pickup; Jr only when an actual native
    // warp-out later supplies a return mark, which the adapter installs.
    Projection reconcile(const Scene& scene, int current, int localReturnOwner, bool safe) {
        if (!safe || (current != None && !is_form(current))) return {current};
        // Native boss exits/demo saves can remove Ooccoo. Old receipts must not
        // resurrect her every tick or every time the same snapshot is received.
        if (current == None && pending_ == 0) {
            managedForm_ = false;
            return {None, false, true};
        }

        const int here = scene_dungeon(scene);
        const uint8_t hereBit = dungeon_bit(here);
        const bool completeHere = (progress_.completed & hereBit) != 0;
        const bool regularHere = (progress_.acquired & hereBit) != 0 && !completeHere;
        const bool specialHere = here == CityDungeon && progress_.citySpecial && !completeHere;
        const bool activeSpecial = progress_.citySpecial &&
            !(progress_.completed & dungeon_bit(CityDungeon));
        const bool validReturn = is_dungeon(localReturnOwner) &&
            !(progress_.completed & dungeon_bit(localReturnOwner));
        const uint8_t active = static_cast<uint8_t>(progress_.acquired &
            ~progress_.completed & DungeonMask);
        int anchoredStage = -1;
        for (int stage = FirstDungeon; stage <= CityDungeon; ++stage) {
            if ((active & dungeon_bit(stage)) &&
                valid_anchor(stage, progress_.anchors[stage - FirstDungeon])) {
                anchoredStage = stage;
                break;
            }
        }
        int desired = Note;
        int installReturnStage = -1;
        const bool remoteProjection = managedForm_ || pending_ != 0;
        if (current == Junior && validReturn && (!regularHere || city_shop(scene))) {
            desired = Junior; // Never borrow, rewrite or clear another player's mark.
        } else if (regularHere) {
            desired = Parent;
        } else if (specialHere) {
            desired = CityParent;
        } else if (current == CityParent && progress_.citySpecial &&
                   !(progress_.completed & dungeon_bit(CityDungeon))) {
            // Vanilla playerInit only converts ordinary Sr. to a Note outside
            // dungeons. Preserve an already-held special City item, but don't
            // give that form to a remote recipient outside the City.
            desired = CityParent;
        } else if (remoteProjection && anchoredStage >= 0) {
            desired = Junior;
            if (!validReturn) installReturnStage = anchoredStage;
        } else if (((current == Parent || current == CityParent) && completeHere) ||
                   (current == Junior && is_dungeon(localReturnOwner) && !validReturn)) {
            // A different, newly received dungeon still deserves its Note.
            desired = pending_ != 0 ? Note : None;
        } else if (active == 0 && !activeSpecial && !progress_.unboundNote &&
                   managedForm_) {
            // A Note created by an earlier remote projection must go when its
            // dungeon is complete, even if this player never entered it.
            desired = None;
        }
        if (pending_ != 0 && desired != None) managedForm_ = true;
        if (desired == None) managedForm_ = false;
        pending_ = 0;
        return {desired, current == Junior && desired != Junior, true, installReturnStage};
    }

private:
    Progress progress_;
    uint16_t pending_ = 0;
    bool managedForm_ = false;
};

} // namespace dusklight_online::game::ooccoo
