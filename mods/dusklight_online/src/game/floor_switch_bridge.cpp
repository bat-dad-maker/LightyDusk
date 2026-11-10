#include "dusklight_online/game/floor_switch_bridge.hpp"
#include "dusklight_online/game/floor_switch_wire.hpp"
#include "dusklight_online/game/game_adapter.hpp"

#include "d/dolzel.h"
#include "d/actor/d_a_obj_swpush.h"
#include "d/actor/d_a_obj_swpush5.h"
#include "d/actor/d_a_obj_heavySw.h"
#include "d/d_com_inf_game.h"
#include "d/d_stage.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"
#include "mods/svc/hook.hpp"

#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <vector>

namespace dusklight_online::game {
// Floor switches are observed from the existing fpcM_Execute hook in
// game_adapter.cpp. These actor implementations live behind game actor/REL
// boundaries, so installing a second hook on their member Execute routines is
// both unnecessary and less reliable than the process boundary already used
// by the working room-actor synchronization.
namespace {
namespace fs = floor_switch;
static_assert(fs::Push == fpcNm_Obj_Swpush_e);
static_assert(fs::Iron == fpcNm_Obj_Swpush5_e);
static_assert(fs::Heavy == fpcNm_Obj_HeavySw_e);

GameAdapter* sAdapter = nullptr;
fs::Ledger sReceived;
std::optional<fs::Scene> sScene;
fs::Contacts sSampled, sLocal, sSent;
std::set<fs::Key> sRemoteOrigin, sCompleted;
// Retain only the camera guard while native release finishes after disabling
// sync or disconnecting. This stores no pressure and cannot hold a plate down.
std::optional<fs::Scene> sRetiringScene;
std::set<fs::Key> sRetiringOrigin;
uint64_t sSequence = 0, sLastSent = 0;
bool sNeedSend = true;
struct Frame {
    fopAc_ac_c* actor = nullptr;
    bool remoteOnly = false;
    // Remote pressure must never start a switch demo/pause which can touch the
    // receiver's Link/event state. Preserve only the actor-local gating fields
    // while its native physical state machine consumes the remote pressure.
    int pushEventId = -1;
    uint8_t pushRideSource = 0;
    uint8_t ironPrevRide = 0;
    bool guardedPush = false;
    bool guardedIron = false;
};
std::vector<Frame> sFrames;

uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::optional<fs::Scene> scene_identity() {
    if (sAdapter == nullptr || dComIfGp_getPlayer(0) == nullptr) return std::nullopt;
    auto* info = dComIfGp_getStageStagInfo();
    const char* stage = dComIfGp_getStartStageName();
    const int room = dComIfGp_roomControl_getStayNo();
    if (info == nullptr || stage == nullptr || room < 0) return std::nullopt;
    // Resolve the actual layer: a -1 automatic start layer and an explicit
    // warp layer can describe the same room on different clients.
    fs::Scene scene{dStage_stagInfo_GetSaveTbl(info), stage, room,
                    dComIfG_play_c::getLayerNo(room)};
    return fs::valid_scene(scene) ? std::optional<fs::Scene>(std::move(scene)) : std::nullopt;
}

std::optional<fs::Scene> current_scene() {
    if (sAdapter == nullptr || !sAdapter->floor_switch_sync_active()) return std::nullopt;
    return scene_identity();
}

void retire_remote_origin() {
    if (sScene && !sRemoteOrigin.empty()) {
        sRetiringScene = sScene;
        sRetiringOrigin = sRemoteOrigin;
    }
}

void send(const fs::Scene& scene, const fs::Contacts& contacts) {
    // Sequence never resets at a room/save/settings boundary.
    sAdapter->publish_local(fs::encode({++sSequence, scene, contacts}));
    sLastSent = now_ms();
    sSent = contacts;
    sNeedSend = false;
}

void update_scene() {
    auto scene = current_scene();
    if (scene == sScene) return;
    if (sScene && !sSent.empty()) send(*sScene, {});
    retire_remote_origin();
    sScene = std::move(scene);
    sSampled.clear();
    sLocal.clear();
    sSent.clear();
    sRemoteOrigin.clear();
    sCompleted.clear();
    sReceived.clear_pressure();
    sNeedSend = true;
}

std::optional<fs::Key> actor_key(fopAc_ac_c* actor) {
    if (actor == nullptr) return std::nullopt;
    fs::Key key{fopAcM_GetName(actor), fopAcM_GetParam(actor), {}, 0};
    if (!fs::supported(key.actor, key.params)) return std::nullopt;
    const float coords[] = {actor->home.pos.x, actor->home.pos.y, actor->home.pos.z};
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(coords[i]) || std::abs(coords[i]) > 1000000.0f) return std::nullopt;
        key.home[i] = static_cast<int32_t>(std::lround(coords[i] * 4.0f));
    }
    if (key.actor == fs::Push) {
        const auto* plate = static_cast<daObjSwpush::Act_c*>(actor);
        // prmZ_init() deliberately zeroes home.angle.z; use its cached value.
        key.placementZ = plate->mPrmZInit ? plate->mPrmZ : uint16_t(actor->home.angle.z);
    }
    return fs::valid_key(key) ? std::optional<fs::Key>(key) : std::nullopt;
}

fs::Pressure local_pressure(fopAc_ac_c* actor, int kind) {
    if (kind == fs::Push) {
        const auto* a = static_cast<daObjSwpush::Act_c*>(actor);
        return {uint8_t(std::min<unsigned>(a->mRidingMode, 2)),
                a->mRidingMode != 0 && a->mHeavyRiding};
    }
    if (kind == fs::Iron) {
        const auto* a = static_cast<daObjSw5_c*>(actor);
        return {uint8_t(std::min<unsigned>(a->field_0x5ae, 2)),
                a->field_0x5ae != 0 && a->mIsPlayerRideHvy != 0};
    }
    const auto* a = static_cast<daHeavySw_c*>(actor);
    // HeavySw clears riding, but not its cached boots bit, at end of Execute.
    return {uint8_t(a->field_0x5d8 != 0), a->field_0x5d8 != 0 && a->field_0x5dc != 0};
}

void inject(fopAc_ac_c* actor, int kind, fs::Pressure pressure) {
    if (kind == fs::Push) {
        auto* a = static_cast<daObjSwpush::Act_c*>(actor);
        a->mRidingMode = pressure.ride;
        a->mHeavyRiding = pressure.heavy;
    } else if (kind == fs::Iron) {
        auto* a = static_cast<daObjSw5_c*>(actor);
        a->field_0x5ae = pressure.ride;
        a->mIsPlayerRideHvy = pressure.heavy;
        if (pressure.active()) a->mUnkRideTimer = 6;
    } else {
        auto* a = static_cast<daHeavySw_c*>(actor);
        a->field_0x5d8 = pressure.active();
        a->field_0x5dc = pressure.heavy;
    }
}

bool idle(fopAc_ac_c* actor, int kind) {
    if (kind == fs::Push) {
        const auto* a = static_cast<daObjSwpush::Act_c*>(actor);
        return a->mMode == daObjSwpush::Act_c::MODE_UPPER && !a->mMiniPushFlg;
    }
    if (kind == fs::Iron) return static_cast<daObjSw5_c*>(actor)->mMode == 0;
    return static_cast<daHeavySw_c*>(actor)->mMode == daHeavySw_c::MODE_WAIT;
}

// This is a completion repair, NOT another source of pressure. It is only
// armed after receipt of the durable output bit, never by an initial press.
bool complete_native(fopAc_ac_c* actor, const fs::Key& key) {
    if (!sCompleted.contains(key)) return false;
    if (!fopAcM_isSwitch(actor, fs::output_flag(key.actor, key.params))) {
        sCompleted.erase(key); // An external timer/reset wins.
        return false;
    }
    if (key.actor == fs::Heavy) {
        auto* a = static_cast<daHeavySw_c*>(actor);
        if (a->mMode == daHeavySw_c::MODE_MOVE_END) {
            sCompleted.erase(key);
            return false;
        }
        if (a->mMode != daHeavySw_c::MODE_MOVE) a->init_modeMove();
        return true; // Maintain the proven latch through native downward motion.
    }
    if (key.actor == fs::Iron) {
        auto* a = static_cast<daObjSw5_c*>(actor);
        if (a->mMode == 0 || a->mMode == 3) a->modeLowerInit();
    }
    // Gold A already obeys its saved bit in mode_upper(). Its demo initializer
    // is suppressed by remote-origin tracking, not a global event reset.
    sCompleted.erase(key);
    return false;
}

void process_pre_impl(fopAc_ac_c* actor) {
    update_scene();
    if (actor == nullptr || !fopAcM_IsActor(actor)) return;
    const auto key = actor_key(actor);
    if (!key) return;

    Frame frame;
    frame.actor = actor;
    if (sRetiringScene && scene_identity() != sRetiringScene) {
        sRetiringScene.reset();
        sRetiringOrigin.clear();
    }
    if (sRetiringOrigin.contains(*key)) {
        if (local_pressure(actor, key->actor).active() || idle(actor, key->actor))
            sRetiringOrigin.erase(*key);
        else
            frame.remoteOnly = true;
    }

    if (sScene && fopAcM_GetHomeRoomNo(actor) == sScene->room) {
        // Sample BEFORE adding peer pressure. This is the exact separation
        // that prevents remote pressure from being echoed back as local input.
        const auto local = local_pressure(actor, key->actor);
        if (sSampled.contains(*key) || sSampled.size() < fs::MaxSwitches) sSampled[*key] = local;
        const auto remote = sReceived.pressure(*sScene, *key, now_ms());
        const bool completion = sCompleted.contains(*key);
        if (local.active()) sRemoteOrigin.erase(*key);
        else if (remote.active() || completion) sRemoteOrigin.insert(*key);
        else if (idle(actor, key->actor)) sRemoteOrigin.erase(*key);

        frame.remoteOnly = !local.active() &&
                           (frame.remoteOnly || sRemoteOrigin.contains(*key));

        const bool finishingHeavy = complete_native(actor, *key);
        auto pressure = fs::combine(local, remote);
        if (finishingHeavy) pressure = fs::combine(pressure, {1, true});
        inject(actor, key->actor, pressure);

        // Keep the switch actor's native physics/animation, but block the
        // actor-local paths which can order a pause/demo on the receiving
        // player's Link. No Link or remote-dummy state is touched.
        if (frame.remoteOnly && key->actor == fs::Push) {
            auto* push = static_cast<daObjSwpush::Act_c*>(actor);
            frame.pushEventId = push->mEventID;
            frame.pushRideSource = push->field_0x5c5;
            frame.guardedPush = true;
            push->mEventID = -1;   // demo_reqSw_init becomes a no-op
            push->field_0x5c5 = 1; // mode_upper skips demo_reqPause_init
        } else if (frame.remoteOnly && key->actor == fs::Iron) {
            auto* iron = static_cast<daObjSw5_c*>(actor);
            frame.ironPrevRide = iron->field_0x5af;
            frame.guardedIron = true;
            // modeWaitLower only orders its Link pause on a new ride edge.
            iron->field_0x5af = iron->field_0x5ae;
        }
    }
    sFrames.push_back(frame);
}

void process_post_impl(fopAc_ac_c* actor) {
    if (sFrames.empty()) return;
    Frame frame = sFrames.back();
    if (frame.actor != actor) return;
    sFrames.pop_back();

    if (frame.guardedPush) {
        auto* push = static_cast<daObjSwpush::Act_c*>(actor);
        push->mEventID = static_cast<s16>(frame.pushEventId);
        push->field_0x5c5 = frame.pushRideSource;
    }
    if (frame.guardedIron) {
        static_cast<daObjSw5_c*>(actor)->field_0x5af = frame.ironPrevRide;
    }
}

struct FindFlag { int room; int flag; bool completion; bool found = false; };
void* visit_flag(void* ptr, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(ptr);
    auto& search = *static_cast<FindFlag*>(data);
    const auto key = actor_key(actor);
    if (!key || fopAcM_GetHomeRoomNo(actor) != search.room ||
        fs::output_flag(key->actor, key->params) != search.flag) return nullptr;
    if (fs::momentary(key->actor, key->params)) {
        search.found = true;
    } else if (search.completion && sCompleted.size() < fs::MaxSwitches) {
        sCompleted.insert(*key);
        sRemoteOrigin.insert(*key);
    }
    return nullptr; // Visit ALL placements, including those sharing a flag.
}
struct SnapshotCopy { int room; dSv_info_c* info; };
void* sanitize_snapshot_actor(void* ptr, void* data) {
    auto* actor = static_cast<fopAc_ac_c*>(ptr);
    auto& copy = *static_cast<SnapshotCopy*>(data);
    const auto key = actor_key(actor);
    if (!key || fopAcM_GetHomeRoomNo(actor) != copy.room ||
        !fs::momentary(key->actor, key->params)) return nullptr;
    const int flag = fs::output_flag(key->actor, key->params);
    if (flag >= 0 && flag < 0xFF) {
        // Zone flags require a real loaded zone even in a copied dSv_info_c.
        if (flag < dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH ||
            (dComIfGp_roomControl_getZoneNo(copy.room) >= 0 &&
             dComIfGp_roomControl_getZoneNo(copy.room) < dSv_info_c::ZONE_MAX))
            copy.info->offSwitch(flag, copy.room);
    }
    return nullptr;
}
} // namespace

ModResult install_floor_switch_hooks(GameAdapter& adapter, ModError*) {
    // Lifecycle registration only. Runtime observation/injection is driven by
    // GameAdapter's existing fpcM_Execute hook so it follows the same path as
    // other synchronized room actors.
    sAdapter = &adapter;
    return MOD_OK;
}

void uninstall_floor_switch_hooks() {
    reset_floor_switch_state(true);
    sFrames.clear();
    sRetiringScene.reset();
    sRetiringOrigin.clear();
    sAdapter = nullptr;
}

void floor_switch_process_pre(void* process) {
    if (process == nullptr || !fopAcM_IsActor(process)) return;
    if (!is_floor_switch_actor(fopAcM_GetName(static_cast<fopAc_ac_c*>(process)))) return;
    process_pre_impl(static_cast<fopAc_ac_c*>(process));
}

void floor_switch_process_post(void* process) {
    if (process == nullptr || !fopAcM_IsActor(process)) return;
    if (!is_floor_switch_actor(fopAcM_GetName(static_cast<fopAc_ac_c*>(process)))) return;
    process_post_impl(static_cast<fopAc_ac_c*>(process));
}

void flush_floor_switch_state() {
    update_scene();
    if (!sScene) return;
    sLocal = fs::finish_frame(std::move(sLocal), sSampled,
                              dComIfGp_isPauseFlag() || dComIfGp_event_runCheck());
    sSampled.clear();
    if (sNeedSend || sLocal != sSent ||
        (!sLocal.empty() && now_ms() - sLastSent >= fs::HeartbeatMs))
        send(*sScene, sLocal);
}

ApplyResult receive_floor_switch_state(std::string_view peer, const nlohmann::json& message) {
    const auto packet = fs::decode(message);
    if (!packet) return ApplyResult::Rejected;
    update_scene();
    switch (sReceived.receive(peer, *packet, now_ms())) {
    case fs::Ledger::Result::Accepted: return ApplyResult::Applied;
    case fs::Ledger::Result::Stale: return ApplyResult::IgnoredByPolicy;
    default: return ApplyResult::Rejected;
    }
}

void forget_floor_switch_peer(std::string_view peer) { sReceived.forget(peer); }
void reset_floor_switch_state(bool newSession) {
    if (!newSession && sAdapter && sScene && !sSent.empty()) send(*sScene, {});
    retire_remote_origin();
    sScene.reset();
    sSampled.clear(); sLocal.clear(); sSent.clear();
    sRemoteOrigin.clear(); sCompleted.clear();
    sNeedSend = true;
    if (newSession) { sReceived.reset_session(); sSequence = 0; }
    else sReceived.clear_pressure();
}

bool is_remote_floor_switch_execution(const void* actor) {
    return !sFrames.empty() && sFrames.back().actor == actor && sFrames.back().remoteOnly;
}
bool is_floor_switch_actor(int actor) {
    return actor == fs::Push || actor == fs::Iron || actor == fs::Heavy;
}
bool is_floor_switch_momentary_output(int actor, uint32_t params, int flag) {
    return flag >= 0 && flag != 0xFF && fs::momentary(actor, params) &&
           fs::output_flag(actor, params) == flag;
}
bool loaded_floor_switch_owns_flag(int stage, int flag) {
    const auto scene = current_scene();
    if (!scene || scene->table != stage || flag < 0 || flag == 0xFF) return false;
    FindFlag search{scene->room, flag, false};
    fopAcIt_Judge(&visit_flag, &search);
    return search.found;
}
void sanitize_floor_switch_snapshot(dSv_info_c& copy) {
    const auto scene = current_scene();
    if (!scene) return;
    SnapshotCopy context{scene->room, &copy};
    fopAcIt_Judge(&sanitize_snapshot_actor, &context);
}
void repair_floor_switch_completion(int stage, int flag) {
    update_scene();
    if (!sScene || sScene->table != stage || flag < 0 || flag == 0xFF) return;
    FindFlag search{sScene->room, flag, true};
    fopAcIt_Judge(&visit_flag, &search);
}
} // namespace dusklight_online::game
