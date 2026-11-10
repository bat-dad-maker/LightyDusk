#include "dusklight_online/game/coop_features.hpp"

#include "dusklight_online/game/visual_bridge.hpp"
#include "dusklight_online/logging.hpp"
#include "dusklight_online/net/transport.hpp"

#include "d/dolzel.h" // Public game umbrella must precede individual game headers.

#include "d/d_com_inf_game.h"
#include "d/actor/d_a_door_boss.h"
#include "d/actor/d_a_door_bossL1.h"
#include "d/actor/d_a_door_bossL5.h"
#include "d/actor/d_a_player.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace dusklight_online::game::coop {

DEFINE_HOOK(&daBdoor_c::checkOpen, CoopBossDoorCheckOpenHook);
DEFINE_HOOK(&daBdoorL1_c::checkOpen, CoopBossDoorL1CheckOpenHook);
DEFINE_HOOK(&daBdoorL5_c::checkOpen, CoopBossDoorL5CheckOpenHook);

namespace {

struct State {
    net::Transport* transport = nullptr;
    Options local;
    std::optional<Options> fromHost;
    Options lastAnnounced;
    bool announcedOnce = false;

    // Cached once per tick from the transport.
    bool lobbyActive = false;
    bool isOwner = false;
    std::string localPeerId;

    uint64_t tick = 0;
    uint32_t presenceSequence = 0;
    uint32_t damageSequence = 0;
    std::string lastStage;

    std::map<std::string, PartnerPresence> partners;
    SequenceFilter presenceFilter;
    SequenceFilter damageFilter;
    std::unordered_map<void*, DamageScaler> bossScalers;
    Cooldown doorToast;
    bool applyingRemote = false;

    struct Pending {
        void* process = nullptr;
        int health = 0;
        bool tracked = false;
    };
    std::vector<Pending> executeStack;
    bool hooksInstalled = false;
};

State& state() {
    static State s;
    return s;
}

bool is_boss_actor(int name) {
    switch (name) {
    case fpcNm_B_BH_e:     // Diababa's Baba Serpent heads
    case fpcNm_B_BQ_e:     // Diababa
    case fpcNm_B_DR_e:     // Argorok
    case fpcNm_B_DRE_e:
    case fpcNm_B_DS_e:     // Stallord
    case fpcNm_B_GG_e:     // Darkhammer (Snowpeak / Sky mini-boss)
    case fpcNm_B_GM_e:     // Armogohma
    case fpcNm_B_GND_e:    // Ganondorf (horseback and sword)
    case fpcNm_B_GO_e:     // Dangoro
    case fpcNm_B_GOS_e:
    case fpcNm_B_MGN_e:    // Beast Ganon
    case fpcNm_B_OB_e:     // Morpheel
    case fpcNm_B_OH_e:
    case fpcNm_B_OH2_e:
    case fpcNm_B_TN_e:     // Darknut (Temple of Time mini-boss)
    case fpcNm_B_YO_e:     // Blizzeta
    case fpcNm_B_YOI_e:
    case fpcNm_B_ZANT_e:   // Zant and his forms
    case fpcNm_B_ZANTM_e:
    case fpcNm_B_ZANTS_e:
    case fpcNm_B_ZANTZ_e:
        return true;
    default:
        return false;
    }
}

std::string current_stage_name() {
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr) return {};
    return std::string(stage);
}

Vec3 to_vec(const cXyz& v) { return Vec3{v.x, v.y, v.z}; }

bool tracked_enemy(void* process) {
    if (process == nullptr || !fopAcM_IsActor(process)) return false;
    auto* actor = static_cast<fopAc_ac_c*>(process);
    return fopAcM_GetGroup(actor) == fopAc_ENEMY_e;
}

bool send(const nlohmann::json& message) {
    State& s = state();
    if (s.transport == nullptr || !s.lobbyActive) return false;
    return s.transport->send(message);
}

// ------------------------------------------------------------------ doors --

void boss_door_check_post(fopAc_ac_c* door, void* retval) {
    if (door == nullptr || retval == nullptr) return;
    auto* result = static_cast<int*>(retval);
    if (*result == 0) return;  // the game already said no (no key, wrong side...)

    State& s = state();
    const Options options = current_options();
    const std::string stage = current_stage_name();
    const bool near = partner_near(s.partners, stage, to_vec(door->current.pos), s.tick);
    if (!boss_door_should_block(s.lobbyActive, options.bossDoorTogether, near)) return;

    *result = 0;
    if (s.doorToast.ready(s.tick, kDoorToastCooldownTicks)) {
        const bool anyoneConnected = partners_in_stage(s.partners, stage, s.tick) > 0 ||
                                     !s.partners.empty();
        push_online_notification(anyoneConnected ? kBossDoorBlockedMessage
                                                 : kBossDoorNobodyMessage,
                                 5.0f, true);
    }
}

void bdoor_check_open_post(ModContext*, void* args, void* retval, void*) {
    boss_door_check_post(mods::arg<fopAc_ac_c*>(args, 0), retval);
}
void bdoor_l1_check_open_post(ModContext*, void* args, void* retval, void*) {
    boss_door_check_post(mods::arg<fopAc_ac_c*>(args, 0), retval);
}
void bdoor_l5_check_open_post(ModContext*, void* args, void* retval, void*) {
    boss_door_check_post(mods::arg<fopAc_ac_c*>(args, 0), retval);
}

// ----------------------------------------------------------- remote damage --

struct SearchContext {
    EnemyKey wanted;
    std::vector<EnemyKey> keys;
    std::vector<fopAc_ac_c*> actors;
};

void* collect_enemy_candidates(void* actor, void* data) {
    auto* ctx = static_cast<SearchContext*>(data);
    if (!tracked_enemy(actor)) return nullptr;
    auto* enemy = static_cast<fopAc_ac_c*>(actor);
    if (fpcM_GetName(enemy) != ctx->wanted.actorName) return nullptr;
    ctx->keys.push_back(EnemyKey{fpcM_GetName(enemy), fopAcM_GetParam(enemy),
                                 fopAcM_GetRoomNo(enemy), to_vec(enemy->home.pos)});
    ctx->actors.push_back(enemy);
    return nullptr;  // keep iterating
}

ApplyResult apply_remote_damage_event(const DamageEvent& event) {
    if (event.stage != current_stage_name()) return ApplyResult::IgnoredByPolicy;
    SearchContext ctx;
    ctx.wanted = event.enemy;
    fopAcIt_Judge(&collect_enemy_candidates, &ctx);
    const auto match = best_enemy_match(event.enemy, ctx.keys);
    if (!match) return ApplyResult::IgnoredByPolicy;
    fopAc_ac_c* enemy = ctx.actors[*match];
    const int before = enemy->health;
    const int after = apply_remote_damage(before, event.damage);
    if (after == before) return ApplyResult::IgnoredByPolicy;
    State& s = state();
    s.applyingRemote = true;
    enemy->health = static_cast<s16>(after);
    s.applyingRemote = false;
    return ApplyResult::Applied;
}

}  // namespace

// ------------------------------------------------------------------ public --

ModResult install_hooks() {
    State& s = state();
    if (s.hooksInstalled) return MOD_OK;
    // Each boss door family is optional: a missing symbol on one build must
    // not disable the rest of the online mod.
    bool any = false;
    if (mods::hook::add_post<CoopBossDoorCheckOpenHook>(&bdoor_check_open_post) == MOD_OK)
        any = true;
    else
        log_info("Co-op: boss door hook unavailable (daBdoor_c::checkOpen)");
    if (mods::hook::add_post<CoopBossDoorL1CheckOpenHook>(&bdoor_l1_check_open_post) == MOD_OK)
        any = true;
    else
        log_info("Co-op: boss door hook unavailable (daBdoorL1_c::checkOpen)");
    if (mods::hook::add_post<CoopBossDoorL5CheckOpenHook>(&bdoor_l5_check_open_post) == MOD_OK)
        any = true;
    else
        log_info("Co-op: boss door hook unavailable (daBdoorL5_c::checkOpen)");
    s.hooksInstalled = true;
    log_info(any ? "Co-op: boss door gate ready" : "Co-op: boss door gate disabled");
    return MOD_OK;
}

void uninstall_hooks() {
    State& s = state();
    if (!s.hooksInstalled) return;
    mods::hook::uninstall<CoopBossDoorCheckOpenHook>();
    mods::hook::uninstall<CoopBossDoorL1CheckOpenHook>();
    mods::hook::uninstall<CoopBossDoorL5CheckOpenHook>();
    s.hooksInstalled = false;
}

void attach(net::Transport* transport) { state().transport = transport; }

void set_local_options(const Options& options) { state().local = options; }

Options current_options() {
    const State& s = state();
    return effective_options(s.lobbyActive, s.isOwner, s.local, s.fromHost);
}

bool host_controls_options() {
    const State& s = state();
    return s.lobbyActive && !s.isOwner;
}

void process_execute_pre(void* process) {
    State& s = state();
    State::Pending pending;
    pending.process = process;
    if (s.lobbyActive && tracked_enemy(process)) {
        pending.health = static_cast<fopAc_ac_c*>(process)->health;
        pending.tracked = true;
    }
    s.executeStack.push_back(pending);
}

void process_execute_post(void* process) {
    State& s = state();
    if (s.executeStack.empty()) return;
    const State::Pending pending = s.executeStack.back();
    s.executeStack.pop_back();
    if (!pending.tracked || pending.process != process || s.applyingRemote) return;
    if (!tracked_enemy(process)) return;

    auto* enemy = static_cast<fopAc_ac_c*>(process);
    const int before = pending.health;
    int after = enemy->health;
    if (after >= before) return;

    const Options options = current_options();
    if (!options.sharedHp) return;

    const std::string stage = current_stage_name();
    if (stage.empty()) return;
    const int name = fpcM_GetName(enemy);
    int damage = before - after;

    if (is_boss_actor(name)) {
        const int players = 1 + partners_in_stage(s.partners, stage, s.tick);
        if (players > 1) {
            const int scaled = s.bossScalers[process].scale(damage, players);
            if (const auto restored = scaled_boss_health(before, after, scaled)) {
                enemy->health = static_cast<s16>(*restored);
                after = *restored;
            }
            damage = before - after;
        }
    }
    if (damage <= 0) return;

    DamageEvent event;
    event.stage = stage;
    event.enemy = EnemyKey{name, fopAcM_GetParam(enemy), fopAcM_GetRoomNo(enemy),
                           to_vec(enemy->home.pos)};
    event.damage = std::min(damage, kMaxDamagePerMessage);
    event.killed = after <= 0;
    event.sequence = ++s.damageSequence;
    if (event.enemy.room < 0 || event.enemy.room >= 64) return;
    (void)send(make_damage_message(event));
}

void tick() {
    State& s = state();
    ++s.tick;
    if (s.transport == nullptr) return;
    const net::Status status = s.transport->status();
    const bool wasActive = s.lobbyActive;
    s.lobbyActive = status.enabled && status.welcomed;
    s.isOwner = status.mode == net::Mode::DirectHost ||
                (net::is_room_mode(status.mode) && status.isOwner);
    s.localPeerId = status.mode == net::Mode::DirectHost ? "direct" : status.clientId;
    if (!s.lobbyActive) {
        if (wasActive) reset();
        return;
    }

    const std::string stage = current_stage_name();
    if (stage != s.lastStage) {
        // Actor pointers from the previous stage are gone.
        s.bossScalers.clear();
        s.executeStack.clear();
        s.lastStage = stage;
    }

    // Forget partners we stopped hearing from.
    for (auto it = s.partners.begin(); it != s.partners.end();) {
        if (s.tick >= it->second.seenTick &&
            s.tick - it->second.seenTick > kPresenceStaleTicks * 20) {
            it = s.partners.erase(it);
        } else {
            ++it;
        }
    }

    if (s.tick % kPresenceIntervalTicks == 0 && !stage.empty()) {
        if (const auto* player = daPy_getPlayerActorClass(); player != nullptr) {
            (void)send(make_presence_message(
                stage, dComIfGp_roomControl_getStayNo(), to_vec(player->current.pos),
                ++s.presenceSequence));
        }
    }

    if (s.isOwner) {
        const bool changed = !s.announcedOnce || s.lastAnnounced != s.local;
        if (changed || s.tick % kSettingsIntervalTicks == 0) {
            if (send(make_settings_message(s.local))) {
                s.lastAnnounced = s.local;
                s.announcedOnce = true;
            }
        }
    }
}

bool handles(const std::string& type) {
    return type == kPresenceType || type == kSettingsType || type == kEnemyDamageType;
}

ApplyResult consume(const RoutedMessage& message) {
    State& s = state();
    const std::string type = message.payload.value("type", std::string());
    if (message.peerId.empty() || message.peerId == s.localPeerId)
        return ApplyResult::IgnoredByPolicy;

    if (type == kPresenceType) {
        auto presence = parse_presence_message(message.payload, s.tick);
        if (!presence) return ApplyResult::Rejected;
        if (!s.presenceFilter.accept(message.peerId, presence->sequence))
            return ApplyResult::IgnoredByPolicy;
        s.partners[message.peerId] = std::move(*presence);
        return ApplyResult::Applied;
    }
    if (type == kSettingsType) {
        const auto options = parse_settings_message(message.payload);
        if (!options) return ApplyResult::Rejected;
        if (s.isOwner) return ApplyResult::IgnoredByPolicy;
        if (!s.fromHost || *s.fromHost != *options) {
            push_online_notification(std::string("Host co-op settings: Shared HP ") +
                                         (options->sharedHp ? "on" : "off") +
                                         ", boss doors together " +
                                         (options->bossDoorTogether ? "on" : "off") + ".",
                                     4.0f, false);
        }
        s.fromHost = *options;
        return ApplyResult::Applied;
    }
    if (type == kEnemyDamageType) {
        if (!current_options().sharedHp) return ApplyResult::IgnoredByPolicy;
        const auto event = parse_damage_message(message.payload);
        if (!event) return ApplyResult::Rejected;
        if (!s.damageFilter.accept(message.peerId, event->sequence))
            return ApplyResult::IgnoredByPolicy;
        return apply_remote_damage_event(*event);
    }
    return ApplyResult::Unsupported;
}

void peer_left(const std::string& peerId) {
    State& s = state();
    s.partners.erase(peerId);
    s.presenceFilter.forget(peerId);
    s.damageFilter.forget(peerId);
}

void reset() {
    State& s = state();
    s.partners.clear();
    s.presenceFilter.clear();
    s.damageFilter.clear();
    s.bossScalers.clear();
    s.executeStack.clear();
    s.fromHost.reset();
    s.announcedOnce = false;
    s.lastStage.clear();
}

}  // namespace dusklight_online::game::coop
