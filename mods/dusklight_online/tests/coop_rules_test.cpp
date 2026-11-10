#undef NDEBUG
#include "dusklight_online/game/coop_rules.hpp"

#include <cassert>
#include <cstdio>
#include <limits>

using namespace dusklight_online::game::coop;

namespace {

void test_effective_options() {
    const Options local{true, false};
    const Options host{false, true};
    assert(effective_options(false, false, local, host) == local);   // offline
    assert(effective_options(true, true, local, host) == local);     // owner decides
    assert(effective_options(true, false, local, host) == host);     // guest follows host
    assert(effective_options(true, false, local, std::nullopt) == local);
}

void test_presence_and_door() {
    std::map<std::string, PartnerPresence> partners;
    const Vec3 door{100.0f, 0.0f, 100.0f};
    assert(!partner_near(partners, "D_MN05A", door, 10));
    assert(partners_in_stage(partners, "D_MN05A", 10) == 0);

    partners["b"] = PartnerPresence{"D_MN05A", 3, {150.0f, 0.0f, 120.0f}, 10, 1};
    assert(partner_near(partners, "D_MN05A", door, 10));
    assert(partners_in_stage(partners, "D_MN05A", 10) == 1);
    // Different stage never counts.
    assert(!partner_near(partners, "D_MN04", door, 10));
    // Too far away.
    assert(!partner_near(partners, "D_MN05A", {5000.0f, 0.0f, 5000.0f}, 10));
    // Stale presence is ignored.
    assert(!partner_near(partners, "D_MN05A", door, 10 + kPresenceStaleTicks + 1));
    assert(partners_in_stage(partners, "D_MN05A", 10 + kPresenceStaleTicks + 1) == 0);
    // Exactly at the stale limit still counts.
    assert(partner_near(partners, "D_MN05A", door, 10 + kPresenceStaleTicks));
    // Non-finite positions are rejected.
    partners["c"] = PartnerPresence{"D_MN05A", 3,
        {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}, 10, 1};
    assert(partners_in_stage(partners, "D_MN05A", 10) == 2);
    partners.erase("b");
    assert(!partner_near(partners, "D_MN05A", door, 10));
    assert(!partner_near(partners, "", door, 10));

    assert(!boss_door_should_block(false, true, false));  // offline: vanilla
    assert(!boss_door_should_block(true, false, false));  // gate turned off
    assert(!boss_door_should_block(true, true, true));    // partner is here
    assert(boss_door_should_block(true, true, false));    // alone in a room
}

void test_damage_scaler() {
    DamageScaler s;
    assert(s.scale(0, 2) == 0);
    assert(s.scale(-4, 2) == 0);
    assert(s.scale(5, 1) == 5);
    // Two players: 1-damage chip hits land every second hit.
    assert(s.scale(1, 2) == 0);
    assert(s.carry() == 1);
    assert(s.scale(1, 2) == 1);
    assert(s.carry() == 0);
    // Total damage is preserved exactly over many hits.
    DamageScaler t;
    int sum = 0;
    for (int i = 0; i < 1000; ++i) sum += t.scale(3, 2);
    assert(sum + t.carry() / 2 == 1500 || sum == 1500);
    assert(sum == 1500);
    DamageScaler u;
    int total = 0;
    for (int i = 0; i < 99; ++i) total += u.scale(1, 3);
    assert(total == 33);
    // Player count is clamped.
    DamageScaler v;
    assert(v.scale(80, 1000) == 10);
    DamageScaler w;
    assert(w.scale(7, 0) == 7);
    w.reset();
    assert(w.carry() == 0);
}

void test_scaled_boss_health() {
    // Hit for 10 with two players: only 5 should stick.
    assert(scaled_boss_health(100, 90, 5).value() == 95);
    // No change when the hit did nothing or healed.
    assert(!scaled_boss_health(100, 100, 5));
    assert(!scaled_boss_health(100, 120, 5));
    // A killing blow is never reversed.
    assert(!scaled_boss_health(10, 0, 5));
    assert(!scaled_boss_health(10, -3, 5));
    // Scaled damage larger than the real hit never adds damage.
    assert(!scaled_boss_health(100, 95, 10));
    // Zero scaled damage restores the full health.
    assert(scaled_boss_health(20, 19, 0).value() == 20);
}

void test_remote_damage() {
    assert(apply_remote_damage(10, 3) == 7);
    assert(apply_remote_damage(10, 10) == 1);
    assert(apply_remote_damage(10, 500) == 1);
    assert(apply_remote_damage(0, 5) == 0);    // already dead stays dead
    assert(apply_remote_damage(-2, 5) == -2);
    assert(apply_remote_damage(5, 0) == 5);
    assert(apply_remote_damage(5, -9) == 5);
    assert(apply_remote_damage(30000, 1000000) == 30000 - kMaxDamagePerMessage);
}

void test_enemy_matching() {
    const EnemyKey wanted{0x1A0, 0xFF01, 2, {100.0f, 0.0f, 100.0f}};
    std::vector<EnemyKey> candidates = {
        {0x1A0, 0xFF01, 2, {900.0f, 0.0f, 900.0f}},  // too far
        {0x1A1, 0xFF01, 2, {100.0f, 0.0f, 100.0f}},  // different profile
        {0x1A0, 0xFF02, 2, {100.0f, 0.0f, 100.0f}},  // different params
        {0x1A0, 0xFF01, 3, {100.0f, 0.0f, 100.0f}},  // different room
        {0x1A0, 0xFF01, 2, {160.0f, 0.0f, 100.0f}},  // close
        {0x1A0, 0xFF01, 2, {110.0f, 0.0f, 100.0f}},  // closest
    };
    assert(best_enemy_match(wanted, candidates).value() == 5);
    candidates.pop_back();
    assert(best_enemy_match(wanted, candidates).value() == 4);
    candidates.pop_back();
    assert(!best_enemy_match(wanted, candidates));
    assert(!best_enemy_match(wanted, {}));
}

void test_damage_wire() {
    DamageEvent e;
    e.stage = "D_MN05A";
    e.enemy = {0x1A0, 0xDEADBEEF, 4, {1.5f, -2.0f, 3.25f}};
    e.damage = 12;
    e.killed = true;
    e.sequence = 77;
    const auto parsed = parse_damage_message(make_damage_message(e));
    assert(parsed);
    assert(parsed->stage == e.stage);
    assert(parsed->enemy.actorName == e.enemy.actorName);
    assert(parsed->enemy.params == e.enemy.params);
    assert(parsed->enemy.room == e.enemy.room);
    assert(parsed->enemy.home.z == 3.25f);
    assert(parsed->damage == 12 && parsed->killed && parsed->sequence == 77);

    // Round trip through text, the way it crosses the network.
    const auto reparsed = parse_damage_message(
        nlohmann::json::parse(make_damage_message(e).dump()));
    assert(reparsed && reparsed->enemy.params == 0xDEADBEEF);

    auto bad = make_damage_message(e);
    bad["damage"] = 0;
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["damage"] = kMaxDamagePerMessage + 1;
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["room"] = 64;
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["stage"] = "";
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["stage"] = std::string(40, 'x');
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad.erase("hx");
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["actor"] = "zelda";
    assert(!parse_damage_message(bad));
    bad = make_damage_message(e);
    bad["type"] = "pvp_hit";
    assert(!parse_damage_message(bad));
    assert(!parse_damage_message(nlohmann::json::array()));
    assert(!parse_damage_message(nlohmann::json("text")));
}

void test_presence_wire() {
    const auto m = make_presence_message("F_SP108", 1, {10.0f, 20.0f, 30.0f}, 5);
    const auto p = parse_presence_message(m, 42);
    assert(p && p->stage == "F_SP108" && p->room == 1 && p->seenTick == 42);
    assert(p->pos.y == 20.0f && p->sequence == 5);
    auto bad = m;
    bad.erase("x");
    assert(!parse_presence_message(bad, 1));
    bad = m;
    bad["stage"] = std::string(30, 'y');
    assert(!parse_presence_message(bad, 1));
    bad = m;
    bad["type"] = kSettingsType;
    assert(!parse_presence_message(bad, 1));
}

void test_settings_wire() {
    for (bool a : {false, true})
        for (bool b : {false, true}) {
            const Options o{a, b};
            assert(parse_settings_message(make_settings_message(o)).value() == o);
        }
    auto bad = make_settings_message({true, true});
    bad["shared_hp"] = 1;
    assert(!parse_settings_message(bad));
    bad = make_settings_message({true, true});
    bad.erase("boss_door_together");
    assert(!parse_settings_message(bad));
    assert(!parse_settings_message(nlohmann::json::object()));
}

void test_sequence_filter() {
    SequenceFilter f;
    assert(f.accept("a", 1));
    assert(!f.accept("a", 1));   // replay
    assert(f.accept("a", 2));
    assert(!f.accept("a", 1));   // stale
    assert(f.accept("b", 1));    // other sender is independent
    assert(f.accept("a", 0));    // unnumbered always passes
    assert(f.accept("a", 0));
    // Wrap-around.
    assert(f.accept("w", 0xFFFFFFFEU));
    assert(f.accept("w", 0xFFFFFFFFU));
    assert(f.accept("w", 1));
    f.forget("a");
    assert(f.accept("a", 1));
    f.clear();
    assert(f.accept("b", 1));
}

void test_cooldown() {
    Cooldown c;
    assert(c.ready(100, 150));
    assert(!c.ready(101, 150));
    assert(!c.ready(249, 150));
    assert(c.ready(250, 150));
    // Tick counter reset (new session) never locks the toast forever.
    assert(c.ready(3, 150));
}

}  // namespace

int main() {
    test_effective_options();
    test_presence_and_door();
    test_damage_scaler();
    test_scaled_boss_health();
    test_remote_damage();
    test_enemy_matching();
    test_damage_wire();
    test_presence_wire();
    test_settings_wire();
    test_sequence_filter();
    test_cooldown();
    std::puts("coop_rules_test: all tests passed");
    return 0;
}
