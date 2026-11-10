#pragma once

// Dusklight Co-op: engine-independent rules for shared enemy HP, two-player
// boss health and boss doors that need every player present.
//
// Everything in this header is pure logic with no game headers, so it is unit
// tested on the host (see tests/coop_rules_test.cpp). The game-side glue lives
// in src/game/coop_features.cpp.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dusklight_online::game::coop {

inline constexpr const char* kPresenceType = "coop_presence";
inline constexpr const char* kSettingsType = "coop_settings";
inline constexpr const char* kEnemyDamageType = "coop_enemy_damage";

// Game ticks run at 30 Hz.
inline constexpr uint64_t kPresenceIntervalTicks = 10;      // 3 updates per second
inline constexpr uint64_t kPresenceStaleTicks = 90;         // 3 seconds without news
inline constexpr uint64_t kSettingsIntervalTicks = 60;      // host re-announces every 2 s
inline constexpr uint64_t kDoorToastCooldownTicks = 150;    // 5 seconds between toasts
inline constexpr float kBossDoorPartnerRadius = 900.0f;     // ~9 metres of game space
inline constexpr float kEnemyMatchTolerance = 450.0f;
inline constexpr int kMaxDamagePerMessage = 1000;
inline constexpr int kMaxPlayersForScaling = 8;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline float distance(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline bool finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct Options {
    bool sharedHp = true;
    bool bossDoorTogether = true;

    bool operator==(const Options& other) const {
        return sharedHp == other.sharedHp && bossDoorTogether == other.bossDoorTogether;
    }
    bool operator!=(const Options& other) const { return !(*this == other); }
};

// The room owner decides for everyone so both games always agree. Until the
// owner's choice has arrived, a guest uses its own toggles.
inline Options effective_options(bool lobbyActive, bool isOwner, const Options& local,
                                 const std::optional<Options>& fromHost) {
    if (!lobbyActive || isOwner || !fromHost) return local;
    return *fromHost;
}

struct PartnerPresence {
    std::string stage;
    int room = -1;
    Vec3 pos;
    uint64_t seenTick = 0;
    uint32_t sequence = 0;
};

inline bool presence_fresh(const PartnerPresence& p, uint64_t nowTick) {
    return nowTick >= p.seenTick && nowTick - p.seenTick <= kPresenceStaleTicks;
}

// Number of other players currently loaded into the same stage.
inline int partners_in_stage(const std::map<std::string, PartnerPresence>& partners,
                             const std::string& stage, uint64_t nowTick) {
    if (stage.empty()) return 0;
    int count = 0;
    for (const auto& [peer, presence] : partners) {
        (void)peer;
        if (presence_fresh(presence, nowTick) && presence.stage == stage) ++count;
    }
    return count;
}

// True when at least one other player stands near the given boss door.
inline bool partner_near(const std::map<std::string, PartnerPresence>& partners,
                         const std::string& stage, const Vec3& doorPos, uint64_t nowTick,
                         float radius = kBossDoorPartnerRadius) {
    if (stage.empty() || !finite(doorPos)) return false;
    for (const auto& [peer, presence] : partners) {
        (void)peer;
        if (!presence_fresh(presence, nowTick) || presence.stage != stage) continue;
        if (!finite(presence.pos)) continue;
        if (distance(presence.pos, doorPos) <= radius) return true;
    }
    return false;
}

// A boss door only opens when the gate is off, nobody is connected, or a
// partner is standing at the door too. Solo play outside a room is untouched.
inline bool boss_door_should_block(bool lobbyActive, bool gateEnabled, bool partnerIsNear) {
    return lobbyActive && gateEnabled && !partnerIsNear;
}

// Bosses take damage from every player in both games. Dividing each hit by
// the number of players in the stage keeps a two-player fight at double the
// boss's health without losing chip damage to rounding.
class DamageScaler {
public:
    int scale(int damage, int players) {
        if (damage <= 0) return 0;
        players = std::clamp(players, 1, kMaxPlayersForScaling);
        if (players == 1) return damage;
        const int total = damage + carry_;
        carry_ = total % players;
        return total / players;
    }
    [[nodiscard]] int carry() const { return carry_; }
    void reset() { carry_ = 0; }

private:
    int carry_ = 0;
};

// Applying a partner's hit never kills locally: the death animation, drops and
// room-clear switches must run through the game's own hit handling. The
// enemy is left on 1 HP so the next local hit finishes it.
inline int apply_remote_damage(int health, int damage) {
    if (health <= 0 || damage <= 0) return health;
    const int next = health - std::min(damage, kMaxDamagePerMessage);
    return std::max(1, next);
}

// How far a hit should actually move a boss's health after scaling, given the
// health before and after the game's own hit code. Returns the health value
// to restore, or nullopt when nothing should be changed.
//
// The killing blow is never undone: once the game has moved a boss into its
// death state, reviving it would leave it stuck.
inline std::optional<int> scaled_boss_health(int before, int after, int scaledDamage) {
    if (after >= before || after <= 0) return std::nullopt;
    const int target = before - std::max(0, scaledDamage);
    if (target <= after) return std::nullopt;
    return std::max(1, target);
}

struct EnemyKey {
    int actorName = -1;
    uint32_t params = 0;
    int room = -1;
    Vec3 home;
};

// The same placed enemy shares its profile, parameters, room and home
// position in both games. Pick the closest home among identical candidates.
inline std::optional<size_t> best_enemy_match(const EnemyKey& wanted,
                                              const std::vector<EnemyKey>& candidates,
                                              float tolerance = kEnemyMatchTolerance) {
    std::optional<size_t> best;
    float bestDistance = tolerance;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const EnemyKey& c = candidates[i];
        if (c.actorName != wanted.actorName || c.params != wanted.params ||
            c.room != wanted.room || !finite(c.home)) continue;
        const float d = distance(c.home, wanted.home);
        if (d <= bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

struct DamageEvent {
    std::string stage;
    EnemyKey enemy;
    int damage = 0;
    bool killed = false;
    uint32_t sequence = 0;
};

inline nlohmann::json make_damage_message(const DamageEvent& e) {
    return {
        {"type", kEnemyDamageType},
        {"stage", e.stage},
        {"room", e.enemy.room},
        {"actor", e.enemy.actorName},
        {"params", e.enemy.params},
        {"hx", e.enemy.home.x},
        {"hy", e.enemy.home.y},
        {"hz", e.enemy.home.z},
        {"damage", e.damage},
        {"killed", e.killed},
        {"seq", e.sequence},
    };
}

inline std::optional<DamageEvent> parse_damage_message(const nlohmann::json& m) {
    if (!m.is_object() || m.value("type", std::string()) != kEnemyDamageType) return std::nullopt;
    try {
        DamageEvent e;
        e.stage = m.at("stage").get<std::string>();
        e.enemy.room = m.at("room").get<int>();
        e.enemy.actorName = m.at("actor").get<int>();
        e.enemy.params = m.at("params").get<uint32_t>();
        e.enemy.home = {m.at("hx").get<float>(), m.at("hy").get<float>(),
                        m.at("hz").get<float>()};
        e.damage = m.at("damage").get<int>();
        e.killed = m.value("killed", false);
        e.sequence = m.value("seq", 0U);
        if (e.stage.empty() || e.stage.size() > 16) return std::nullopt;
        if (e.enemy.room < 0 || e.enemy.room >= 64) return std::nullopt;
        if (e.enemy.actorName < 0 || e.enemy.actorName > 0xFFFF) return std::nullopt;
        if (e.damage <= 0 || e.damage > kMaxDamagePerMessage) return std::nullopt;
        if (!finite(e.enemy.home)) return std::nullopt;
        return e;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

inline nlohmann::json make_presence_message(const std::string& stage, int room,
                                            const Vec3& pos, uint32_t sequence) {
    return {{"type", kPresenceType}, {"stage", stage}, {"room", room},
            {"x", pos.x}, {"y", pos.y}, {"z", pos.z}, {"seq", sequence}};
}

inline std::optional<PartnerPresence> parse_presence_message(const nlohmann::json& m,
                                                             uint64_t nowTick) {
    if (!m.is_object() || m.value("type", std::string()) != kPresenceType) return std::nullopt;
    try {
        PartnerPresence p;
        p.stage = m.at("stage").get<std::string>();
        p.room = m.value("room", -1);
        p.pos = {m.at("x").get<float>(), m.at("y").get<float>(), m.at("z").get<float>()};
        p.sequence = m.value("seq", 0U);
        p.seenTick = nowTick;
        if (p.stage.size() > 16 || !finite(p.pos)) return std::nullopt;
        return p;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

inline nlohmann::json make_settings_message(const Options& o) {
    return {{"type", kSettingsType}, {"shared_hp", o.sharedHp},
            {"boss_door_together", o.bossDoorTogether}};
}

inline std::optional<Options> parse_settings_message(const nlohmann::json& m) {
    if (!m.is_object() || m.value("type", std::string()) != kSettingsType) return std::nullopt;
    const auto shared = m.find("shared_hp");
    const auto door = m.find("boss_door_together");
    if (shared == m.end() || door == m.end() || !shared->is_boolean() || !door->is_boolean())
        return std::nullopt;
    return Options{shared->get<bool>(), door->get<bool>()};
}

// Drops replays and reordered duplicates per sender. Sequence 0 means the
// sender did not number the message, which is always accepted.
class SequenceFilter {
public:
    bool accept(const std::string& peer, uint32_t sequence) {
        if (sequence == 0) return true;
        auto [it, inserted] = last_.try_emplace(peer, sequence);
        if (inserted) return true;
        // Allow wrap-around: anything "ahead" within half the range is new.
        const uint32_t delta = sequence - it->second;
        if (delta == 0 || delta > 0x7FFFFFFFU) return false;
        it->second = sequence;
        return true;
    }
    void forget(const std::string& peer) { last_.erase(peer); }
    void clear() { last_.clear(); }

private:
    std::map<std::string, uint32_t> last_;
};

class Cooldown {
public:
    bool ready(uint64_t nowTick, uint64_t interval) {
        if (fired_ && nowTick >= last_ && nowTick - last_ < interval) return false;
        fired_ = true;
        last_ = nowTick;
        return true;
    }

private:
    bool fired_ = false;
    uint64_t last_ = 0;
};

inline constexpr const char* kBossDoorBlockedMessage =
    "Bosses are fought together! Wait for your partner at the boss door.";
inline constexpr const char* kBossDoorNobodyMessage =
    "Bosses are fought together! Invite a partner to your room to open this door.";

}  // namespace dusklight_online::game::coop
