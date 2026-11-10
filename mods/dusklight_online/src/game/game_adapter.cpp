#include "dusklight_online/game/appearance.hpp"
#include "dusklight_online/game/coop_features.hpp"
#include "dusklight_online/game/game_adapter.hpp"
#include "dusklight_online/game/pickup_sync.hpp"
#include "dusklight_online/game/randomizer_check_names.hpp"
#include "dusklight_online/game/poe_sync.hpp"
#include "dusklight_online/game/trade_item_sync.hpp"
#include "dusklight_online/game/bomb_bag_sync.hpp"
#include "dusklight_online/game/bottle_sync.hpp"
#include "dusklight_online/game/cave_map_sync.hpp"
#include "dusklight_online/game/chat.hpp"
#include "dusklight_online/game/ooccoo_wire.hpp"
#include "dusklight_online/game/audio_bridge.hpp"
#include "dusklight_online/game/bomb_bridge.hpp"
#include "dusklight_online/game/collectible_visual_bridge.hpp"
#include "dusklight_online/game/floor_switch_bridge.hpp"
#include "dusklight_online/game/local_pose.hpp"
#include "dusklight_online/game/local_event_visibility.hpp"
#include "dusklight_online/game/randomizer_item_names.hpp"
#include "dusklight_online/game/remote_actor_bridge.hpp"
#include "dusklight_online/game/remote_pose.hpp"
#include "dusklight_online/game/pose_prediction.hpp"
#include "dusklight_online/game/visual_bridge.hpp"
#include "dusklight_online/logging.hpp"

#include "d/dolzel.h" // Public game umbrella must precede individual game headers.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_meter2.h"
#include "d/d_meter2_info.h"
#include "d/d_map_path_dmap.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "d/d_s_room.h"
#include "d/d_stage.h"
#include "d/d_vibration.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_arrow.h"
#include "d/actor/d_a_door_shutter.h"
#include "d/actor/d_a_obj_mirror_table.h"
#include "d/actor/d_a_obj_mirror_chain.h"
#include "d/actor/d_a_obj_spinLift.h"
#include "d/actor/d_a_player.h"
#include "d/actor/d_a_spinner.h"
#include "Z2AudioLib/Z2SeMgr.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "Z2AudioLib/Z2Audience.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_deletor.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_MemCard.h"
#include "dusk/frame_interpolation.h"
#include "dusk/multiplayer/remote_link_dummy.hpp"
#include "JSystem/JParticle/JPAEmitter.h"
#include "SSystem/SComponent/c_node.h"
#include "SSystem/SComponent/c_tag.h"
#include "SSystem/SComponent/c_tag_iter.h"
#include "SSystem/SComponent/c_tree_iter.h"
#include "f_pc/f_pc_base.h"
#include "f_pc/f_pc_deletor.h"
#include "f_pc/f_pc_layer.h"
#include "f_pc/f_pc_line.h"
#include "f_pc/f_pc_line_iter.h"
#include "mods/service.hpp"
#include "mods/svc/item.h"
#include "mods/svc/save.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include <zstd.h>

namespace dusklight_online::game {

DEFINE_HOOK(&daAlink_c::checkSetNpcTks, OoccooReunionHook);
DEFINE_HOOK(&daAlink_c::createNpcTks, OoccooWarpActorHook);
DEFINE_HOOK(&dMeter2Info_c::warpOutProc, OoccooWarpOutHook);
DEFINE_HOOK(&dSv_event_c::onEventBit, EventBitOnHook);
DEFINE_HOOK(&dSv_event_c::offEventBit, EventBitOffHook);
DEFINE_HOOK(&dSv_memBit_c::onTbox, MemoryTboxOnHook);
DEFINE_HOOK(&dSv_memBit_c::onItem, MemoryItemOnHook);
DEFINE_HOOK(&dSv_memBit_c::onSwitch, MemorySwitchOnHook);
DEFINE_HOOK(&dSv_memBit_c::offSwitch, MemorySwitchOffHook);
DEFINE_HOOK(&dSv_memBit_c::onDungeonItem, MemoryDungeonItemOnHook);
DEFINE_HOOK(&dComIfGs_onVisitedRoom, VisitedRoomOnHook);
DEFINE_HOOK(&dSv_player_get_item_c::onFirstBit, PlayerItemFirstOnHook);
DEFINE_HOOK(&dSv_player_get_item_c::offFirstBit, PlayerItemFirstOffHook);
DEFINE_HOOK(&dSv_player_item_c::setItem, PlayerItemSetHook);
DEFINE_HOOK(&dSv_player_collect_c::setCollect, PlayerCollectSetHook);
DEFINE_HOOK(&dSv_player_collect_c::onCollectCrystal, PlayerCrystalSetHook);
DEFINE_HOOK(&dSv_player_collect_c::onCollectMirror, PlayerMirrorSetHook);
DEFINE_HOOK(&dSv_fishing_info_c::addFishCount, FishingAddCountHook);
DEFINE_HOOK(&dSv_light_drop_c::setLightDropNum, LightDropNumSetHook);
DEFINE_HOOK(&dSv_light_drop_c::onLightDropGetFlag, LightDropFlagOnHook);
DEFINE_HOOK(static_cast<void (dSv_player_item_c::*)()>(&dSv_player_item_c::setEmptyBottle),
            EmptyBottleSetHook);
DEFINE_HOOK(static_cast<void (dSv_player_item_c::*)(u8)>(&dSv_player_item_c::setEmptyBottle),
            EmptyBottleItemSetHook);
DEFINE_HOOK(&dSv_event_c::setEventReg, EventRegSetHook);
DEFINE_HOOK(&dMsgObject_c::setSmellTypeLocal, SmellTypeSetHook);
DEFINE_HOOK(&dSv_player_status_b_c::onDarkClearLV, DarkClearSetHook);
DEFINE_HOOK(&dSv_player_status_b_c::onTransformLV, TransformSetHook);
DEFINE_HOOK(&dSv_letter_info_c::onLetterGetFlag, LetterGetSetHook);
DEFINE_HOOK(static_cast<void (*)(int, u8)>(&dComIfGs_setKeyNum), StageKeyNumSetHook);
DEFINE_HOOK(&dMeter2_c::moveKey, MeterMoveKeyHook);
DEFINE_HOOK(&dMeter2_c::moveLife, MeterMoveLifeHook);
DEFINE_HOOK(&dMeter2_c::moveRupee, MeterMoveRupeeHook);
DEFINE_HOOK(&fpcM_Execute, ProcessExecuteHook);
DEFINE_HOOK(&daDoor20_c::checkExecute, Door20CheckExecuteHook);
DEFINE_HOOK(&daDoor20_c::chkStopOpen, Door20StopOpenHook);
DEFINE_HOOK(&daObjMirrorTable_c::execute, MirrorTableExecuteHook);
DEFINE_HOOK_SYMBOL("daObjMirrorChain_Draw", int(daObjMirrorChain_c*), MirrorChainDrawHook);
DEFINE_HOOK(&dSv_info_c::onSwitch, InfoSwitchOnHook);
DEFINE_HOOK(&dSv_info_c::offSwitch, InfoSwitchOffHook);
DEFINE_HOOK(&daAlink_c::getDamageVec, PvpDamageVectorHook);
DEFINE_HOOK(&daArrow_c::atHitCallBack, BombArrowPvpHitHook);
DEFINE_HOOK(&daAlink_c::checkEnemyGroup, RemoteEnemyGroupHook);
DEFINE_HOOK(&daAlink_c::searchWolfLockEnemy, RemoteWolfLockHook);
DEFINE_HOOK(&daAlink_c::setAtnList, RemoteAttentionMarkHook);
DEFINE_HOOK(&fpcLnIt_Queue, SafeLineQueueHook);
DEFINE_HOOK(&fpcDt_ToQueue, DeleteTagRepairHook);
DEFINE_HOOK(&JPABaseEmitter::deleteAllParticle, NullParticleDeleteHook);
DEFINE_HOOK(&item_func_POU_SPIRIT, PoePickupHook);
DEFINE_HOOK_SYMBOL("toggleAutoSave", void(bool), ToggleAutoSaveHook);

namespace {
// More than the native save's full set of individually tracked facts.
constexpr size_t kMaxRecoveryFacts = 32768;

constexpr int kMaxSyncedDonationTotal = 10000;
constexpr int kSyncedFishSpeciesCount = 6;
constexpr int kMaxSyncedFishCount = 999;
constexpr uint32_t kRemoteSwitchRoomInitTicks = 3;
constexpr uint32_t kProgressionPromptDurationTicks = 8 * 30;
constexpr uint32_t kProgressionPromptHoldTicks = 30;
constexpr uint32_t kProgressionStateReadyMaxAgeTicks = 90;
constexpr uint32_t kPendingSyncReplyTimeoutTicks = 1800;
constexpr uint32_t kManualSyncRequestTimeoutTicks = 5 * 30;
constexpr uint32_t kOrdonReloadWarningTicks = 180;
constexpr std::string_view kOoccooSaveBlob = "ooccoo-progress-v2";
constexpr std::string_view kLegacyOoccooSaveBlob = "ooccoo-note-v1";
constexpr std::string_view kBombBagSaveWarpBlob = "bomb-bag-savewarp-v1";
static_assert(ooccoo::Parent == dItemNo_DUNGEON_EXIT_e);
static_assert(ooccoo::Junior == dItemNo_DUNGEON_BACK_e);
static_assert(ooccoo::Note == dItemNo_TKS_LETTER_e);
static_assert(ooccoo::ParentAgain == dItemNo_DUNGEON_EXIT_2_e);
static_assert(ooccoo::CityParent == dItemNo_LV7_DUNGEON_EXIT_e);
static_assert(ooccoo::None == dItemNo_NONE_e);
static_assert(ooccoo::FirstDungeon == dStage_SaveTbl_LV1);
static_assert(ooccoo::CityDungeon == dStage_SaveTbl_LV7);
static_assert(dStage_SaveTbl_LV2 == 17 && dStage_SaveTbl_LV3 == 18 &&
              dStage_SaveTbl_LV4 == 19 && dStage_SaveTbl_LV5 == 20 && dStage_SaveTbl_LV6 == 21);
static_assert(ST_DUNGEON == 1 && ST_BOSS_ROOM == 3);
constexpr std::string_view kBottleSourcesSaveBlob = "bottle-sources-v1";
constexpr uint16_t kTitleSyntheticEponaRescuedEventBit = 0x0601;
constexpr std::string_view kTitleDemoStage = "F_SP102";

bool visual_wire_trace_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("DUSK_MP_VISUAL_WIRE_TRACE");
        return value != nullptr && std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0 &&
               std::strcmp(value, "FALSE") != 0 &&
               std::strcmp(value, "off") != 0 &&
               std::strcmp(value, "OFF") != 0;
    }();
    return enabled;
}

std::string visual_wire_trace_keys(const std::vector<std::string>& keys) {
    if (keys.empty()) return "-";
    std::ostringstream out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i != 0) out << ',';
        out << keys[i];
    }
    return out.str();
}

struct VisualWireTraceAccumulator {
    LocalPoseMatrixScope scope = LocalPoseMatrixScope::None;
    uint64_t samples = 0;
    uint64_t normalizedWireBytes = 0;
    uint64_t peakNormalizedWireBytes = 0;
    uint64_t fullMsgpackBytes = 0;
    uint64_t preparedMsgpackBytes = 0;
    uint64_t deltaSamples = 0;
    uint64_t fullSamples = 0;
    uint64_t fullSnapshotWireBytes = 0;
    int preparedSizeBand = -1;
};

VisualWireTraceAccumulator sVisualWireTrace;

constexpr int kProgressionCueSewersStage = dStage_SaveTbl_PRISON;
constexpr int kProgressionCueWakeUpInJailSwitch = 27;
constexpr int kProgressionCueHyruleFieldStage = dStage_SaveTbl_FIELD;
constexpr int kProgressionCueEldinTwilightSwitch = 0x0C;
constexpr int kProgressionCueLanayruTwilightSwitch = 0x0D;
constexpr int kProgressionCueForestTempleStage = dStage_SaveTbl_FARON;
constexpr int kProgressionCueForestTempleSavePromptSwitch = 0x01;
constexpr int kProgressionCueGoronMinesStage = dStage_SaveTbl_ELDIN;
constexpr int kProgressionCueGoronMinesSavePromptSwitch = 0x7C;
constexpr int kProgressionCueLakebedTempleStage = dStage_SaveTbl_LANAYRU;
constexpr int kProgressionCueLakebedTempleSavePromptSwitch = 0x0E;
constexpr int kProgressionCueArbitersGroundsStage = dStage_SaveTbl_DESERT;
constexpr int kProgressionCueArbitersGroundsSavePromptSwitch = 0x0A;
constexpr int kProgressionCueSnowpeakRuinsStage = dStage_SaveTbl_SNOWPEAK;
constexpr int kProgressionCueSnowpeakRuinsSavePromptSwitch = 0x19;
constexpr int kProgressionCueCityInTheSkyStage = dStage_SaveTbl_LV7;
constexpr int kProgressionCueCityInTheSkySavePromptSwitch = 0x25;
constexpr int kProgressionCuePalaceOfTwilightStage = dStage_SaveTbl_LV8;
constexpr int kProgressionCuePalaceOfTwilightSavePromptSwitch = 0x16;
constexpr uint16_t kProgressionCueSewersCompleteEventBit = 0x6140;
constexpr uint16_t kProgressionCueFaronTwilightEventBit = 0x0640;
constexpr uint16_t kProgressionCueTempleOfTimeClearEventBit = 0x2004;
constexpr std::string_view kProgressionCueTempleOfTimeExitStage = "F_SP117";
constexpr std::string_view kProgressionCueSewersCompleteDestStage = "F_SP104";
constexpr std::string_view kProgressionCueFaronTwilightDestStage = "F_SP108";
constexpr std::string_view kProgressionCueFinalGanondorfStage = "D_MN09C";

struct ProgressionCueDescriptor {
    std::string_view cueKey;
    std::string_view expectedStage;
    int8_t requireWolf;
    std::string_view warpStage;
    int8_t warpRoom;
    int8_t warpLayer;
    int16_t warpStartPoint;
};

constexpr ProgressionCueDescriptor kProgressionCueDescriptors[] = {
    {"sewers_complete", "F_SP104", -1, "F_SP104", 1, -1, 30},
    {"faron_twilight_entered", "F_SP108", -1, "F_SP108", 0, -1, 0},
};

const ProgressionCueDescriptor* progression_cue_descriptor(std::string_view cueKey) {
    for (const ProgressionCueDescriptor& descriptor : kProgressionCueDescriptors) {
        if (descriptor.cueKey == cueKey) return &descriptor;
    }
    return nullptr;
}

int8_t local_player_wolf_form() {
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    if (player == nullptr || fopAcM_GetName(player) != fpcNm_ALINK_e) return -1;
    return static_cast<daAlink_c*>(player)->checkWolf() ? 1 : 0;
}

bool local_state_ready_for_cue(std::string_view cueKey) {
    const ProgressionCueDescriptor* descriptor = progression_cue_descriptor(cueKey);
    if (descriptor == nullptr) return true;
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr || descriptor->expectedStage != stage) return false;
    return descriptor->requireWolf < 0 || local_player_wolf_form() == descriptor->requireWolf;
}
constexpr int kPvpAttackLight = 1;
constexpr int kPvpAttackHeavy = 2;
constexpr int kPvpLightDamage = 2;
constexpr int kPvpSlingshotDamage = kPvpLightDamage / 2;
constexpr int kPvpClawshotDamage = 1;
constexpr int kPvpHeavyDamage = 4;
constexpr int kPvpIronBallDamage = 12;
constexpr int kPvpSpecialTechniqueDamage = 12;
constexpr std::string_view kPvpReactionClawshot = "clawshot";
constexpr std::string_view kPvpReactionSlingshot = "slingshot";
constexpr std::string_view kPvpReactionIronBallLaunch = "iron_ball_launch";
constexpr std::string_view kPvpReactionMortalDraw = "mortal_draw";
constexpr std::string_view kPvpReactionGreatSpin = "great_spin";
constexpr std::string_view kPvpReactionShieldBash = "shield_bash";
GameAdapter* sActiveAdapter = nullptr;

bool is_arbiter_spin_lift(void* process) {
    if (process == nullptr || fpcM_GetName(process) != fpcNm_Obj_SpinLift_e) return false;
    auto* lift = static_cast<daSpinLift_c*>(process);
    const char* stage = dComIfGp_getStartStageName();
    return fopAcM_GetRoomNo(lift) == 9 && lift->mModelType == 1 &&
           lift->getEndSw() == 0x11 && stage != nullptr &&
           std::strcmp(stage, "D_MN10") == 0;
}

void finish_remote_arbiter_spin_lift(void* process) {
    if (!is_arbiter_spin_lift(process)) return;
    auto* lift = static_cast<daSpinLift_c*>(process);
    if (lift->mpModel == nullptr || !fopAcM_isSwitch(lift, lift->getEndSw())) return;
    const float finishedY = lift->home.pos.y + lift->mMoveHeight;
    if (lift->current.pos.y >= finishedY - 0.5f) return;

    // A remote completion bit can arrive while the lift is already loaded.
    // Its native moveLift() then stops watching the trigger switch, leaving
    // the drawn model below the floor until the room is reloaded. Match the
    // native create path for an already completed lift, including its moving
    // background matrix, so the visible rail and collision agree this frame.
    lift->current.pos.y = finishedY;
    lift->init_modeMoveEnd();
    lift->setBaseMtx();
    MTXCopy(lift->mpModel->getBaseTRMtx(), lift->mBgMtx);
    if (lift->mpBgW != nullptr && lift->mpBgW->ChkUsed()) lift->mpBgW->Move();
}
std::vector<void*> sExecutingProcessStack;
std::vector<int> sWebDeleteTimerStack;
std::vector<int> sRoomActorActionStateStack;
std::vector<int> sRemoteMoveboxPushPullKeepStack;
std::unordered_map<void*, int> sDoor20ExecuteModes;
uint32_t sDoor20StopOpenDepth = 0;
std::vector<bool> sInfoSwitchWasSetStack;
std::vector<bool> sInfoSwitchWasSetOffStack;
std::vector<bool> sMemorySwitchWasSetStack;
std::vector<bool> sMemorySwitchWasSetOffStack;
std::vector<bool> sItemFirstWasOwnedStack;
std::vector<int> sTradeItemPreviousStack;
std::vector<int> sPoePickupPrevious;
std::vector<bool> sItemFirstWasOwnedOffStack;
std::vector<int> sStageKeyPreviousCounts;
struct PendingMeterKeyMutation {
    bool publish = false;
    int stage = -1;
    int previous = -1;
};
std::vector<PendingMeterKeyMutation> sPendingMeterKeyMutations;
struct PendingMeterScalarMutation {
    bool publish = false;
    int previous = 0;
};
std::vector<PendingMeterScalarMutation> sPendingMeterLifeMutations;
std::vector<PendingMeterScalarMutation> sPendingMeterRupeeMutations;
std::vector<int> sLightDropPreviousCounts;
struct PendingBottleMutation {
    int previous = -1;
    uint8_t sourceItem = dItemNo_NONE_e;
};
std::vector<PendingBottleMutation> sEmptyBottleMutations;
std::vector<PendingBottleMutation> sEmptyBottleItemMutations;
std::vector<int> sVisitedRoomPendingRegions;
daAlink_c* sSyntheticDamagePlayer = nullptr;
dCcD_GObjInf* sSyntheticDamageObject = nullptr;
cXyz* sSyntheticDamageVector = nullptr;
bool sWorldSyncEnabled = false;

bool is_remote_link_actor(fopAc_ac_c* actor) {
    return actor != nullptr && fopAcM_GetName(actor) == fpcNm_REMOTE_LINK_e;
}

HookAction remote_enemy_group_pre(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || !is_remote_link_actor(mods::arg<fopAc_ac_c*>(args, 0))) {
        return HOOK_CONTINUE;
    }
    *static_cast<BOOL*>(retval) = TRUE;
    return HOOK_SKIP_ORIGINAL;
}

HookAction remote_wolf_lock_pre(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    auto* actor = mods::arg<fopAc_ac_c*>(args, 1);
    auto* outActor = static_cast<fopAc_ac_c**>(mods::arg<void*>(args, 2));
    if (link == nullptr || !is_remote_link_actor(actor)) return HOOK_CONTINUE;

    // Remote Link is an actor target, not an enemy-class instance, so avoid
    // fopEn_enemy_c::checkWolfNoLock here.
    for (int i = 0; i < link->mWolfLockNum; ++i) {
        if (link->mWolfLockAcKeep[i].getActor() == actor) return HOOK_SKIP_ORIGINAL;
    }
    if (outActor != nullptr &&
        (actor->attention_info.flags & (fopAc_AttnFlag_BATTLE_e | fopAc_AttnFlag_LOCK_e))) {
        const float distance2 = link->current.pos.abs2(actor->eyePos);
        if (actor->eyePos.y >= link->current.pos.y - 50.0f && distance2 < link->field_0x3478) {
            link->field_0x3478 = distance2;
            *outActor = actor;
        }
    }
    return HOOK_SKIP_ORIGINAL;
}

void remote_attention_mark_post(ModContext*, void* args, void*, void*) {
    auto* link = mods::arg<daAlink_c*>(args, 0);
    if (link != nullptr && is_remote_link_actor(link->mTargetedActor)) {
        link->mZ2Link.setMarkState(3);
    }
}

int safe_line_method_call(create_tag_class* createTag, method_filter* filter) {
    if (createTag == nullptr || filter == nullptr || !cTg_IsUse(createTag)) return 0;
    auto* process = static_cast<base_process_class*>(createTag->mpTagData);
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(process);
#if defined(__ANDROID__) && defined(__aarch64__)
    // Android tags the top byte of heap pointers. Check the address bits, but
    // keep the original tagged pointer when accessing the process.
    const std::uintptr_t addressToCheck = address & 0x00FFFFFFFFFFFFFFULL;
#else
    const std::uintptr_t addressToCheck = address;
#endif
    if (addressToCheck < 0x10000 ||
        (sizeof(std::uintptr_t) == 8 && addressToCheck > 0x00007FFFFFFFFFFFULL) ||
        process->state.init_state == 3 || process->layer_tag.layer == nullptr) {
        return 0;
    }
    layer_class* savedLayer = fpcLy_CurrentLayer();
    fpcLy_SetCurrentLayer(process->layer_tag.layer);
    const int result = cTgIt_MethodCall(createTag, filter);
    fpcLy_SetCurrentLayer(savedLayer);
    return result;
}

HookAction safe_line_queue_pre(ModContext*, void* args, void*, void*) {
    method_filter filter;
    filter.mpMethodFunc = reinterpret_cast<cNdIt_MethodFunc>(
        mods::arg<fpcLnIt_QueueFunc>(args, 0));
    filter.mpUserData = nullptr;
    cTrIt_Method(&g_fpcLn_Queue,
                 reinterpret_cast<cNdIt_MethodFunc>(&safe_line_method_call), &filter);
    return HOOK_SKIP_ORIGINAL;
}

HookAction delete_tag_repair_pre(ModContext*, void* args, void*, void*) {
    auto* process = mods::arg<base_process_class*>(args, 0);
    if (process == nullptr || process->unk_0xA == 1 || fpcBs_IsDelete(process) != 1) {
        return HOOK_CONTINUE;
    }
    create_tag_class* deleteTag = &process->delete_tag.base;
    node_class* node = &deleteTag->mpNode;
    if (!cTg_IsUse(deleteTag) &&
        (node->mpPrevNode != nullptr || node->mpNextNode != nullptr ||
         node->mpData != process)) {
        cNd_ForcedClear(node);
        cNd_SetObject(node, process);
    }
    return HOOK_CONTINUE;
}

HookAction null_particle_delete_pre(ModContext*, void* args, void*, void*) {
    // A member call with a null emitter can only dereference invalid state.
    // Keep this guard active during disconnect/reload teardown as well, when
    // sync-world has already been reset but Remote Link is still deleting.
    return mods::arg<JPABaseEmitter*>(args, 0) == nullptr ? HOOK_SKIP_ORIGINAL :
                                                           HOOK_CONTINUE;
}

int angle_y_from_delta(float dx, float dz) {
    constexpr float kAngleScale = 32768.0f / 3.14159265358979323846f;
    return static_cast<int>(std::atan2(dx, dz) * kAngleScale);
}

class SyntheticDamageVectorScope {
public:
    SyntheticDamageVectorScope(daAlink_c* player, dCcD_GObjInf* object, cXyz* vector)
        : previousPlayer_(sSyntheticDamagePlayer), previousObject_(sSyntheticDamageObject),
          previousVector_(sSyntheticDamageVector) {
        sSyntheticDamagePlayer = player;
        sSyntheticDamageObject = object;
        sSyntheticDamageVector = vector;
    }

    ~SyntheticDamageVectorScope() {
        sSyntheticDamagePlayer = previousPlayer_;
        sSyntheticDamageObject = previousObject_;
        sSyntheticDamageVector = previousVector_;
    }

private:
    daAlink_c* previousPlayer_;
    dCcD_GObjInf* previousObject_;
    cXyz* previousVector_;
};

HookAction pvp_damage_vector_pre(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || sSyntheticDamageVector == nullptr ||
        mods::arg<daAlink_c*>(args, 0) != sSyntheticDamagePlayer ||
        mods::arg<dCcD_GObjInf*>(args, 1) != sSyntheticDamageObject) {
        return HOOK_CONTINUE;
    }
    *static_cast<cXyz**>(retval) = sSyntheticDamageVector;
    return HOOK_SKIP_ORIGINAL;
}

void remote_link_pvp_target_hit(fopAc_ac_c* remoteLinkActor, fopAc_ac_c* attackActor,
                                dCcD_GObjInf* attackInfo) {
    if (sActiveAdapter != nullptr) {
        sActiveAdapter->report_pvp_target_hit(remoteLinkActor, attackActor, attackInfo);
    }
}

void bomb_arrow_pvp_hit_post(ModContext*, void* args, void*, void*) {
    auto* arrow = mods::arg<daArrow_c*>(args, 0);
    if (arrow == nullptr || !arrow->checkBombArrow()) return;
    // Bomb arrows suppress the target hit callback; their own hit callback
    // still runs, so forward remote-player impacts through the usual PvP path.
    remote_link_pvp_target_hit(mods::arg<fopAc_ac_c*>(args, 2), arrow,
                               mods::arg<dCcD_GObjInf*>(args, 1));
}

bool apply_pvp_player_damage(int attackClass, bool ironBallLaunch, int damage,
                             float sourceX, float sourceZ) {
    daAlink_c* player = static_cast<daAlink_c*>(daPy_getPlayerActorClass());
    if (player == nullptr) {
        daPy_py_c::setPlayerDamage(damage, TRUE);
        return false;
    }

    const s16 hitAngle = static_cast<s16>(
        angle_y_from_delta(player->current.pos.x - sourceX, player->current.pos.z - sourceZ));
    if (!player->checkWolf() && player->checkModeFlg(daAlink_c::MODE_SWIMMING)) {
        player->setDamagePointNormal(damage);
        player->current.angle.y = hitAngle;
        return player->procSwimDamageInit(nullptr) != 0;
    }
    if (ironBallLaunch) {
        player->current.angle.y = hitAngle;
        player->setDamagePointNormal(damage);
        return player->procCoLargeDamageInit(-1, FALSE, 0, 0, nullptr, 0) != 0;
    }
    if (attackClass == kPvpAttackHeavy ||
        player->checkModeFlg(daAlink_c::MODE_PLAYER_FLY)) {
        player->setThrowDamage(hitAngle, 35.0f, 22.0f, damage, 1, 0);
        return player->procCoLargeDamageInit(-3, TRUE, 0, 0, nullptr, 0) != 0;
    }

    player->setDamagePointNormal(damage);
    if (player->checkWolf()) {
        player->field_0x311e = hitAngle;
        cXyz damageVector(cM_ssin(hitAngle), 0.0f, cM_scos(hitAngle));
        dCcD_GObjInf syntheticHit;
        dCcD_GObjInf syntheticAttack;
        syntheticAttack.SetAtMtrl(dCcD_MTRL_NONE);
        syntheticHit.SetTgHit(&syntheticAttack);
        SyntheticDamageVectorScope vectorScope(player, &syntheticHit, &damageVector);
        return player->procWolfDamageInit(&syntheticHit) != 0;
    }

    player->field_0x3102 = static_cast<s16>(hitAngle - 0x8000);
    return player->procDamageInit(nullptr, 1) != 0;
}

bool apply_pvp_player_shield_block(int attackClass, float sourceX, float sourceZ) {
    daAlink_c* player = static_cast<daAlink_c*>(daPy_getPlayerActorClass());
    if (player == nullptr || player->checkWolf()) return false;

    const s16 hitAngle = static_cast<s16>(
        angle_y_from_delta(player->current.pos.x - sourceX, player->current.pos.z - sourceZ));
    cXyz damageVector(cM_ssin(hitAngle) * 10.0f, 0.0f, cM_scos(hitAngle) * 10.0f);
    const int atSpl = attackClass == kPvpAttackHeavy ? 1 : 0;
    const int vibration = attackClass == kPvpAttackHeavy ? VIBMODE_S_POWER4 : VIBMODE_S_POWER3;
    dComIfGp_getVibration().StartShock(vibration, 1, cXyz(0.0f, 1.0f, 0.0f));
    const int mapInfo = player->checkWoodShieldEquipNotIronBall() &&
                                !player->checkMagicArmorNoDamage()
                            ? 0x29
                            : 0x28;
    player->playerStartCollisionSE(Z2SE_HIT_METAL_WEAPON, mapInfo);

    dCcD_GObjInf syntheticHit;
    dCcD_GObjInf syntheticAttack;
    syntheticAttack.SetAtMtrl(dCcD_MTRL_NONE);
    syntheticHit.SetTgHit(&syntheticAttack);
    SyntheticDamageVectorScope vectorScope(player, &syntheticHit, &damageVector);
    return player->procGuardSlipInit(atSpl, &syntheticHit) != 0;
}

bool apply_pvp_player_shield_bash(float sourceX, float sourceZ) {
    daAlink_c* player = static_cast<daAlink_c*>(daPy_getPlayerActorClass());
    if (player == nullptr) return false;
    if (player->checkWolf()) {
        const s16 hitAngle = static_cast<s16>(
            angle_y_from_delta(player->current.pos.x - sourceX,
                               player->current.pos.z - sourceZ));
        player->setThrowDamage(hitAngle, 35.0f, 22.0f, 0, 1, 0);
        return player->procCoLargeDamageInit(-3, TRUE, 0, 0, nullptr, 0) != 0;
    }
    return player->procGuardBreakInit() != 0;
}

#pragma pack(push, 1)
struct ManualSyncStatePacket {
    char stageName[8];
    int8_t roomNo;
    int8_t layer;
    int16_t startPoint;
};
#pragma pack(pop)

constexpr size_t kManualSyncStatePacketSize = sizeof(ManualSyncStatePacket) + sizeof(dSv_info_c);
constexpr int kManualSyncDefaultStartEvent = 0xCA;
uint64_t manual_sync_request_id(const nlohmann::json& message) {
    const auto it = message.find("manual_sync_request_id");
    return it != message.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
}
constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const size_t rest = data.size() - i;
        uint32_t chunk = uint32_t(data[i]) << 16;
        if (rest > 1) chunk |= uint32_t(data[i + 1]) << 8;
        if (rest > 2) chunk |= data[i + 2];
        out.push_back(kBase64Chars[(chunk >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(chunk >> 12) & 0x3F]);
        out.push_back(rest > 1 ? kBase64Chars[(chunk >> 6) & 0x3F] : '=');
        out.push_back(rest > 2 ? kBase64Chars[chunk & 0x3F] : '=');
    }
    return out;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>& out) {
    if (text.empty() || text.size() % 4 != 0) return false;
    static const auto lookup = [] {
        std::array<int8_t, 256> table{};
        table.fill(-1);
        for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(kBase64Chars[i])] = i;
        return table;
    }();
    out.clear();
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        uint32_t chunk = 0;
        int pads = 0;
        for (size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                ++pads;
                chunk <<= 6;
                continue;
            }
            const int value = lookup[static_cast<uint8_t>(c)];
            if (value < 0 || pads != 0) return false;
            chunk = (chunk << 6) | static_cast<uint32_t>(value);
        }
        out.push_back(static_cast<uint8_t>((chunk >> 16) & 0xFF));
        if (pads < 2) out.push_back(static_cast<uint8_t>((chunk >> 8) & 0xFF));
        if (pads < 1) out.push_back(static_cast<uint8_t>(chunk & 0xFF));
    }
    return true;
}

constexpr uint16_t kUnsyncedEventBits[] = {0x0580, 0x4D08, 0x0502, 0x6140};
static_assert(dSv_event_flag_c::M_033 == 0x0840);
static_assert(dSv_event_flag_c::M_035 == 0x0810);
static_assert(dSv_event_flag_c::M_036 == 0x0808);

bool is_unsynced_event_bit(uint16_t flag) {
    return std::find(std::begin(kUnsyncedEventBits), std::end(kUnsyncedEventBits), flag) !=
           std::end(kUnsyncedEventBits);
}

bool is_story_reload_event_bit(uint16_t flag) {
    return flag == 0x2B08 || flag == 0x0880 || flag == 0x4A20 || flag == 0x4A40 ||
           flag == 0x4510 || flag == 0x1202 || flag == 0x0501;
}

bool is_ordon_day_boundary_event_bit(uint16_t flag) {
    return flag == 0x4A20 || flag == 0x4A40 || flag == 0x4510;
}

bool is_ordon_day_boundary_stage(std::string_view stage) {
    return stage == "F_SP00" || stage == "F_SP103" || stage == "F_SP104";
}

bool is_mirror_complete_reload_stage(std::string_view stage) {
    return stage == "F_SP118" || stage == "F_SP125";
}

bool is_zora_thaw_reload_area(std::string_view stage, int room) {
    return stage == "F_SP112" || stage == "F_SP113" || stage == "F_SP126" ||
           (stage == "F_SP115" && room == 0);
}

bool is_faron_warp_sequence_event_bit(uint16_t flag) {
    return flag == 0x1202 || flag == 0x0501;
}

bool is_faron_warp_sequence_switch(int stage, int flag) {
    if (stage != dStage_SaveTbl_FARON) return false;
    return flag == 72 || flag == 27 || flag == 64 || flag == 14 || flag == 15 || flag == 71;
}

bool is_unsynced_switch_bit(int stage, int flag) {
    // Keep the post-Goats-2 Ranch exit trigger available for each player.
    return (stage == dStage_SaveTbl_ORDON && (flag == 0x68 || flag == 0x2F || flag == 0x16)) ||
           (stage == dStage_SaveTbl_PRISON && flag == 0x1F) ||
           (stage == dStage_SaveTbl_LANAYRU && flag == 0x1E);
}

// F_SP121's two wagon-escort R_Gate actors use dungeon-switch bits 0x81
// (room 15) and 0x82 (room 3). Their unlock events and collision must stay
// native to each player's escort, not arrive as a remote room switch.
constexpr bool is_local_escort_gate_switch(int stage, int flag, int actor, int room,
                                           std::string_view stageName) {
    return stage == dStage_SaveTbl_FIELD && stageName == "F_SP121" &&
           actor == fpcNm_Obj_RiderGate_e &&
           ((room == 15 && flag == 0x81) || (room == 3 && flag == 0x82));
}

// The field-stage key count is initialized to two only for the wagon escort
// and reset to zero on other scene transitions by dComIfGp_setNextStage.
constexpr bool is_local_escort_key_count(int stage) {
    return stage == dStage_SaveTbl_FIELD;
}

static_assert(is_local_escort_gate_switch(dStage_SaveTbl_FIELD, 0x81,
                                          fpcNm_Obj_RiderGate_e, 15, "F_SP121"));
static_assert(is_local_escort_gate_switch(dStage_SaveTbl_FIELD, 0x82,
                                          fpcNm_Obj_RiderGate_e, 3, "F_SP121"));
static_assert(!is_local_escort_gate_switch(dStage_SaveTbl_FIELD, 0x81,
                                           fpcNm_Obj_RiderGate_e, 3, "F_SP121"));
static_assert(!is_local_escort_gate_switch(dStage_SaveTbl_FIELD, 0x81,
                                           fpcNm_Obj_RiderGate_e, 15, "F_SP122"));
static_assert(!is_local_escort_gate_switch(dStage_SaveTbl_LV1, 0x81,
                                           fpcNm_Obj_RiderGate_e, 15, "F_SP121"));
static_assert(is_local_escort_key_count(dStage_SaveTbl_FIELD));
static_assert(!is_local_escort_key_count(dStage_SaveTbl_LV1));

bool is_lakebed_staircase_switch(int stage, int flag) {
    return stage == dStage_SaveTbl_LV3 && flag >= 0 && flag <= 3;
}

bool is_local_lakebed_staircase_switch_write(int stage, int flag) {
    if (!is_lakebed_staircase_switch(stage, flag) ||
        dComIfGp_roomControl_getStayNo() != 3) return false;
    const char* stageName = dComIfGp_getStartStageName();
    return stageName != nullptr && std::strcmp(stageName, "D_MN01") == 0;
}

enum class RemoteSwitchPolicyMode { ApplyImmediately, DeferUntilRoomInit, SuppressRemote };

struct RemoteSwitchPolicy {
    int stage;
    int flag;
    RemoteSwitchPolicyMode mode;
    const char* stageName;
    int room;
};

constexpr RemoteSwitchPolicy kRemoteSwitchPolicies[] = {
    {0, 4, RemoteSwitchPolicyMode::DeferUntilRoomInit, "F_SP104", 1},
    {0, 11, RemoteSwitchPolicyMode::SuppressRemote, "F_SP104", 1},
    {0, 12, RemoteSwitchPolicyMode::DeferUntilRoomInit, "F_SP104", 1},
    {19, 0x26, RemoteSwitchPolicyMode::DeferUntilRoomInit, "D_MN10", -1},
    {19, 0x43, RemoteSwitchPolicyMode::DeferUntilRoomInit, "D_MN10", -1},
    {23, 63, RemoteSwitchPolicyMode::SuppressRemote, "D_MN08", 0},
};

const RemoteSwitchPolicy* remote_switch_policy(int stage, int flag) {
    for (const auto& policy : kRemoteSwitchPolicies) {
        if (policy.stage == stage && policy.flag == flag) return &policy;
    }
    return nullptr;
}

bool is_small_key_door_switch_actor(int actorName) {
    switch (actorName) {
    case fpcNm_DOOR20_e:
    case fpcNm_L1MBOSS_DOOR_e:
    case fpcNm_Obj_Kshutter_e:
    case fpcNm_Obj_CRVGATE_e:
    case fpcNm_Obj_KkrGate_e:
    case fpcNm_Obj_RiderGate_e:
        return true;
    default:
        return false;
    }
}

bool is_web_switch_actor(int actorName) {
    return actorName == fpcNm_OBJ_WEB0_e || actorName == fpcNm_OBJ_WEB1_e;
}

bool is_live_torch_switch_actor(int actorName) {
    return actorName == fpcNm_Obj_Lv1Cdl00_e;
}

bool is_permanent_room_actor(int actorName) {
    switch (actorName) {
    case fpcNm_BkyRock_e:
    case fpcNm_Obj_HBombkoya_e:
    case fpcNm_Obj_RfHole_e:
    case fpcNm_Obj_BmWindow_e:
    case fpcNm_Obj_WellCover_e:
    case fpcNm_Obj_BBox_e:
    case fpcNm_Obj_Lv5FBoard_e:
    case fpcNm_Obj_Lv4DigSand_e:
    case fpcNm_Obj_Picture_e:
    case fpcNm_Obj_IceWall_e:
    case fpcNm_Obj_Lv5SwIce_e:
        return true;
    default:
        return false;
    }
}

bool is_room_action_actor(int actorName) {
    return is_permanent_room_actor(actorName) || actorName == fpcNm_Obj_Movebox_e ||
           actorName == fpcNm_Obj_RotStair_e || actorName == fpcNm_Obj_IceBlock_e;
}

int permanent_room_actor_switch_flag(int actorName, uint32_t params) {
    switch (actorName) {
    case fpcNm_BkyRock_e:
    case fpcNm_Obj_Picture_e:
        return static_cast<int>((params >> 4) & 0xFF);
    case fpcNm_Obj_HBombkoya_e:
        return static_cast<int>((params >> 8) & 0xFF);
    default:
        return is_permanent_room_actor(actorName) ? static_cast<int>(params & 0xFF) : -1;
    }
}

bool valid_room_actor_action(int actorName, RoomActorAction action) {
    switch (actorName) {
    case fpcNm_BkyRock_e:
        return action == RoomActorAction::DamageStage || action == RoomActorAction::Break;
    case fpcNm_Obj_Lv4DigSand_e:
        return action == RoomActorAction::DigStart || action == RoomActorAction::Break;
    case fpcNm_Obj_IceWall_e:
        return action == RoomActorAction::PartialBreak || action == RoomActorAction::Break;
    case fpcNm_Obj_Movebox_e:
        return action == RoomActorAction::MoveStep;
    case fpcNm_Obj_RotStair_e:
        return action == RoomActorAction::RotateTo;
    case fpcNm_Obj_IceBlock_e:
        return action == RoomActorAction::Slide;
    default:
        return is_permanent_room_actor(actorName) && action == RoomActorAction::Break;
    }
}

bool is_group2_lifecycle_actor(int actorName) {
    return actorName == fpcNm_Tag_Mhint_e || actorName == fpcNm_Tag_Mmsg_e ||
           actorName == fpcNm_Tag_Mstop_e || actorName == fpcNm_Tag_TheBHint_e ||
           actorName == fpcNm_Obj_Timer_e || actorName == fpcNm_NPC_BLUENS_e ||
           actorName == fpcNm_SWC00_e;
}

void* exact_local_switch_actor_context(bool set) {
    if (sExecutingProcessStack.empty()) return nullptr;
    void* process = sExecutingProcessStack.back();
    if (process == nullptr || !fopAcM_IsActor(process)) return nullptr;
    const int actor = fpcM_GetName(process);
    if (is_floor_switch_actor(actor)) return process;
    if (set) {
        if (actor == fpcNm_Tag_Mhint_e || actor == fpcNm_Tag_Mmsg_e ||
            actor == fpcNm_NPC_BLUENS_e || actor == fpcNm_SWC00_e) {
            return process;
        }
        if (is_small_key_door_switch_actor(actor) &&
            !(actor == fpcNm_DOOR20_e && sDoor20StopOpenDepth != 0)) {
            return process;
        }
        if (is_web_switch_actor(actor) || is_room_action_actor(actor) ||
            is_live_torch_switch_actor(actor)) return process;
    } else {
        if (actor == fpcNm_Obj_Timer_e || actor == fpcNm_Obj_Movebox_e ||
            actor == fpcNm_Obj_RotStair_e || actor == fpcNm_Obj_IceBlock_e ||
            is_live_torch_switch_actor(actor)) return process;
    }
    return nullptr;
}

bool valid_stage(int stage);
int current_stage_table();

void publish_room_actor_action(void* process, RoomActorAction action, int actionArgument = 0) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() || process == nullptr ||
        !fopAcM_IsActor(process)) return;
    const int actorName = fpcM_GetName(process);
    if (!is_room_action_actor(actorName) || !valid_room_actor_action(actorName, action)) return;
    const int stage = current_stage_table();
    const int room = fopAcM_GetRoomNo(static_cast<fopAc_ac_c*>(process));
    const char* stageName = dComIfGp_getStartStageName();
    if (!valid_stage(stage) || room < 0 || room >= 64 || stageName == nullptr) return;
    sActiveAdapter->publish_local({
        {"type", "room_actor_action"}, {"stage", stage}, {"room", room},
        {"source_actor", actorName}, {"source_params", fopAcM_GetParam(process)},
        {"source_stage", stageName}, {"action", static_cast<int>(action)},
        {"action_arg", actionArgument},
    });
}

void publish_lakebed_staircase_switch_state(void* process, int target) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() || process == nullptr ||
        !fopAcM_IsActor(process) || fpcM_GetName(process) != fpcNm_Obj_RotStair_e ||
        target < 0 || target >= 4) return;
    const int stage = current_stage_table();
    if (stage != dStage_SaveTbl_LV3) return;
    // The room action is queued first. These durable state messages then reach
    // every peer, including peers outside the room, without starting their
    // loaded staircase before the safe action has initialized it.
    for (int i = 0; i < 4; ++i) {
        if (i == target) continue;
        sActiveAdapter->publish_local(
            {{"type", "switch_bit"}, {"stage", stage}, {"flag", i}, {"set", false}});
    }
    sActiveAdapter->publish_local(
        {{"type", "switch_bit"}, {"stage", stage}, {"flag", target}, {"set", true}});
}

bool is_sewers_progression_switch(int stage, int flag) {
    if (stage != dStage_SaveTbl_PRISON) return false;
    switch (flag) {
    case 1: case 2: case 3: case 6: case 13: case 17:
    case 18: case 19: case 20: case 21: case 22: case 24: case 27:
        return true;
    default:
        return false;
    }
}

bool is_eldin_gorge_bridge_completion(int stage, int flag, int actor, int room,
                                      uint32_t params) {
    return stage == 6 && flag == 24 && actor == fpcNm_Tag_Mhint_e && room == 3 &&
           params == 0xFFFFFFFF;
}

bool is_sacred_grove_block_switch(std::string_view stageName, int stage, int room,
                                  int flag, int actor, uint32_t params, int angleX,
                                  bool set) {
    // This one-shot area trigger sets the block's saved path switch before
    // the block reaches its lower point. The block's later write is a no-op.
    return set && stageName == "F_SP117" && stage == dStage_SaveTbl_GROVE &&
           room == 1 && flag == 4 && actor == fpcNm_SWC00_e &&
           params == 0xFF00FF04 && angleX == 0x0FFF;
}

int twilight_completion_level_for_stage(std::string_view stage, int room) {
    if (stage == "F_SP108" || stage == "R_SP108" || stage == "D_SB10" ||
        stage == "F_SP105") return 0;
    if (stage == "F_SP121") {
        if (room == 0 || (room >= 2 && room <= 5) || room == 7) return 1;
        if (room >= 9 && room <= 14) return 2;
        return -1;
    }
    if (stage == "F_SP109" || stage == "F_SP110" || stage == "R_SP109" ||
        stage == "F_SP111" || stage == "R_SP209") return 1;
    if (stage == "F_SP112" || stage == "F_SP113" || stage == "F_SP115" ||
        stage == "F_SP116" || stage == "F_SP122" || stage == "F_SP126" ||
        stage == "R_SP116") return 2;
    return -1;
}

bool valid_stage(int stage) {
    return stage >= 0 && stage < dSv_save_c::STAGE_MAX;
}

std::optional<int> current_freestanding_check_flag(std::string_view checkName) {
    constexpr std::string_view prefix = "freestanding:";
    if (!checkName.starts_with(prefix)) return std::nullopt;

    const size_t stageEnd = checkName.find(':', prefix.size());
    if (stageEnd == std::string_view::npos) return std::nullopt;
    const char* currentStage = dComIfGp_getStartStageName();
    if (currentStage == nullptr ||
        checkName.substr(prefix.size(), stageEnd - prefix.size()) != currentStage) {
        return std::nullopt;
    }

    const std::string_view value = checkName.substr(stageEnd + 1);
    if (value.empty()) return std::nullopt;
    int globalBit = 0;
    for (const char character : value) {
        if (character < '0' || character > '9') return std::nullopt;
        globalBit = globalBit * 10 + (character - '0');
        if (globalBit >= dSv_info_c::MEMORY_ITEM + dSv_info_c::DAN_ITEM) {
            return std::nullopt;
        }
    }
    // ItemService names freestanding checks with the actor's global item bit.
    // Stage memory stores the 0-based portion after MEMORY_ITEM (0x80).
    if (globalBit < dSv_info_c::MEMORY_ITEM) return std::nullopt;
    return globalBit - dSv_info_c::MEMORY_ITEM;
}

int current_stage_table() {
    stage_stag_info_class* info = dComIfGp_getStageStagInfo();
    return info == nullptr ? -1 : dStage_stagInfo_GetSaveTbl(info);
}

dSv_memBit_c& stage_bits(int stage) {
    if (stage == current_stage_table()) {
        return g_dComIfG_gameInfo.info.getMemory().getBit();
    }
    return g_dComIfG_gameInfo.info.getSavedata().getSave(stage).getBit();
}

// Stage-table numbers are not scene identity. In particular, caves are also
// ST_DUNGEON, and the City shop shares the main dungeon's save table.
ooccoo::Scene current_ooccoo_scene() {
    auto* info = dComIfGp_getStageStagInfo();
    const char* name = dComIfGp_getStartStageName();
    return {name != nullptr ? name : "", current_stage_table(),
            info != nullptr ? static_cast<int>(dStage_stagInfo_GetSTType(info)) : -1,
            static_cast<int>(dComIfGp_roomControl_getStayNo())};
}

int local_ooccoo_return_owner() {
    auto& mark = g_dComIfG_gameInfo.info.getPlayer().getPlayerLastMarkInfo();
    const char* name = mark.getName();
    const auto* end = static_cast<const char*>(std::memchr(name, '\0', sizeof(mark.mName)));
    if (end == nullptr) return -1;
    const cXyz pos = mark.getPos();
    return ooccoo::return_owner({mark.getWarpAcceptStage(),
        std::string_view(name, static_cast<size_t>(end - name)), mark.getRoomNo(),
        pos.x, pos.y, pos.z});
}

ooccoo::Progress native_ooccoo_facts(bool includeAcquisitions, bool includeLive) {
    ooccoo::Progress facts;
    const int here = includeLive ? ooccoo::scene_dungeon(current_ooccoo_scene()) : -1;
    for (int stage = ooccoo::FirstDungeon; stage <= ooccoo::CityDungeon; ++stage) {
        const auto& saved = g_dComIfG_gameInfo.info.getSavedata().getSave(stage).getBit();
        const auto& live = g_dComIfG_gameInfo.info.getMemory().getBit();
        const bool completed = saved.isStageBossEnemy() ||
            (here == stage && live.isStageBossEnemy());
        // onStageBossEnemy also sets OOCCOO_NOTE. It is not a pickup receipt.
        const bool acquired = saved.isDungeonItemWarp() ||
            (here == stage && live.isDungeonItemWarp());
        if (completed) facts.completed |= ooccoo::dungeon_bit(stage);
        else if (includeAcquisitions && acquired) facts.acquired |= ooccoo::dungeon_bit(stage);
    }
    if (includeAcquisitions) {
        facts.citySpecial = dComIfGs_isItemFirstBit(dItemNo_LV7_DUNGEON_EXIT_e) ||
            dComIfGs_getItem(SLOT_18, false) == dItemNo_LV7_DUNGEON_EXIT_e;
        facts.unboundNote = dComIfGs_isItemFirstBit(dItemNo_TKS_LETTER_e);
        // A genuine local Ooccoo warp already has the exact native return mark.
        // Preserve it for catch-up without borrowing arbitrary save coordinates.
        const int owner = local_ooccoo_return_owner();
        if (dComIfGs_getItem(SLOT_18, false) == dItemNo_DUNGEON_BACK_e &&
            ooccoo::is_dungeon(owner) && (facts.acquired & ooccoo::dungeon_bit(owner))) {
            auto& mark = g_dComIfG_gameInfo.info.getPlayer().getPlayerLastMarkInfo();
            const std::string_view name(mark.getName());
            const auto baseName = ooccoo::DungeonNames[owner - ooccoo::FirstDungeon];
            const cXyz pos = mark.getPos();
            ooccoo::ReturnAnchor anchor{true,
                static_cast<uint8_t>(name.size() == baseName.size() + 1),
                static_cast<uint8_t>(mark.getRoomNo()), pos.x, pos.y, pos.z,
                mark.getAngleY()};
            if (ooccoo::valid_anchor(owner, anchor))
                facts.anchors[owner - ooccoo::FirstDungeon] = anchor;
        }
    }
    // A Note or Sr. found in an arbitrary current area is NOT evidence that
    // this area owns Ooccoo. Unbound randomized receipts come from ItemService.
    return facts;
}

bool block_ooccoo_dungeon_actor(void* args) {
    if (sActiveAdapter == nullptr || !sActiveAdapter->ooccoo_sync_active()) return false;
    auto scene = current_ooccoo_scene();
    // Player initialization can run before roomControl's stay room catches up.
    // Both methods have arguments (this, position, room, mode/parameters).
    scene.room = mods::arg<int>(args, 2);
    return ooccoo::scene_dungeon(scene, false) < 0;
}

HookAction ooccoo_reunion_pre(ModContext*, void* args, void* result, void*) {
    if (block_ooccoo_dungeon_actor(args)) {
        // Old saves may already have an erroneous cave OOCCOO_NOTE bit.
        // Returning false also prevents the caller entering a reunion demo.
        *static_cast<bool*>(result) = false;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

HookAction ooccoo_warp_actor_pre(ModContext*, void* args, void*, void*) {
    // Covers procDungeonWarpSceneStartInit as well as checkSetNpcTks. This is
    // Link's dungeon helper only: story NPC_TKS actors remain completely native.
    return block_ooccoo_dungeon_actor(args) ? HOOK_SKIP_ORIGINAL : HOOK_CONTINUE;
}

void ooccoo_warp_out_post(ModContext*, void*, void*, void*) {
    if (sActiveAdapter != nullptr) sActiveAdapter->notify_local_ooccoo_warp_out();
}

int bottle_slot_count() {
    int count = 0;
    for (int slot = SLOT_11; slot <= SLOT_14; ++slot) {
        if (dComIfGs_getItem(slot, true) != dItemNo_NONE_e) {
            ++count;
        }
    }
    return count;
}

bool is_vanilla_bottle_source(int item) {
    switch (item) {
    case dItemNo_EMPTY_BOTTLE_e:
    case dItemNo_HALF_MILK_BOTTLE_e:
    case dItemNo_OIL_BOTTLE_e:
    case dItemNo_FAIRY_DROP_e:
        return true;
    default:
        return false;
    }
}

std::set<uint8_t> parse_bottle_sources(const nlohmann::json& value) {
    std::set<uint8_t> sources;
    if (!value.is_array()) return sources;
    for (const auto& raw : value) {
        if (!raw.is_number_integer()) continue;
        const int source = raw.get<int>();
        if (is_vanilla_bottle_source(source)) {
            sources.insert(static_cast<uint8_t>(source));
        }
    }
    return sources;
}

std::set<uint8_t> bottle_sources_from_vanilla_save() {
    std::set<uint8_t> sources;
    if (dComIfGs_isItemFirstBit(dItemNo_EMPTY_BOTTLE_e)) {
        sources.insert(dItemNo_EMPTY_BOTTLE_e);
    }
    if (dComIfGs_isItemFirstBit(dItemNo_HALF_MILK_BOTTLE_e)) {
        sources.insert(dItemNo_HALF_MILK_BOTTLE_e);
    }
    // These two rewards create bottles containing oil/tears, but their
    // permanent acquisition bits belong to the distinct bottle-grant items.
    if (dComIfGs_isItemFirstBit(dItemNo_OIL_BOTTLE3_e)) {
        sources.insert(dItemNo_OIL_BOTTLE_e);
    }
    if (dComIfGs_isItemFirstBit(dItemNo_DROP_BOTTLE_e)) {
        sources.insert(dItemNo_FAIRY_DROP_e);
    }
    return sources;
}

void remember_vanilla_bottle_source(int source) {
    switch (source) {
    case dItemNo_EMPTY_BOTTLE_e:
    case dItemNo_HALF_MILK_BOTTLE_e:
        dComIfGs_onItemFirstBit(static_cast<uint8_t>(source));
        break;
    case dItemNo_OIL_BOTTLE_e:
        dComIfGs_onItemFirstBit(dItemNo_OIL_BOTTLE3_e);
        break;
    case dItemNo_FAIRY_DROP_e:
        dComIfGs_onItemFirstBit(dItemNo_DROP_BOTTLE_e);
        break;
    default:
        break;
    }
}

bool opening_or_title_active() {
    // The opening scene initializes temporary save data for the title-screen
    // Link before the opening process itself becomes searchable. Those
    // equipment setters are presentation-only and must not be published as
    // persistent inventory. Use the raw start layer to avoid resolving the
    // computed layer through randomizer state during process creation.
    const char* stage = dComIfGp_getStartStageName();
    return (stage != nullptr &&
            (std::strcmp(stage, "S_MV000") == 0 ||
             (std::strcmp(stage, "F_SP102") == 0 &&
              dComIfGp_getStartStageLayer() == 10))) ||
           fpcM_SearchByName(fpcNm_OPENING_SCENE_e) != nullptr ||
           fpcM_SearchByName(fpcNm_TITLE_e) != nullptr;
}

bool remote_link_gameplay_ready(bool manualTransitionActive) {
    if (manualTransitionActive || fpcM_SearchByName(fpcNm_TITLE_e) != nullptr ||
        fpcM_SearchByName(fpcNm_PLAY_SCENE_e) == nullptr ||
        dComIfGp_getWindowNum() == 0 || dComIfGp_getStageStagInfo() == nullptr ||
        local_transition_hides_remote_link()) {
        return false;
    }
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    return player != nullptr && fopAcM_GetName(player) == fpcNm_ALINK_e;
}

bool syncable_bomb_item(int itemId) {
    return itemId == dItemNo_NORMAL_BOMB_e || itemId == dItemNo_WATER_BOMB_e ||
           itemId == dItemNo_POKE_BOMB_e;
}

nlohmann::json bomb_bag_slots_snapshot() {
    nlohmann::json slots = nlohmann::json::array();
    const u8 rental = dMeter2Info_getRentalBombBag();
    for (int bag = 0; bag < 3; ++bag) {
        if (rental != 0xFF && rental == static_cast<u8>(bag)) {
            continue;
        }
        const int item = dComIfGs_getItem(SLOT_15 + bag, false);
        const int count = dComIfGs_getBombNum(static_cast<u8>(bag));
        if (syncable_bomb_item(item) && count > 0) {
            slots.push_back({{"bag", bag}, {"item", item}, {"count", count}});
        }
    }
    return slots;
}

bool apply_bomb_bag_slot(int bag, int item, int count) {
    if (bag < 0 || bag >= 3 || !syncable_bomb_item(item) || count <= 0 || count > 99) {
        return false;
    }
    const u8 rental = dMeter2Info_getRentalBombBag();
    if (rental != 0xFF && rental == static_cast<u8>(bag)) {
        return false;
    }
    const int slot = SLOT_15 + bag;
    dComIfGs_setItem(slot, static_cast<u8>(item));
    dComIfGp_setItem(static_cast<u8>(slot), static_cast<u8>(item));
    dComIfGs_setBombNum(static_cast<u8>(bag), static_cast<u8>(count));
    dComIfGs_setLineUpItem();
    return true;
}

bool is_synced_key_item(int itemId) {
    switch (itemId) {
    case dItemNo_HOOKSHOT_e:
    case dItemNo_W_HOOKSHOT_e:
    case dItemNo_BOOMERANG_e:
    case dItemNo_SPINNER_e:
    case dItemNo_COPY_ROD_e:
    case dItemNo_COPY_ROD_2_e:
    case dItemNo_BOW_e:
    case dItemNo_IRONBALL_e:
    case dItemNo_HAWK_EYE_e:
    case dItemNo_HVY_BOOTS_e:
    case dItemNo_ARMOR_e:
    case dItemNo_PACHINKO_e:
    case dItemNo_KANTERA_e:
    case dItemNo_KANTERA2_e:
    case dItemNo_FISHING_ROD_1_e:
    case dItemNo_LURE_ROD_e:
    case dItemNo_WOOD_STICK_e:
    case dItemNo_SWORD_e:
    case dItemNo_MASTER_SWORD_e:
    case dItemNo_LIGHT_SWORD_e:
    case dItemNo_WOOD_SHIELD_e:
    case dItemNo_SHIELD_e:
    case dItemNo_HYLIA_SHIELD_e:
    case dItemNo_WEAR_CASUAL_e:
    case dItemNo_WEAR_KOKIRI_e:
    case dItemNo_WEAR_ZORA_e:
    case dItemNo_EMPTY_BOTTLE_e:
    case dItemNo_RED_BOTTLE_e:
    case dItemNo_GREEN_BOTTLE_e:
    case dItemNo_BLUE_BOTTLE_e:
    case dItemNo_MILK_BOTTLE_e:
    case dItemNo_HALF_MILK_BOTTLE_e:
    case dItemNo_OIL_BOTTLE_e:
    case dItemNo_WATER_BOTTLE_e:
    case dItemNo_OIL_BOTTLE_2_e:
    case dItemNo_RED_BOTTLE_2_e:
    case dItemNo_OIL_BOTTLE3_e:
    case dItemNo_WALLET_LV1_e:
    case dItemNo_WALLET_LV2_e:
    case dItemNo_WALLET_LV3_e:
    case dItemNo_BOMB_BAG_LV1_e:
    case dItemNo_BOMB_BAG_LV2_e:
    case dItemNo_MAGIC_LV1_e:
    case dItemNo_ARROW_LV1_e:
    case dItemNo_ARROW_LV2_e:
    case dItemNo_ARROW_LV3_e:
    case dItemNo_RAFRELS_MEMO_e:
    case dItemNo_ASHS_SCRIBBLING_e:
    case dItemNo_LETTER_e:
    case dItemNo_BILL_e:
    case dItemNo_WOOD_STATUE_e:
    case dItemNo_IRIAS_PENDANT_e:
    case dItemNo_HORSE_FLUTE_e:
    case dItemNo_ZORAS_JEWEL_e:
    case dItemNo_ANCIENT_DOCUMENT_e:
    case dItemNo_ANCIENT_DOCUMENT2_e:
    case dItemNo_AIR_LETTER_e:
    case dItemNo_TOMATO_PUREE_e:
    case dItemNo_TASTE_e:
    case dItemNo_SURFBOARD_e:
        return true;
    default:
        return false;
    }
}

static_assert(dItemNo_LETTER_e == trade_item::kFirst);
static_assert(dItemNo_BILL_e == trade_item::kFirst + 1);
static_assert(dItemNo_WOOD_STATUE_e == trade_item::kFirst + 2);
static_assert(dItemNo_IRIAS_PENDANT_e == trade_item::kFirst + 3);
static_assert(dItemNo_HORSE_FLUTE_e == trade_item::kLast);

int observed_trade_phase() {
    const int slot = dComIfGs_getItem(SLOT_21, false);
    int phase = trade_item::item_phase(slot);
    for (int item = trade_item::kFirst; item <= trade_item::kLast; ++item) {
        if (!dComIfGs_isItemFirstBit(static_cast<u8>(item))) continue;
        int acquired = trade_item::item_phase(item);
        if (slot == trade_item::kNone) ++acquired;
        phase = std::max(phase, acquired);
    }
    // These durable story milestones also identify hand-ins when the game
    // changes the shared slot without setting a new item-first bit.
    if (dComIfGs_isEventBit(0x2180)) phase = std::max(phase, 1); // Letter -> Telma
    if (dComIfGs_isEventBit(0x2102)) phase = std::max(phase, 3); // Past the doctor
    if (dComIfGs_isEventBit(0x2204)) phase = std::max(phase, 4); // Wooden Statue
    if (dComIfGs_isEventBit(0x2340)) phase = std::max(phase, 5); // Statue -> Ilia
    if (dComIfGs_isEventBit(0x2280)) phase = std::max(phase, 6); // Ilia's Charm
    if (dComIfGs_isEventBit(0x2320)) phase = std::max(phase, 7); // Memory restored
    return phase;
}

bool apply_trade_state(int phase, int item) {
    if (!trade_item::valid_state(phase, item)) return false;
    if (trade_item::should_apply(observed_trade_phase(), phase) &&
        dComIfGs_getItem(SLOT_21, false) != item) {
        dComIfGs_setWarashibeItem(static_cast<u8>(item));
    }
    return true;
}

void record_older_trade_item(int item) {
    dComIfGs_onItemFirstBit(static_cast<u8>(item));
    // This item getter also marks the field encounter, unlike the other four
    // trade-item getters. Preserve that side effect without restoring the item.
    if (item == dItemNo_WOOD_STATUE_e) dComIfGs_onEventBit(0x2204);
}

bool is_synced_item_first_bit(int itemId) {
    return itemId == dItemNo_KAKERA_HEART_e || itemId == dItemNo_UTAWA_HEART_e ||
           itemId == dItemNo_LINKS_SAVINGS_e ||
           (itemId >= dItemNo_M_BEETLE_e && itemId <= dItemNo_F_MAYFLY_e);
}

void repair_lantern_item_state() {
    if (!dComIfGs_isItemFirstBit(dItemNo_KANTERA_e)) return;
    if (dComIfGs_getItem(SLOT_1, true) != dItemNo_KANTERA_e)
        dComIfGs_setItem(SLOT_1, dItemNo_KANTERA_e);
    if (dComIfGs_getMaxOil() == 0) dComIfGs_setMaxOil(21600);
    if (dComIfGs_getOil() == 0) dComIfGs_setOil(dComIfGs_getMaxOil());
}

int malo_fundraising_phase() {
    if (dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[124])) {
        return 2;
    }
    return dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[376]) ? 1 : 0;
}

uint8_t raw_collect_smell() {
    return g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusA().getSelectEquip(COLLECT_SMELL);
}

bool valid_collect_smell(int smell) {
    return smell == dItemNo_NONE_e ||
           (smell >= dItemNo_SMELL_YELIA_POUCH_e && smell <= dItemNo_SMELL_MEDICINE_e);
}

void enqueue_unique_deferred_mutation(std::deque<nlohmann::json>& queue,
                                      nlohmann::json message) {
    const std::string type = message.value("type", std::string());
    const int stage = message.value("stage", -1);
    const int flag = message.value("flag", -1);
    const bool set = message.value("set", true);
    const auto duplicate = std::find_if(queue.begin(), queue.end(), [&](const auto& queued) {
        return queued.value("type", std::string()) == type &&
               queued.value("stage", -1) == stage && queued.value("flag", -1) == flag &&
               queued.value("set", true) == set;
    });
    if (duplicate != queue.end()) return;
    queue.push_back(std::move(message));
}

int collect_smell_priority(int smell) {
    switch (smell) {
    case dItemNo_SMELL_PUMPKIN_e: return 0;
    case dItemNo_SMELL_CHILDREN_e: return 1;
    case dItemNo_SMELL_YELIA_POUCH_e: return 2;
    case dItemNo_SMELL_POH_e: return 3;
    case dItemNo_SMELL_FISH_e: return 4;
    case dItemNo_SMELL_MEDICINE_e: return 5;
    default: return -1;
    }
}

std::string sequence_key(const RoutedMessage& message, std::string_view type) {
    std::string result = message.peerId;
    result.push_back(':');
    result.append(type);
    return result;
}

int stage_for_bits(dSv_memBit_c* bits) {
    if (bits == nullptr) return -1;
    if (bits == &g_dComIfG_gameInfo.info.getMemory().getBit()) return current_stage_table();
    for (int stage = 0; stage < dSv_save_c::STAGE_MAX; ++stage) {
        if (bits == &g_dComIfG_gameInfo.info.getSavedata().getSave(stage).getBit()) return stage;
    }
    return -1;
}

void event_bit_on_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    if (mods::arg<dSv_event_c*>(args, 0) != &g_dComIfG_gameInfo.info.getEvent()) return;
    const uint16_t flag = mods::arg<uint16_t>(args, 1);
    sActiveAdapter->notify_local_event_bit(flag);
    if (!is_unsynced_event_bit(flag)) {
        nlohmann::json message = {{"type", "event_bit"}, {"flag", flag}, {"set", true}};
        const char* stage = dComIfGp_getStartStageName();
        if (flag == kTitleSyntheticEponaRescuedEventBit && stage != nullptr &&
            std::string_view(stage) == kTitleDemoStage) {
            return;
        }
        if (flag == 0x0880 && stage != nullptr && std::string_view(stage) == "F_SP113") {
            message["zora_thaw_destination"] = {
                {"stage", stage}, {"room", dComIfGp_getStartStageRoomNo()},
                {"layer", dComIfGp_getStartStageLayer()},
                {"start_point", dComIfGp_getStartStagePoint()},
            };
        }
        sActiveAdapter->publish_local(std::move(message));
    }
}

void event_bit_off_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    if (mods::arg<dSv_event_c*>(args, 0) != &g_dComIfG_gameInfo.info.getEvent()) return;
    const uint16_t flag = mods::arg<uint16_t>(args, 1);
    if (!is_unsynced_event_bit(flag))
        sActiveAdapter->publish_local({{"type", "event_bit"}, {"flag", flag}, {"set", false}});
}

void memory_tbox_on_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    if (bits != &g_dComIfG_gameInfo.info.getMemory().getBit()) return;
    const int stage = current_stage_table();
    const int flag = mods::arg<int>(args, 1);
    if (valid_stage(stage) && flag >= 0 && flag < 64)
        sActiveAdapter->publish_local({{"type", "tbox_bit"}, {"stage", stage}, {"flag", flag}});
}

void memory_item_on_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    if (bits != &g_dComIfG_gameInfo.info.getMemory().getBit()) return;
    const int stage = current_stage_table();
    const int flag = mods::arg<int>(args, 1);
    sActiveAdapter->observe_local_memory_item(stage, flag);
}

void fishing_add_count_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        mods::arg<dSv_fishing_info_c*>(args, 0) !=
            &g_dComIfG_gameInfo.info.getPlayer().getFishingInfo()) return;
    sActiveAdapter->notify_local_fish_caught(mods::arg<uint8_t>(args, 1));
}

HookAction light_drop_num_set_pre(ModContext*, void* args, void*, void*) {
    auto* drops = mods::arg<dSv_light_drop_c*>(args, 0);
    const int area = mods::arg<u8>(args, 1);
    const bool active = drops == &g_dComIfG_gameInfo.info.getPlayer().getLightDrop() &&
                        area >= 0 && area < 4;
    sLightDropPreviousCounts.push_back(active ? drops->getLightDropNum(static_cast<u8>(area)) : -1);
    return HOOK_CONTINUE;
}

void light_drop_num_set_post(ModContext*, void* args, void*, void*) {
    int previous = -1;
    if (!sLightDropPreviousCounts.empty()) {
        previous = sLightDropPreviousCounts.back();
        sLightDropPreviousCounts.pop_back();
    }
    if (previous < 0 || sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    const int area = mods::arg<u8>(args, 1);
    sActiveAdapter->notify_local_light_drop_num(area, previous, mods::arg<u8>(args, 2));
}

void light_drop_flag_on_post(ModContext*, void* args, void*, void*) {
    auto* drops = mods::arg<dSv_light_drop_c*>(args, 0);
    const int area = mods::arg<u8>(args, 1);
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        drops != &g_dComIfG_gameInfo.info.getPlayer().getLightDrop() || area < 0 || area >= 3) {
        return;
    }
    // The final wrapper notifies on every valid call, not only false->true.
    sActiveAdapter->publish_local({{"type", "light_drop_get_flag"}, {"area", area}});
}

HookAction empty_bottle_set_pre(ModContext*, void* args, void*, void*) {
    const auto* items = mods::arg<dSv_player_item_c*>(args, 0);
    const bool localItems = items == &g_dComIfG_gameInfo.info.getPlayer().getItem();
    const uint8_t source = dItemNo_EMPTY_BOTTLE_e;
    const int previous = localItems ? bottle_slot_count() : -1;
    sEmptyBottleMutations.push_back({previous, source});
    if (previous >= 0 && sActiveAdapter != nullptr &&
        sActiveAdapter->should_suppress_local_bottle_source(source)) {
        sEmptyBottleMutations.back().previous = -1;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void empty_bottle_set_post(ModContext*, void*, void*, void*) {
    PendingBottleMutation mutation;
    if (!sEmptyBottleMutations.empty()) {
        mutation = sEmptyBottleMutations.back();
        sEmptyBottleMutations.pop_back();
    }
    if (mutation.previous >= 0 && sActiveAdapter != nullptr &&
        !sActiveAdapter->applying_remote()) {
        sActiveAdapter->notify_local_bottle_slots(
            mutation.previous, bottle_slot_count(), mutation.sourceItem);
    }
}

HookAction empty_bottle_item_set_pre(ModContext*, void* args, void*, void*) {
    const auto* items = mods::arg<dSv_player_item_c*>(args, 0);
    const bool localItems = items == &g_dComIfG_gameInfo.info.getPlayer().getItem();
    const uint8_t source = mods::arg<u8>(args, 1);
    const int previous = localItems ? bottle_slot_count() : -1;
    sEmptyBottleItemMutations.push_back({previous, source});
    if (previous >= 0 && sActiveAdapter != nullptr &&
        sActiveAdapter->should_suppress_local_bottle_source(source)) {
        sEmptyBottleItemMutations.back().previous = -1;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void empty_bottle_item_set_post(ModContext*, void*, void*, void*) {
    PendingBottleMutation mutation;
    if (!sEmptyBottleItemMutations.empty()) {
        mutation = sEmptyBottleItemMutations.back();
        sEmptyBottleItemMutations.pop_back();
    }
    if (mutation.previous >= 0 && sActiveAdapter != nullptr &&
        !sActiveAdapter->applying_remote()) {
        sActiveAdapter->notify_local_bottle_slots(
            mutation.previous, bottle_slot_count(), mutation.sourceItem);
    }
}

void event_reg_set_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        mods::arg<dSv_event_c*>(args, 0) != &g_dComIfG_gameInfo.info.getEvent()) return;
    const int reg = mods::arg<u16>(args, 1);
    if (reg == 0xFAFF) {
        const int value = dMsgObject_getFundRaising();
        if (value < 0 || value > kMaxSyncedDonationTotal) return;
        sActiveAdapter->publish_local({
            {"type", "malo_fundraising"}, {"phase", malo_fundraising_phase()},
            {"value", value},
        });
    } else if (reg == 0xF8FF) {
        const int value = dMsgObject_getOffering();
        if (value < 0 || value > kMaxSyncedDonationTotal) return;
        sActiveAdapter->publish_local(
            {{"type", "charlo_offering"}, {"value", value}});
    }
}

void smell_type_set_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        mods::arg<dMsgObject_c*>(args, 0) != dMsgObject_getMsgObjectClass()) return;
    const int smell = raw_collect_smell();
    if (valid_collect_smell(smell)) {
        sActiveAdapter->publish_local({{"type", "collect_smell"}, {"value", smell}});
    }
}

void dark_clear_set_post(ModContext*, void* args, void*, void*) {
    const int level = mods::arg<int>(args, 1);
    if (sActiveAdapter != nullptr && level >= 0 && level <= 3 &&
        mods::arg<dSv_player_status_b_c*>(args, 0) ==
            &g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusB()) {
        sActiveAdapter->notify_local_dark_clear(level);
    }
}

void transform_set_post(ModContext*, void* args, void*, void*) {
    const int level = mods::arg<int>(args, 1);
    if (sActiveAdapter != nullptr && !sActiveAdapter->applying_remote() && level == 3 &&
        dComIfGs_isEventBit(dSv_event_flag_c::M_071) &&
        mods::arg<dSv_player_status_b_c*>(args, 0) ==
            &g_dComIfG_gameInfo.info.getPlayer().getPlayerStatusB()) {
        sActiveAdapter->publish_local({{"type", "transform_lv"}, {"no", level}});
    }
}

void letter_get_set_post(ModContext*, void* args, void*, void*) {
    const int letter = mods::arg<int>(args, 1);
    if (sActiveAdapter != nullptr && !sActiveAdapter->applying_remote() &&
        letter >= 0 && letter < LETTER_INFO_BIT &&
        mods::arg<dSv_letter_info_c*>(args, 0) ==
            &g_dComIfG_gameInfo.info.getPlayer().getLetterInfo()) {
        sActiveAdapter->publish_local({{"type", "letter_get"}, {"no", letter}});
    }
}

HookAction stage_key_num_set_pre(ModContext*, void* args, void*, void*) {
    const int stage = mods::arg<int>(args, 0);
    sStageKeyPreviousCounts.push_back(
        valid_stage(stage) ? stage_bits(stage).getKeyNum() : -1);
    return HOOK_CONTINUE;
}

void stage_key_num_set_post(ModContext*, void* args, void*, void*) {
    int previous = -1;
    if (!sStageKeyPreviousCounts.empty()) {
        previous = sStageKeyPreviousCounts.back();
        sStageKeyPreviousCounts.pop_back();
    }
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    const int stage = mods::arg<int>(args, 0);
    const int count = mods::arg<u8>(args, 1);
    if (previous < 0 || stage != current_stage_table() || !valid_stage(stage) ||
        count < 0 || count > 99 ||
        is_local_escort_key_count(stage) ||
        (sActiveAdapter->randomizer_active() && count > previous)) {
        return;
    }
    // Publish only the current-stage branch. Durable off-stage maintenance is
    // save bookkeeping rather than a live key-count mutation.
    sActiveAdapter->publish_local(
        {{"type", "key_num"}, {"stage", stage}, {"count", count}});
}

HookAction meter_move_key_pre(ModContext*, void*, void*, void*) {
    PendingMeterKeyMutation pending;
    pending.publish = sActiveAdapter != nullptr && !sActiveAdapter->applying_remote() &&
                      dComIfGp_getItemKeyNumCount() != 0;
    pending.stage = pending.publish ? current_stage_table() : -1;
    pending.previous = pending.publish ? dComIfGs_getKeyNum() : -1;
    sPendingMeterKeyMutations.push_back(pending);
    return HOOK_CONTINUE;
}

void meter_move_key_post(ModContext*, void*, void*, void*) {
    PendingMeterKeyMutation pending;
    if (!sPendingMeterKeyMutations.empty()) {
        pending = sPendingMeterKeyMutations.back();
        sPendingMeterKeyMutations.pop_back();
    }
    if (!pending.publish || is_local_escort_key_count(pending.stage) ||
        sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        pending.stage != current_stage_table() || !valid_stage(pending.stage)) return;
    const int count = dComIfGs_getKeyNum();
    if (sActiveAdapter->randomizer_active() && count > pending.previous) return;
    sActiveAdapter->publish_local({
        {"type", "key_num"}, {"stage", pending.stage}, {"count", count},
    });
}

HookAction meter_move_life_pre(ModContext*, void*, void*, void*) {
    sPendingMeterLifeMutations.push_back({
        sActiveAdapter != nullptr && !sActiveAdapter->applying_remote() &&
            dComIfGp_getItemMaxLifeCount() != 0,
        dComIfGs_getMaxLife(),
    });
    return HOOK_CONTINUE;
}

void meter_move_life_post(ModContext*, void*, void*, void*) {
    PendingMeterScalarMutation pending;
    if (!sPendingMeterLifeMutations.empty()) {
        pending = sPendingMeterLifeMutations.back();
        sPendingMeterLifeMutations.pop_back();
    }
    if (pending.publish && sActiveAdapter != nullptr && !sActiveAdapter->applying_remote()) {
        sActiveAdapter->notify_local_max_life(pending.previous, dComIfGs_getMaxLife());
    }
}

HookAction meter_move_rupee_pre(ModContext*, void*, void*, void*) {
    sPendingMeterRupeeMutations.push_back({
        sActiveAdapter != nullptr && !sActiveAdapter->applying_remote() &&
            dComIfGp_getItemRupeeCount() != 0,
        dComIfGs_getRupee(),
    });
    return HOOK_CONTINUE;
}

void meter_move_rupee_post(ModContext*, void*, void*, void*) {
    PendingMeterScalarMutation pending;
    if (!sPendingMeterRupeeMutations.empty()) {
        pending = sPendingMeterRupeeMutations.back();
        sPendingMeterRupeeMutations.pop_back();
    }
    if (pending.publish && sActiveAdapter != nullptr && !sActiveAdapter->applying_remote()) {
        sActiveAdapter->notify_local_rupees(pending.previous, dComIfGs_getRupee());
    }
}

HookAction memory_switch_on_pre(ModContext*, void* args, void*, void*) {
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    const int flag = mods::arg<int>(args, 1);
    const bool valid = bits != nullptr && flag >= 0 && flag < dSv_info_c::MEMORY_SWITCH;
    sMemorySwitchWasSetStack.push_back(valid && bits->isSwitch(flag));
    return HOOK_CONTINUE;
}

void memory_switch_on_post(ModContext*, void* args, void*, void*) {
    bool wasSet = true;
    if (!sMemorySwitchWasSetStack.empty()) {
        wasSet = sMemorySwitchWasSetStack.back();
        sMemorySwitchWasSetStack.pop_back();
    }
    if (wasSet || sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    // The engine often mirrors this mutation into the durable stage table;
    // observing both low-level calls would publish the same switch twice.
    if (bits != &g_dComIfG_gameInfo.info.getMemory().getBit()) {
        // Ilia's Wooden Statue scene writes these field switches directly to
        // the saved stage table while the player is inside Kakariko. They do
        // not pass through the current-stage hook below.
        const int flag = mods::arg<int>(args, 1);
        if (bits == &g_dComIfG_gameInfo.info.getSavedata()
                         .getSave(dStage_SaveTbl_FIELD).getBit() &&
            (flag == 0x40 || flag == 0x1C) &&
            !sActiveAdapter->randomizer_active()) {
            sActiveAdapter->publish_local({{"type", "switch_bit"},
                {"stage", dStage_SaveTbl_FIELD}, {"flag", flag}, {"set", true}});
        }
        return;
    }
    const int stage = current_stage_table();
    const int flag = mods::arg<int>(args, 1);
    if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::MEMORY_SWITCH ||
        is_unsynced_switch_bit(stage, flag)) return;
    if (is_local_lakebed_staircase_switch_write(stage, flag)) return;
    nlohmann::json message =
        {{"type", "switch_bit"}, {"stage", stage}, {"flag", flag}, {"set", true}};
    if (void* process = exact_local_switch_actor_context(true); process != nullptr) {
        const int actor = fpcM_GetName(process);
        // Live action remains exact-room, but an actor's own durable stage
        // switches must reach peers while the room is unloaded. That lets the
        // actor consume already-synchronized save state during native create.
        const bool remoteBlockAction =
            (actor == fpcNm_Obj_Movebox_e && remote_movebox_action_active(process)) ||
            (actor == fpcNm_Obj_IceBlock_e && remote_iceblock_action_active(process));
        if ((is_room_action_actor(actor) && !room_actor_persistent_switch(process, flag)) ||
            remoteBlockAction ||
            is_floor_switch_momentary_output(actor, fpcM_GetParam(process), flag) ||
            is_remote_floor_switch_execution(process)) return;
        const auto* source = static_cast<const fopAc_ac_c*>(process);
        const int room = fopAcM_GetHomeRoomNo(source);
        const uint32_t params = fpcM_GetParam(process);
        const char* stageName = dComIfGp_getStartStageName();
        const int angleX = static_cast<uint16_t>(source->shape_angle.x);
        const bool caveMap = is_cave_map_reveal(actor == fpcNm_SWC00_e,
            stageName != nullptr ? stageName : "", stage, room, flag, params,
            angleX, wasSet, bits->isSwitch(flag));
        const bool sacredBlock = is_sacred_grove_block_switch(
            stageName != nullptr ? stageName : "", stage, room, flag, actor,
            params, angleX, bits->isSwitch(flag));
        if (is_group2_lifecycle_actor(actor) &&
            !is_sewers_progression_switch(stage, flag) &&
            !is_eldin_gorge_bridge_completion(stage, flag, actor, room, params) &&
            !caveMap && !sacredBlock) return;
        message.update({{"source_actor", actor}, {"source_room", room},
                        {"source_params", params}});
        if (caveMap || sacredBlock) {
            message.update({{"source_stage", stageName}, {"source_angle_x", angleX}});
        }
    }
    sActiveAdapter->publish_local(std::move(message));
}

HookAction memory_switch_off_pre(ModContext*, void* args, void*, void*) {
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    const int flag = mods::arg<int>(args, 1);
    const bool valid = bits != nullptr && flag >= 0 && flag < dSv_info_c::MEMORY_SWITCH;
    sMemorySwitchWasSetOffStack.push_back(valid && bits->isSwitch(flag));
    return HOOK_CONTINUE;
}

void memory_switch_off_post(ModContext*, void* args, void*, void*) {
    bool wasSet = false;
    if (!sMemorySwitchWasSetOffStack.empty()) {
        wasSet = sMemorySwitchWasSetOffStack.back();
        sMemorySwitchWasSetOffStack.pop_back();
    }
    if (!wasSet || sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    if (bits != &g_dComIfG_gameInfo.info.getMemory().getBit()) return;
    const int stage = current_stage_table();
    const int flag = mods::arg<int>(args, 1);
    if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::MEMORY_SWITCH ||
        is_unsynced_switch_bit(stage, flag)) return;
    if (is_local_lakebed_staircase_switch_write(stage, flag)) return;
    nlohmann::json message =
        {{"type", "switch_bit"}, {"stage", stage}, {"flag", flag}, {"set", false}};
    if (void* process = exact_local_switch_actor_context(false); process != nullptr) {
        const int actor = fpcM_GetName(process);
        const bool remoteBlockAction =
            (actor == fpcNm_Obj_Movebox_e && remote_movebox_action_active(process)) ||
            (actor == fpcNm_Obj_IceBlock_e && remote_iceblock_action_active(process));
        if ((is_room_action_actor(actor) && !room_actor_persistent_switch(process, flag)) ||
            remoteBlockAction ||
            is_floor_switch_momentary_output(actor, fpcM_GetParam(process), flag) ||
            is_remote_floor_switch_execution(process)) return;
        const int room = fopAcM_GetHomeRoomNo(static_cast<const fopAc_ac_c*>(process));
        const uint32_t params = fpcM_GetParam(process);
        if (is_group2_lifecycle_actor(actor) && !is_sewers_progression_switch(stage, flag)) return;
        message.update({{"source_actor", actor}, {"source_room", room},
                        {"source_params", params}});
    }
    sActiveAdapter->publish_local(std::move(message));
}

void memory_dungeon_item_on_post(ModContext*, void* args, void*, void*) {
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    auto* bits = mods::arg<dSv_memBit_c*>(args, 0);
    if (bits != &g_dComIfG_gameInfo.info.getMemory().getBit()) return;
    const int stage = current_stage_table();
    const int kind = mods::arg<int>(args, 1);
    // Ooccoo has a dedicated receipt protocol. The raw bit can be set by
    // boss clear or by an NPC before the actual item give has completed.
    if (kind == dSv_memBit_c::OOCCOO_NOTE) return;
    if (valid_stage(stage) && kind >= 0 && kind <= 7) {
        sActiveAdapter->publish_local(
            {{"type", "dungeon_item_bit"}, {"stage", stage}, {"kind", kind}});
    }
}

HookAction visited_room_on_pre(ModContext*, void*, void*, void*) {
    int region = -1;
    if (sActiveAdapter != nullptr && !sActiveAdapter->applying_remote()) {
        const u8 candidate = dComIfG_getNowCalcRegion();
        if (candidate < 8 && !dComIfGs_isRegionBit(candidate)) region = candidate;
    }
    sVisitedRoomPendingRegions.push_back(region);
    return HOOK_CONTINUE;
}

void visited_room_on_post(ModContext*, void* args, void*, void*) {
    const int pendingRegion = sVisitedRoomPendingRegions.empty()
                                  ? -1
                                  : sVisitedRoomPendingRegions.back();
    if (!sVisitedRoomPendingRegions.empty()) sVisitedRoomPendingRegions.pop_back();
    if (sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    // The vanilla wrapper is the sole gameplay caller of onRegionBit and only
    // invokes it for a new region. The optimized setter is too small for a
    // portable trampoline, so publish from this stable wrapper after verifying
    // that its mutation completed.
    if (pendingRegion >= 0 && dComIfGs_isRegionBit(pendingRegion)) {
        sActiveAdapter->publish_local({{"type", "region_bit"}, {"region", pendingRegion}});
    }
    const int room = mods::arg<int>(args, 0);
    if (room < 0 || room >= 64) return;
    const dStage_FileList2_dt_c* target = dStage_roomControl_c::getFileList2(room);
    const int stayRoom = dComIfGp_roomControl_getStayNo();
    if (target == nullptr || target->field_0x13 >= 0x40 || stayRoom < 0 || stayRoom >= 64)
        return;
    const dStage_FileList2_dt_c* current = dStage_roomControl_c::getFileList2(stayRoom);
    if (current == nullptr) return;
    const int stage = current->field_0x13;
    if (stage >= 0 && stage < dSv_save_c::STAGE2_MAX && room >= 0 && room < 64) {
        sActiveAdapter->publish_local(
            {{"type", "visited_room"}, {"stage", stage}, {"room", room}});
    }
}

HookAction player_item_first_on_pre(ModContext*, void* args, void*, void*) {
    auto* items = mods::arg<dSv_player_get_item_c*>(args, 0);
    const uint8_t item = mods::arg<uint8_t>(args, 1);
    const bool active = items == &g_dComIfG_gameInfo.info.getPlayer().getGetItem();
    // Treat an inactive save/editor object as already owned so its post-hook
    // cannot escape through the active-player notification lane.
    sItemFirstWasOwnedStack.push_back(!active || items->isFirstBit(item));
    return HOOK_CONTINUE;
}

HookAction poe_pickup_pre(ModContext*, void*, void*, void*) {
    sPoePickupPrevious.push_back(dComIfGs_getPohSpiritNum());
    return HOOK_CONTINUE;
}

void poe_pickup_post(ModContext*, void*, void*, void*) {
    if (sPoePickupPrevious.empty()) return;
    const int previous = sPoePickupPrevious.back();
    sPoePickupPrevious.pop_back();
    if (sActiveAdapter != nullptr)
        sActiveAdapter->notify_local_poe_pickup(previous, dComIfGs_getPohSpiritNum());
}

void player_item_first_on_post(ModContext*, void* args, void*, void*) {
    const bool wasOwned = !sItemFirstWasOwnedStack.empty() &&
                          sItemFirstWasOwnedStack.back();
    if (!sItemFirstWasOwnedStack.empty()) sItemFirstWasOwnedStack.pop_back();
    if (wasOwned || sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        sActiveAdapter->randomizer_active()) return;
    const int item = mods::arg<uint8_t>(args, 1);
    if (is_synced_key_item(item)) {
        sActiveAdapter->publish_local({{"type", "item_get"}, {"item_id", item}});
    }
    if (is_synced_item_first_bit(item)) {
        sActiveAdapter->publish_local(
            {{"type", "item_first_bit"}, {"item_id", item}, {"owned", true}});
    }
}

HookAction player_item_set_pre(ModContext*, void* args, void*, void*) {
    const auto* items = mods::arg<dSv_player_item_c*>(args, 0);
    const int slot = mods::arg<int>(args, 1);
    sTradeItemPreviousStack.push_back(
        items == &g_dComIfG_gameInfo.info.getPlayer().getItem() && slot == SLOT_21
            ? dComIfGs_getItem(SLOT_21, false) : -1);
    return HOOK_CONTINUE;
}

void player_item_set_post(ModContext*, void*, void*, void*) {
    int previous = -1;
    if (!sTradeItemPreviousStack.empty()) {
        previous = sTradeItemPreviousStack.back();
        sTradeItemPreviousStack.pop_back();
    }
    if (previous < 0 || sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        sActiveAdapter->randomizer_active()) return;
    const int current = dComIfGs_getItem(SLOT_21, false);
    if (current == previous) return;
    const int phase = trade_item::phase_for_change(previous, current);
    if (phase < 0) return;
    sActiveAdapter->publish_local({{"type", "trade_item"}, {"phase", phase},
                                  {"item", trade_item::item_for_phase(phase)}});
}

HookAction player_item_first_off_pre(ModContext*, void* args, void*, void*) {
    auto* items = mods::arg<dSv_player_get_item_c*>(args, 0);
    const uint8_t item = mods::arg<uint8_t>(args, 1);
    const bool active = items == &g_dComIfG_gameInfo.info.getPlayer().getGetItem();
    sItemFirstWasOwnedOffStack.push_back(active && items->isFirstBit(item));
    return HOOK_CONTINUE;
}

void player_item_first_off_post(ModContext*, void* args, void*, void*) {
    const bool wasOwned = !sItemFirstWasOwnedOffStack.empty() &&
                          sItemFirstWasOwnedOffStack.back();
    if (!sItemFirstWasOwnedOffStack.empty()) sItemFirstWasOwnedOffStack.pop_back();
    if (!wasOwned || sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        sActiveAdapter->randomizer_active()) return;
    const int item = mods::arg<uint8_t>(args, 1);
    if (is_synced_item_first_bit(item)) {
        sActiveAdapter->publish_local(
            {{"type", "item_first_bit"}, {"item_id", item}, {"owned", false}});
    }
}

void player_collect_set_post(ModContext*, void* args, void*, void*) {
    const int type = mods::arg<int>(args, 1);
    const int item = mods::arg<uint8_t>(args, 2);
    if (type < 0 || type > 2 || item < 0 || item >= 8 ||
        sActiveAdapter == nullptr || sActiveAdapter->applying_remote() ||
        sActiveAdapter->randomizer_active() ||
        mods::arg<dSv_player_collect_c*>(args, 0) !=
            &g_dComIfG_gameInfo.info.getPlayer().getCollect() ||
        opening_or_title_active()) return;
    sActiveAdapter->publish_local(
        {{"type", "collect"}, {"collect_type", type}, {"item", item}});
}

void player_crystal_set_post(ModContext*, void* args, void*, void*) {
    const int item = mods::arg<uint8_t>(args, 1);
    if (item < 8 && sActiveAdapter != nullptr &&
        !sActiveAdapter->applying_remote() &&
        mods::arg<dSv_player_collect_c*>(args, 0) ==
            &g_dComIfG_gameInfo.info.getPlayer().getCollect()) {
        sActiveAdapter->publish_local({{"type", "collect_crystal"}, {"item", item}});
    }
}

void player_mirror_set_post(ModContext*, void* args, void*, void*) {
    const int item = mods::arg<uint8_t>(args, 1);
    if (item < 8 && sActiveAdapter != nullptr &&
        !sActiveAdapter->applying_remote() &&
        mods::arg<dSv_player_collect_c*>(args, 0) ==
            &g_dComIfG_gameInfo.info.getPlayer().getCollect()) {
        sActiveAdapter->publish_local({{"type", "collect_mirror"}, {"item", item}});
    }
}

HookAction process_execute_pre(ModContext*, void* args, void*, void*) {
    void* process = mods::arg<void*>(args, 0);
    sExecutingProcessStack.push_back(process);
    coop::process_execute_pre(process);
    floor_switch_process_pre(process);
    sWebDeleteTimerStack.push_back(web_delete_timer(process));
    sRoomActorActionStateStack.push_back(room_actor_action_state(process));
    int preservePushPullKeep = -1;
    if (remote_movebox_action_active(process)) {
        const auto* player = static_cast<const daPy_py_c*>(dComIfGp_getPlayer(0));
        preservePushPullKeep = player != nullptr &&
            player->checkNoResetFlg0(daPy_py_c::FLG0_PUSH_PULL_KEEP) != 0;
    }
    sRemoteMoveboxPushPullKeepStack.push_back(preservePushPullKeep);
    if (process != nullptr && fpcM_GetName(process) == fpcNm_DOOR20_e) {
        sDoor20ExecuteModes.erase(process);
    }
    return HOOK_CONTINUE;
}

void mirror_table_set_base_mtx_compat(daObjMirrorTable_c* table) {
    // Mainline declares setBaseMtx but does not export it through the mod SDK.
    // Keep this small method body local so the null-animation guard can return
    // without an unresolved game import.
    mDoMtx_stack_c::transS(table->current.pos);
    mDoMtx_stack_c::ZXYrotM(table->shape_angle);
    table->mpTableModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mpStairModel != nullptr) table->mpStairModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mpPanelModel != nullptr) table->mpPanelModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mpLightModel != nullptr) table->mpLightModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mpMSquareModel != nullptr) table->mpMSquareModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mBgW[0].ChkUsed()) {
        MTXCopy(mDoMtx_stack_c::get(), table->mMtx[0]);
        table->mBgW[0].Move();
    }
    if (table->mBgW[2].ChkUsed()) {
        MTXCopy(mDoMtx_stack_c::get(), table->mMtx[2]);
        table->mBgW[2].Move();
    }
    mDoMtx_stack_c::copy(table->mpTableModel->getAnmMtx(1));
    table->mpMirrorModel->setBaseTRMtx(mDoMtx_stack_c::get());
    if (table->mBgW[0].ChkUsed()) {
        MTXCopy(mDoMtx_stack_c::get(), table->mMtx[1]);
        table->mBgW[1].Move();
    }
    if (table->mpEmitter1 != nullptr) {
        cXyz pos;
        mDoMtx_stack_c::multVecZero(&pos);
        table->mpEmitter1->setGlobalTranslation(pos.x, pos.y, pos.z);
        if (Z2AudioMgr* audioMgr = Z2GetAudioMgr(); audioMgr != nullptr) {
            audioMgr->seStartLevel(
                table->field_0x874 ? Z2SE_OBJ_MIRROR_LIGHT : Z2SE_OBJ_MIRROR_LIGHT_S,
                &pos, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        }
    }
}

HookAction mirror_chain_draw_pre(ModContext*, void* args, void* retval, void*) {
    const auto* chain = mods::arg<daObjMirrorChain_c*>(args, 0);
    if (chain != nullptr && chain->mpPortalModel == nullptr &&
        dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[354])) {
        // Complete-only models are allocated at actor creation. A live flag
        // update can precede room teardown; do not dereference the absent
        // portal or submit partial draw/scissor state in that interval.
        if (retval != nullptr) *static_cast<int*>(retval) = 1;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

HookAction mirror_table_execute_pre(ModContext*, void* args, void* retval, void*) {
    auto* table = mods::arg<daObjMirrorTable_c*>(args, 0);
    if (table == nullptr || table->mpStairBrkAnm != nullptr ||
        !dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[354])) {
        return HOOK_CONTINUE;
    }
    // Avoid executing the missing stair animation when this actor was created
    // without the complete-only stair resources.
    mirror_table_set_base_mtx_compat(table);
    if (retval != nullptr) *static_cast<int*>(retval) = 1;
    return HOOK_SKIP_ORIGINAL;
}

void door20_check_execute_post(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr) return;
    sDoor20ExecuteModes[mods::arg<daDoor20_c*>(args, 0)] = *static_cast<int*>(retval);
}

HookAction door20_stop_open_pre(ModContext*, void*, void*, void*) {
    ++sDoor20StopOpenDepth;
    return HOOK_CONTINUE;
}

void door20_stop_open_post(ModContext*, void*, void*, void*) {
    if (sDoor20StopOpenDepth != 0) --sDoor20StopOpenDepth;
}

void process_execute_post(ModContext*, void* args, void*, void*) {
    void* process = mods::arg<void*>(args, 0);
    coop::process_execute_post(process);
    finish_remote_arbiter_spin_lift(process);
    const int preservePushPullKeep = sRemoteMoveboxPushPullKeepStack.empty()
                                         ? -1
                                         : sRemoteMoveboxPushPullKeepStack.back();
    if (!sRemoteMoveboxPushPullKeepStack.empty()) {
        sRemoteMoveboxPushPullKeepStack.pop_back();
    }
    if (preservePushPullKeep >= 0) {
        auto* player = static_cast<daPy_py_c*>(dComIfGp_getPlayer(0));
        if (player != nullptr) {
            if (preservePushPullKeep != 0) player->onPushPullKeep();
            else player->offPushPullKeep();
        }
        // Erases the remote marker on the exact frame the native walk mode
        // returns to wait; no stale actor pointer survives room teardown.
        (void)remote_movebox_action_active(process);
    }
    const int previousActorState = sRoomActorActionStateStack.empty()
                                       ? -1
                                       : sRoomActorActionStateStack.back();
    if (!sRoomActorActionStateStack.empty()) sRoomActorActionStateStack.pop_back();
    const int actorState = room_actor_action_state(process);
    if (previousActorState == 0 && actorState == 1 && process != nullptr &&
        !sExecutingProcessStack.empty() && sExecutingProcessStack.back() == process) {
        const int actorName = fpcM_GetName(process);
        if (actorName == fpcNm_BkyRock_e) {
            publish_room_actor_action(process, RoomActorAction::DamageStage);
        } else if (actorName == fpcNm_Obj_Lv4DigSand_e) {
            publish_room_actor_action(process, RoomActorAction::DigStart);
        } else if (actorName == fpcNm_Obj_Movebox_e) {
            const int actionArgument = room_actor_action_argument(process);
            if (actionArgument >= 0) {
                publish_room_actor_action(process, RoomActorAction::MoveStep, actionArgument);
            }
        } else if (actorName == fpcNm_Obj_RotStair_e) {
            const int actionArgument = room_actor_action_argument(process);
            if (actionArgument >= 0 && actionArgument < 4) {
                publish_room_actor_action(process, RoomActorAction::RotateTo, actionArgument);
                publish_lakebed_staircase_switch_state(process, actionArgument);
            }
        } else if (actorName == fpcNm_Obj_IceBlock_e) {
            const int actionArgument = room_actor_action_argument(process);
            if (actionArgument >= 4 && actionArgument <= 11) {
                publish_room_actor_action(process, RoomActorAction::Slide, actionArgument);
            }
        }
    }
    finish_remote_iceblock_action(process);
    const int previousWebTimer = sWebDeleteTimerStack.empty()
                                     ? -1
                                     : sWebDeleteTimerStack.back();
    if (!sWebDeleteTimerStack.empty()) sWebDeleteTimerStack.pop_back();
    const int webTimer = web_delete_timer(process);
    if (previousWebTimer == 0 && (webTimer == 1 || webTimer == 41) &&
        sActiveAdapter != nullptr && !sActiveAdapter->applying_remote()) {
        const int actorName = fpcM_GetName(process);
        // WEB1 has no Ball-and-Chain branch; only WEB0 may begin at 41.
        if (is_web_switch_actor(actorName) &&
            (webTimer == 1 || actorName == fpcNm_OBJ_WEB0_e)) {
            const int stage = current_stage_table();
            const int room = fopAcM_GetRoomNo(static_cast<fopAc_ac_c*>(process));
            const char* stageName = dComIfGp_getStartStageName();
            if (valid_stage(stage) && room >= 0 && room < 64 && stageName != nullptr) {
                sActiveAdapter->publish_local({
                    {"type", "web_timer"}, {"stage", stage}, {"room", room},
                    {"source_actor", actorName},
                    {"source_params", fopAcM_GetParam(process)},
                    {"source_stage", stageName}, {"timer", webTimer},
                });
            }
        }
    }
    const bool isDoor20 = process != nullptr && fpcM_GetName(process) == fpcNm_DOOR20_e;
    const auto doorMode = isDoor20 ? sDoor20ExecuteModes.find(process) : sDoor20ExecuteModes.end();
    if (!isDoor20 || (doorMode != sDoor20ExecuteModes.end() && doorMode->second == 2)) {
        repair_remote_key_door_actor(process);
    }
    if (doorMode != sDoor20ExecuteModes.end()) sDoor20ExecuteModes.erase(doorMode);
    if (sActiveAdapter != nullptr && process != nullptr &&
        fpcM_GetName(process) == fpcNm_ROOM_SCENE_e) {
        auto* roomScene = static_cast<room_of_scene_class*>(process);
        const int room = fopScnM_GetParam(roomScene);
        if (roomScene->field_0x1d4 < 0 && roomScene->field_0x1d5 == 0 &&
            !dComIfGp_event_runCheck() && room == dComIfGp_roomControl_getStayNo()) {
            sActiveAdapter->notify_room_scene_initialized(room);
        }
    }
    floor_switch_process_post(process);
    if (!sExecutingProcessStack.empty()) sExecutingProcessStack.pop_back();
}

HookAction info_switch_on_pre(ModContext*, void* args, void*, void*) {
    dSv_info_c* info = mods::arg<dSv_info_c*>(args, 0);
    const int flag = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    const bool valid = info != nullptr && flag >= 0 && flag != 255 && room >= 0 && room < 64;
    const bool wasSet = valid && info->isSwitch(flag, room);
    sInfoSwitchWasSetStack.push_back(wasSet);
    if (valid && sActiveAdapter != nullptr && !sActiveAdapter->applying_remote()) {
        void* process = exact_local_switch_actor_context(true);
        if (process != nullptr) {
            const int actorName = fpcM_GetName(process);
            if (is_permanent_room_actor(actorName) &&
                permanent_room_actor_switch_flag(actorName, fopAcM_GetParam(process)) == flag) {
                if (actorName == fpcNm_Obj_IceWall_e) {
                    publish_room_actor_action(process, room_actor_switch_action(process, wasSet));
                } else if (!wasSet) {
                    publish_room_actor_action(process, RoomActorAction::Break);
                }
            }
        }
    }
    return HOOK_CONTINUE;
}

void info_switch_on_post(ModContext*, void* args, void*, void*) {
    bool wasSet = true;
    if (!sInfoSwitchWasSetStack.empty()) {
        wasSet = sInfoSwitchWasSetStack.back();
        sInfoSwitchWasSetStack.pop_back();
    }
    if (wasSet || sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    const int flag = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    void* process = exact_local_switch_actor_context(true);
    if (process == nullptr) return;
    const int actorName = fpcM_GetName(process);
    const bool webSwitch = is_web_switch_actor(actorName);
    const bool permanentActor = is_permanent_room_actor(actorName);
    const bool liveTorch = is_live_torch_switch_actor(actorName) &&
                           flag >= dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH &&
                           (fpcM_GetParam(process) & 0xFF) == static_cast<uint32_t>(flag);
    if (flag < 0 || flag >= 0xFF || room < 0 || room >= 64 ||
        (!is_small_key_door_switch_actor(actorName) && !webSwitch && !permanentActor &&
         !liveTorch) ||
        (!webSwitch && !permanentActor && !liveTorch &&
         flag < dSv_info_c::MEMORY_SWITCH) ||
        (permanentActor &&
         permanent_room_actor_switch_flag(actorName, fopAcM_GetParam(process)) != flag)) return;
    const int stage = current_stage_table();
    if (!valid_stage(stage)) return;
    const char* stageName = dComIfGp_getStartStageName();
    if (is_local_escort_gate_switch(stage, flag, actorName, room,
                                    stageName != nullptr ? stageName : "")) return;
    // Permanent actors already emitted a transient exact-room action above,
    // and their native durable bit is carried by switch_bit. Re-emitting the
    // room switch would allow a delayed room replay to visibly break the actor.
    if (permanentActor) return;
    sActiveAdapter->publish_local({
        {"type", "room_switch_bit"}, {"stage", stage}, {"flag", flag}, {"room", room},
        {"source_actor", actorName}, {"source_room", room},
        {"source_params", fpcM_GetParam(process)},
        {"source_action", 0}, {"set", true},
        {"source_stage", stageName != nullptr ? stageName : ""},
    });
}

HookAction info_switch_off_pre(ModContext*, void* args, void*, void*) {
    auto* info = mods::arg<dSv_info_c*>(args, 0);
    const int flag = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    const bool valid = info != nullptr && flag >= 0 && flag < 0xFF &&
                       room >= 0 && room < 64;
    sInfoSwitchWasSetOffStack.push_back(valid && info->isSwitch(flag, room));
    return HOOK_CONTINUE;
}

void info_switch_off_post(ModContext*, void* args, void*, void*) {
    const bool wasSet = !sInfoSwitchWasSetOffStack.empty() &&
                        sInfoSwitchWasSetOffStack.back();
    if (!sInfoSwitchWasSetOffStack.empty()) sInfoSwitchWasSetOffStack.pop_back();
    if (!wasSet || sActiveAdapter == nullptr || sActiveAdapter->applying_remote()) return;
    const int flag = mods::arg<int>(args, 1);
    const int room = mods::arg<int>(args, 2);
    void* process = exact_local_switch_actor_context(false);
    if (process == nullptr || !is_live_torch_switch_actor(fpcM_GetName(process)) ||
        flag < dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH ||
        flag >= 0xFF || room < 0 || room >= 64 ||
        (fpcM_GetParam(process) & 0xFF) != static_cast<uint32_t>(flag)) return;
    const int stage = current_stage_table();
    const char* stageName = dComIfGp_getStartStageName();
    if (!valid_stage(stage) || stageName == nullptr) return;
    sActiveAdapter->publish_local({
        {"type", "room_switch_bit"}, {"stage", stage}, {"flag", flag}, {"room", room},
        {"source_actor", fpcM_GetName(process)}, {"source_room", room},
        {"source_params", fpcM_GetParam(process)}, {"source_stage", stageName},
        {"set", false},
    });
}

void save_new(ModContext*, uint32_t slot, void* data) {
    static_cast<GameAdapter*>(data)->notify_local_save_new(slot);
}

void save_loaded(ModContext*, uint32_t slot, void* data) {
    static_cast<GameAdapter*>(data)->notify_local_save_loaded(slot);
}

void save_written(ModContext*, uint32_t slot, void* data) {
    static_cast<GameAdapter*>(data)->notify_local_save_written(slot);
}

void item_given(ModContext*, const ItemGiveInfo* info, void* data) {
    if (info != nullptr && data != nullptr) {
        static_cast<GameAdapter*>(data)->notify_local_item_grant(*info);
    }
}

class RemoteApplicationGuard {
public:
    explicit RemoteApplicationGuard(bool& value) : value_(value), previous_(value) { value_ = true; }
    ~RemoteApplicationGuard() { value_ = previous_; }
private:
    bool& value_;
    bool previous_;
};

}  // namespace

GameAdapter::GameAdapter(net::Transport& transport) : transport_(transport) {
    coop::attach(&transport_);
}

bool GameAdapter::randomizer_active() const {
    // The randomizer owns the "randomizer" game mode/save namespace. Unlike
    // the old all-in-one build, its private context is not linkable from this
    // standalone mod, so use the host-selected save namespace only to choose
    // randomizer packet semantics.
    const char* saveFileName = mDoMemCd_GetFileName();
    return saveFileName != nullptr && std::string_view(saveFileName) == "randomizer";
}

bool GameAdapter::local_wagon_escort_unfinished() {
    if (dComIfGs_isEventBit(dSv_event_flag_c::M_035)) return false;
    const char* stage = dComIfGp_getStartStageName();
    if (stage != nullptr && std::string_view(stage) == "F_SP121" &&
        dComIfGp_getStageStagInfo() != nullptr) {
        // A void restarts with layer override -1. Inspect the effective layer,
        // and remember participation after the player leaves that scene.
        const int layer = dComIfG_play_c::getLayerNo(0);
        if (layer == 2 || layer == 3) localWagonEscortStarted_ = true;
    }
    return localWagonEscortStarted_;
}

void GameAdapter::notify_local_event_bit(uint16_t flag) {
    if ((flag == dSv_event_flag_c::M_033 || flag == dSv_event_flag_c::M_036) &&
        !dComIfGs_isEventBit(dSv_event_flag_c::M_035)) {
        localWagonEscortStarted_ = true;
    } else if (flag == dSv_event_flag_c::M_035) {
        localWagonEscortStarted_ = false;
        std::erase_if(deferredStoryEvents_, [](const nlohmann::json& queued) {
            return queued.value("flag", -1) == dSv_event_flag_c::M_035;
        });
    }
    if (!is_ordon_day_boundary_event_bit(flag) || pendingOrdonEventBits_.erase(flag) == 0) {
        return;
    }
    if (!pendingOrdonEventBits_.empty()) return;

    ordonReloadSafeTicks_ = 0;
    ordonReloadWaitTicks_ = 0;
    ordonReloadTransitionActive_ = false;
    ordonReloadSawStageLoad_ = false;
    svc_log->info(mod_ctx,
        "Cancelled redundant Ordon reload after local progression reached the queued state");
}

void GameAdapter::notify_local_item_grant(const ItemGiveInfo& info) {
    if (applyingRemote_) return;
    if (ooccoo::is_grant(info.item) && !opening_or_title_active()) {
        bind_ooccoo_to_save();
        auto receipt = ooccoo::receipt_for_grant(info.item, current_ooccoo_scene());
        ooccooState_.record_local(receipt);
        svc_log->info(mod_ctx, ("OOCCOO local grant item=" + std::to_string(info.item) +
            " acquired=" + std::to_string(receipt.acquired)).c_str());
        // Send even if hydration just saw the native bit set by this very
        // grant. Receivers union receipts, so repeated reunion grants are safe.
        if (syncFlagsEnabled_ && transport_.status().welcomed) {
            publish_local({{"type", "ooccoo_state"}, {"state", ooccoo_snapshot_state()}});
        }
    }
    if (!syncFlagsEnabled_ || !transport_.status().welcomed || !randomizer_active()) return;

    std::string checkName = info.check_name != nullptr ? info.check_name : "";
    if (repeatable_pickup_check(checkName)) checkName.clear();
    if (local_only_randomizer_pickup(info.item, checkName)) return;
    // Ordinary currency pickups need only the wallet update. Named one-time
    // rewards still carry their location and notification, but not extra money.
    if (checkName.empty() && rupee_pickup_amount(info.item) != 0) return;
    if (!checkName.empty() && !completedRandomizerChecks_.insert(checkName).second) {
        svc_log->info(mod_ctx,
            ("Ignored duplicate local randomizer check '" + checkName + "'").c_str());
        return;
    }

    nlohmann::json packet = {
        {"type", "rando_item_get"},
        {"item_id", info.item},
    };
    if (ooccoo::is_grant(info.item)) packet["ooccoo_state"] = ooccoo_snapshot_state();
    if (!checkName.empty()) {
        packet["check_name"] = checkName;
        if (const auto flag = current_freestanding_check_flag(checkName)) {
            const int stage = current_stage_table();
            if (valid_stage(stage)) {
                packet["location_stage"] = stage;
                packet["location_flag"] = *flag;
            }
        }
    }
    publish_local(std::move(packet));

    std::ostringstream log;
    log << "Sent resolved randomizer item 0x" << std::hex << std::setw(2)
        << std::setfill('0') << static_cast<int>(info.item);
    if (!checkName.empty()) log << " for check '" << checkName << "'";
    svc_log->info(mod_ctx, log.str().c_str());
}

void GameAdapter::notify_local_ooccoo_warp_out() {
    // warpOutProc has already made the real Jr and adjusted its return point
    // for special rooms. Publish those exact native fields, not a guessed
    // point captured when Sr. was first picked up.
    if (applyingRemote_ || !ooccoo_sync_active()) return;
    bind_ooccoo_to_save();
    auto facts = native_ooccoo_facts(true, true);
    const int owner = local_ooccoo_return_owner();
    if (!ooccoo::is_dungeon(owner) ||
        !(facts.acquired & ooccoo::dungeon_bit(owner)) ||
        !ooccoo::valid_anchor(owner, facts.anchors[owner - ooccoo::FirstDungeon])) {
        svc_log->warn(mod_ctx, "OOCCOO native warp-out had no valid Jr return mark");
        return;
    }
    ooccooState_.observe_native_completion(facts.completed);
    ooccooState_.record_local(facts);
    svc_log->info(mod_ctx, ("OOCCOO native warp-out stage=" + std::to_string(owner) +
        " room=" + std::to_string(facts.anchors[owner - ooccoo::FirstDungeon].room)).c_str());
    publish_local({{"type", "ooccoo_state"}, {"state", ooccoo_snapshot_state()}});
}

void GameAdapter::remember_memory_item(int stage, int flag) {
    if (valid_stage(stage) && flag >= 0 && flag < dSv_info_c::DAN_ITEM) {
        observedMemoryItems_[stage].insert(flag);
    }
}

void GameAdapter::observe_local_memory_item(int stage, int flag) {
    if (applyingRemote_ || !syncFlagsEnabled_ || !valid_stage(stage) || flag < 0 ||
        flag >= dSv_info_c::DAN_ITEM) {
        return;
    }
    // Preserve the pickup before the welcome check. Current-stage memory can
    // be newer than the durable stage table until a later transition, so keep
    // this observation for late-join snapshots and periodic reapplication.
    remember_memory_item(stage, flag);
    publish_local({{"type", "item_bit"}, {"stage", stage}, {"flag", flag}});
}

void GameAdapter::reapply_observed_memory_items_for_current_stage() {
    const int stage = current_stage_table();
    const auto found = observedMemoryItems_.find(stage);
    if (!valid_stage(stage) || found == observedMemoryItems_.end()) return;
    for (const int flag : found->second) {
        if (stage_bits(stage).isItem(flag)) continue;
        RemoteApplicationGuard applying(applyingRemote_);
        stage_bits(stage).onItem(flag);
    }
}

void GameAdapter::notify_local_fish_caught(int fishIndex) {
    if (applyingRemote_ || !syncFlagsEnabled_ || !transport_.status().welcomed ||
        fishIndex < 0 || fishIndex >= kSyncedFishSpeciesCount) {
        return;
    }
    const int count = dComIfGs_getFishNum(static_cast<u8>(fishIndex));
    if (count < 0 || count > kMaxSyncedFishCount) return;
    if (++localFishSequence_ == 0) ++localFishSequence_;
    publish_local({{"type", "fish_record"}, {"index", fishIndex}, {"count", count},
                   {"max_size", dComIfGs_getFishSize(static_cast<u8>(fishIndex))},
                   {"catch_sequence", localFishSequence_}});
}

void GameAdapter::notify_local_light_drop_num(int area, int previous, int value) {
    if (applyingRemote_ || !syncFlagsEnabled_ || !transport_.status().welcomed ||
        area < 0 || area >= 4 || previous < 0 || previous > 0xFF || value < 0 || value > 0xFF) {
        return;
    }
    nlohmann::json light = {
        {"type", "light_drop_num"}, {"area", area},
        {"previous_count", previous}, {"count", value},
    };
    if (lastLocalTboxStage_ >= 0 && lastLocalTboxFlag_ >= 0 &&
        std::chrono::steady_clock::now() - lastLocalTboxAt_ <= std::chrono::seconds(2)) {
        light["tear_stage"] = lastLocalTboxStage_;
        light["tear_flag"] = lastLocalTboxFlag_;
    }
    lastLocalTboxStage_ = -1;
    lastLocalTboxFlag_ = -1;
    lastLocalTboxAt_ = {};
    publish_local(std::move(light));
}

void GameAdapter::notify_local_dark_clear(int level) {
    // A local or remotely-applied setter satisfies an older deferred clear at
    // the mutation boundary; do not leave the pending repair alive until the
    // next tick.
    if (level >= 0 && level < static_cast<int>(pendingDarkClears_.size())) {
        pendingDarkClears_[static_cast<size_t>(level)] = 0;
    }
    if (applyingRemote_ || level < 0 || level > 3) return;
    publish_local({{"type", "dark_clear_lv"}, {"no", level}});
}

void GameAdapter::notify_local_max_life(int previous, int value) {
    if (pendingMaxLifePublicationToSuppress_.has_value()) {
        const bool matches = *pendingMaxLifePublicationToSuppress_ == value;
        pendingMaxLifePublicationToSuppress_.reset();
        if (matches) return;
    }
    if (applyingRemote_ || !syncFlagsEnabled_ || !transport_.status().welcomed ||
        randomizer_active() ||
        value <= previous) return;
    if (++localPermanentSequence_ == 0) ++localPermanentSequence_;
    publish_local({
        {"type", "max_life_update"}, {"previous_value", previous}, {"value", value},
        {"event_sequence", localPermanentSequence_},
    });
}

bool GameAdapter::should_suppress_local_bottle_source(uint8_t sourceItem) const {
    return !applyingRemote_ && !randomizer_active() && is_vanilla_bottle_source(sourceItem) &&
           completedBottleSources_.find(sourceItem) != completedBottleSources_.end();
}

void GameAdapter::notify_local_bottle_slots(int previous, int value, uint8_t sourceItem) {
    if (applyingRemote_ || randomizer_active() || value <= previous) return;
    const bool knownSource = is_vanilla_bottle_source(sourceItem);
    if (knownSource) {
        completedBottleSources_.insert(sourceItem);
    }
    // Unknown grants also invalidate an exact ledger. Never carry an old
    // "complete" marker across a slot increase it cannot account for.
    bottleSourcesComplete_ = bottle_sources_exact(
        bottleSourcesComplete_, completedBottleSources_.size(), value);
    if (!syncFlagsEnabled_ || !transport_.status().welcomed) return;
    if (++localPermanentSequence_ == 0) ++localPermanentSequence_;
    nlohmann::json packet = {
        {"type", "bottle_slots"}, {"previous_count", previous}, {"count", value},
        {"event_sequence", localPermanentSequence_},
    };
    if (knownSource) {
        packet["source_item"] = sourceItem;
        packet["sources_complete"] = bottleSourcesComplete_;
    }
    publish_local(std::move(packet));
}

void GameAdapter::notify_local_rupees(int previous, int value) {
    // The meter hook and fallback polling observe the same setter. Remember
    // it here so a local mutation is not published again on the next poll.
    observedRupees_ = value;
    if (pendingRupeePublicationToSuppress_.has_value()) {
        const bool matches = *pendingRupeePublicationToSuppress_ == value;
        pendingRupeePublicationToSuppress_.reset();
        if (matches) {
            dusklight_online::log_info("Suppressed coalesced rupee total " + std::to_string(value));
            return;
        }
    }
    if (applyingRemote_ || !syncFlagsEnabled_ || !transport_.status().welcomed) return;

    // Match the AIO: wallet totals carry gains AND spending in both modes.
    // Remote rupee item grants are notification/location metadata only below.
    publish_local({{"type", "rupee_count"}, {"value", value}});
    dusklight_online::log_info("Sent rupee total " + std::to_string(value) +
                              " (previous " + std::to_string(previous) + ")");
}

void GameAdapter::notify_local_poe_pickup(int previous, int value) {
    if (!syncFlagsEnabled_ || !transport_.status().welcomed) return;
    const auto pickup = local_poe_pickup(previous, value, applyingRemote_,
        randomizer_active(), localPermanentSequence_, MAX_POH_NUM);
    if (!pickup) return;
    publish_local({{"type", "poe_count"}, {"previous_value", pickup->previous},
                   {"value", pickup->value}, {"event_sequence", pickup->sequence}});
    dusklight_online::log_info("Sent Poe pickup previous=" + std::to_string(previous) +
        " value=" + std::to_string(value) + " sequence=" + std::to_string(pickup->sequence));
}

ModResult GameAdapter::initialize_hooks(ModError* error) {
    sActiveAdapter = this;
    transport_.set_pose_delta_codec(&expand_remote_pose_delta,
                                    &prepare_remote_pose_delta);
    transport_.set_visual_wire_diagnostics(visual_wire_trace_enabled());
    if (install_remote_actor_profile(error) != MOD_OK ||
        install_audio_hooks(error) != MOD_OK ||
        install_floor_switch_hooks(*this, error) != MOD_OK ||
        install_bomb_hooks(transport_, error) != MOD_OK ||
        install_visual_hooks(error) != MOD_OK ||
        mods::hook::install<ToggleAutoSaveHook>() != MOD_OK ||
        mods::hook::add_pre<OoccooReunionHook>(&ooccoo_reunion_pre) != MOD_OK ||
        mods::hook::add_pre<OoccooWarpActorHook>(&ooccoo_warp_actor_pre) != MOD_OK ||
        mods::hook::add_post<OoccooWarpOutHook>(&ooccoo_warp_out_post) != MOD_OK ||
        mods::hook::add_pre<PvpDamageVectorHook>(&pvp_damage_vector_pre) != MOD_OK ||
        mods::hook::add_post<BombArrowPvpHitHook>(&bomb_arrow_pvp_hit_post) != MOD_OK ||
        mods::hook::add_pre<RemoteEnemyGroupHook>(&remote_enemy_group_pre) != MOD_OK ||
        mods::hook::add_pre<RemoteWolfLockHook>(&remote_wolf_lock_pre) != MOD_OK ||
        mods::hook::add_post<RemoteAttentionMarkHook>(&remote_attention_mark_post) != MOD_OK ||
        mods::hook::add_pre<SafeLineQueueHook>(&safe_line_queue_pre) != MOD_OK ||
        mods::hook::add_pre<DeleteTagRepairHook>(&delete_tag_repair_pre) != MOD_OK ||
        mods::hook::add_pre<NullParticleDeleteHook>(&null_particle_delete_pre) != MOD_OK ||
        mods::hook::add_pre<PoePickupHook>(&poe_pickup_pre) != MOD_OK ||
        mods::hook::add_post<PoePickupHook>(&poe_pickup_post) != MOD_OK ||
        mods::hook::add_post<EventBitOnHook>(&event_bit_on_post) != MOD_OK ||
        mods::hook::add_post<EventBitOffHook>(&event_bit_off_post) != MOD_OK ||
        mods::hook::add_post<MemoryTboxOnHook>(&memory_tbox_on_post) != MOD_OK ||
        mods::hook::add_post<MemoryItemOnHook>(&memory_item_on_post) != MOD_OK ||
        mods::hook::add_pre<MemorySwitchOnHook>(&memory_switch_on_pre) != MOD_OK ||
        mods::hook::add_post<MemorySwitchOnHook>(&memory_switch_on_post) != MOD_OK ||
        mods::hook::add_pre<MemorySwitchOffHook>(&memory_switch_off_pre) != MOD_OK ||
        mods::hook::add_post<MemorySwitchOffHook>(&memory_switch_off_post) != MOD_OK ||
        mods::hook::add_post<MemoryDungeonItemOnHook>(&memory_dungeon_item_on_post) != MOD_OK ||
        mods::hook::add_pre<VisitedRoomOnHook>(&visited_room_on_pre) != MOD_OK ||
        mods::hook::add_post<VisitedRoomOnHook>(&visited_room_on_post) != MOD_OK ||
        mods::hook::add_pre<PlayerItemFirstOnHook>(&player_item_first_on_pre) != MOD_OK ||
        mods::hook::add_post<PlayerItemFirstOnHook>(&player_item_first_on_post) != MOD_OK ||
        mods::hook::add_pre<PlayerItemSetHook>(&player_item_set_pre) != MOD_OK ||
        mods::hook::add_post<PlayerItemSetHook>(&player_item_set_post) != MOD_OK ||
        mods::hook::add_pre<PlayerItemFirstOffHook>(&player_item_first_off_pre) != MOD_OK ||
        mods::hook::add_post<PlayerItemFirstOffHook>(&player_item_first_off_post) != MOD_OK ||
        mods::hook::add_post<PlayerCollectSetHook>(&player_collect_set_post) != MOD_OK ||
        mods::hook::add_post<PlayerCrystalSetHook>(&player_crystal_set_post) != MOD_OK ||
        mods::hook::add_post<PlayerMirrorSetHook>(&player_mirror_set_post) != MOD_OK ||
        mods::hook::add_post<FishingAddCountHook>(&fishing_add_count_post) != MOD_OK ||
        mods::hook::add_pre<LightDropNumSetHook>(&light_drop_num_set_pre) != MOD_OK ||
        mods::hook::add_post<LightDropNumSetHook>(&light_drop_num_set_post) != MOD_OK ||
        mods::hook::add_post<LightDropFlagOnHook>(&light_drop_flag_on_post) != MOD_OK ||
        mods::hook::add_pre<EmptyBottleSetHook>(&empty_bottle_set_pre) != MOD_OK ||
        mods::hook::add_post<EmptyBottleSetHook>(&empty_bottle_set_post) != MOD_OK ||
        mods::hook::add_pre<EmptyBottleItemSetHook>(&empty_bottle_item_set_pre) != MOD_OK ||
        mods::hook::add_post<EmptyBottleItemSetHook>(&empty_bottle_item_set_post) != MOD_OK ||
        mods::hook::add_post<EventRegSetHook>(&event_reg_set_post) != MOD_OK ||
        mods::hook::add_post<SmellTypeSetHook>(&smell_type_set_post) != MOD_OK ||
        mods::hook::add_post<DarkClearSetHook>(&dark_clear_set_post) != MOD_OK ||
        mods::hook::add_post<TransformSetHook>(&transform_set_post) != MOD_OK ||
        mods::hook::add_post<LetterGetSetHook>(&letter_get_set_post) != MOD_OK ||
        mods::hook::add_pre<StageKeyNumSetHook>(&stage_key_num_set_pre) != MOD_OK ||
        mods::hook::add_post<StageKeyNumSetHook>(&stage_key_num_set_post) != MOD_OK ||
        mods::hook::add_pre<MeterMoveKeyHook>(&meter_move_key_pre) != MOD_OK ||
        mods::hook::add_post<MeterMoveKeyHook>(&meter_move_key_post) != MOD_OK ||
        mods::hook::add_pre<MeterMoveLifeHook>(&meter_move_life_pre) != MOD_OK ||
        mods::hook::add_post<MeterMoveLifeHook>(&meter_move_life_post) != MOD_OK ||
        mods::hook::add_pre<MeterMoveRupeeHook>(&meter_move_rupee_pre) != MOD_OK ||
        mods::hook::add_post<MeterMoveRupeeHook>(&meter_move_rupee_post) != MOD_OK ||
        mods::hook::add_pre<ProcessExecuteHook>(&process_execute_pre) != MOD_OK ||
        mods::hook::add_post<ProcessExecuteHook>(&process_execute_post) != MOD_OK ||
        mods::hook::add_post<Door20CheckExecuteHook>(&door20_check_execute_post) != MOD_OK ||
        mods::hook::add_pre<Door20StopOpenHook>(&door20_stop_open_pre) != MOD_OK ||
        mods::hook::add_post<Door20StopOpenHook>(&door20_stop_open_post) != MOD_OK ||
        mods::hook::add_pre<MirrorTableExecuteHook>(&mirror_table_execute_pre) != MOD_OK ||
        mods::hook::add_pre<MirrorChainDrawHook>(&mirror_chain_draw_pre) != MOD_OK ||
        mods::hook::add_pre<InfoSwitchOnHook>(&info_switch_on_pre) != MOD_OK ||
        mods::hook::add_post<InfoSwitchOnHook>(&info_switch_on_post) != MOD_OK ||
        mods::hook::add_pre<InfoSwitchOffHook>(&info_switch_off_pre) != MOD_OK ||
        mods::hook::add_post<InfoSwitchOffHook>(&info_switch_off_post) != MOD_OK) {
        shutdown_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "required progression mutation hooks are unavailable");
    }
    if (svc_save->observe_saves(mod_ctx, &save_new, &save_loaded, &save_written,
                                this, &saveObserver_) != MOD_OK) {
        shutdown_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "required save lifecycle observer is unavailable");
    }
    if (svc_item->observe_gives(mod_ctx, &item_given, this, &itemGiveObserver_) != MOD_OK) {
        shutdown_hooks();
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "required item grant observer is unavailable");
    }
    dusk::multiplayer::set_remote_pvp_hit_callback(&remote_link_pvp_target_hit);
    (void)coop::install_hooks();
    hooksInstalled_ = true;
    return MOD_OK;
}

void GameAdapter::shutdown_hooks() {
    coop::uninstall_hooks();
    uninstall_floor_switch_hooks();
    if (sActiveAdapter == this) sActiveAdapter = nullptr;
    transport_.set_pose_delta_codec(nullptr, nullptr);
    dusk::multiplayer::set_remote_pvp_hit_callback(nullptr);
    // uninstall is idempotent for entries that were not fully installed.
    if (itemGiveObserver_ != 0) {
        svc_item->unobserve_gives(mod_ctx, itemGiveObserver_);
        itemGiveObserver_ = 0;
    }
    if (saveObserver_ != 0) {
        svc_save->unobserve_saves(mod_ctx, saveObserver_);
        saveObserver_ = 0;
    }
    mods::hook::uninstall<InfoSwitchOnHook>();
    mods::hook::uninstall<InfoSwitchOffHook>();
    mods::hook::uninstall<MirrorTableExecuteHook>();
    mods::hook::uninstall<MirrorChainDrawHook>();
    mods::hook::uninstall<Door20StopOpenHook>();
    mods::hook::uninstall<Door20CheckExecuteHook>();
    mods::hook::uninstall<ProcessExecuteHook>();
    mods::hook::uninstall<MeterMoveRupeeHook>();
    mods::hook::uninstall<MeterMoveLifeHook>();
    mods::hook::uninstall<MeterMoveKeyHook>();
    mods::hook::uninstall<StageKeyNumSetHook>();
    mods::hook::uninstall<LetterGetSetHook>();
    mods::hook::uninstall<TransformSetHook>();
    mods::hook::uninstall<DarkClearSetHook>();
    mods::hook::uninstall<SmellTypeSetHook>();
    mods::hook::uninstall<EventRegSetHook>();
    mods::hook::uninstall<EmptyBottleItemSetHook>();
    mods::hook::uninstall<EmptyBottleSetHook>();
    mods::hook::uninstall<LightDropFlagOnHook>();
    mods::hook::uninstall<LightDropNumSetHook>();
    mods::hook::uninstall<FishingAddCountHook>();
    mods::hook::uninstall<PlayerMirrorSetHook>();
    mods::hook::uninstall<PlayerCrystalSetHook>();
    mods::hook::uninstall<PlayerCollectSetHook>();
    mods::hook::uninstall<PlayerItemFirstOffHook>();
    mods::hook::uninstall<PlayerItemFirstOnHook>();
    mods::hook::uninstall<PlayerItemSetHook>();
    mods::hook::uninstall<VisitedRoomOnHook>();
    mods::hook::uninstall<MemoryDungeonItemOnHook>();
    mods::hook::uninstall<MemorySwitchOffHook>();
    mods::hook::uninstall<MemorySwitchOnHook>();
    mods::hook::uninstall<MemoryItemOnHook>();
    mods::hook::uninstall<MemoryTboxOnHook>();
    mods::hook::uninstall<EventBitOffHook>();
    mods::hook::uninstall<EventBitOnHook>();
    mods::hook::uninstall<OoccooWarpActorHook>();
    mods::hook::uninstall<OoccooWarpOutHook>();
    mods::hook::uninstall<OoccooReunionHook>();
    mods::hook::uninstall<ToggleAutoSaveHook>();

    // Remote Link's method table lives in this DLL. Finish every queued actor
    // deletion before removing the lifecycle-repair hooks or unloading any
    // bridge code that its Delete method may call.
    destroy_remote_actor_processes_for_unload();

    mods::hook::uninstall<PvpDamageVectorHook>();
    mods::hook::uninstall<BombArrowPvpHitHook>();
    mods::hook::uninstall<RemoteAttentionMarkHook>();
    mods::hook::uninstall<RemoteWolfLockHook>();
    mods::hook::uninstall<RemoteEnemyGroupHook>();
    mods::hook::uninstall<NullParticleDeleteHook>();
    mods::hook::uninstall<DeleteTagRepairHook>();
    mods::hook::uninstall<SafeLineQueueHook>();
    uninstall_visual_hooks();
    uninstall_bomb_hooks();
    uninstall_audio_hooks();
    uninstall_remote_actor_profile();
    sExecutingProcessStack.clear();
    sWebDeleteTimerStack.clear();
    sRoomActorActionStateStack.clear();
    sRemoteMoveboxPushPullKeepStack.clear();
    sDoor20ExecuteModes.clear();
    sDoor20StopOpenDepth = 0;
    sInfoSwitchWasSetStack.clear();
    sInfoSwitchWasSetOffStack.clear();
    sMemorySwitchWasSetStack.clear();
    sMemorySwitchWasSetOffStack.clear();
    sItemFirstWasOwnedStack.clear();
    mods::hook::uninstall<PoePickupHook>();
    sPoePickupPrevious.clear();
    sItemFirstWasOwnedOffStack.clear();
    sStageKeyPreviousCounts.clear();
    sPendingMeterKeyMutations.clear();
    sPendingMeterLifeMutations.clear();
    sPendingMeterRupeeMutations.clear();
    sLightDropPreviousCounts.clear();
    sEmptyBottleMutations.clear();
    sEmptyBottleItemMutations.clear();
    sVisitedRoomPendingRegions.clear();
    hooksInstalled_ = false;
    sWorldSyncEnabled = false;
}

void GameAdapter::publish_local(nlohmann::json message) {
    if (applyingRemote_ || !syncFlagsEnabled_ || !transport_.status().welcomed) return;
    const std::string type = message.value("type", std::string());
    if (type == "tbox_bit") {
        lastLocalTboxStage_ = message.value("stage", -1);
        lastLocalTboxFlag_ = message.value("flag", -1);
        lastLocalTboxAt_ = std::chrono::steady_clock::now();
    }
    if (type == "switch_bit") {
        const int stage = message.value("stage", -1), flag = message.value("flag", -1);
        const bool set = message.value("set", true);
        if (stage == dStage_SaveTbl_FARON && flag == 72 && set) {
            set_local_faron_warp_sequence_active(true);
        }
        if (stage == dStage_SaveTbl_FARON && flag == 71 && set) {
            // Completion releases every peer's safety fence. Advertise the
            // inactive participant state before the completion switch.
            set_local_faron_warp_sequence_active(false);
            transport_.send(message);
            return;
        }
        if (is_faron_warp_sequence_switch(stage, flag) &&
            should_defer_faron_warp_sequence()) {
            // This is a trusted local mutation stream. Preserve every ordered
            // set/off edge.
            deferredLocalEvents_.push_back(std::move(message));
            return;
        }
    }
    if (type == "event_bit") {
        const int raw = message.value("flag", -1);
        if (raw >= 0 && raw <= 0xFFFF) {
            const uint16_t flag = static_cast<uint16_t>(raw);
            if (is_faron_warp_sequence_event_bit(flag) &&
                should_defer_faron_warp_sequence()) {
                deferredLocalEvents_.push_back(std::move(message));
                return;
            }
            if (message.value("set", true) && is_ordon_day_boundary_event_bit(flag)) {
                deferredLocalEvents_.push_back(std::move(message));
                faronDayBroadcastHoldTicks_ = std::max<uint32_t>(faronDayBroadcastHoldTicks_, 60);
                return;
            }
        }
    }
    transport_.send(message);
}

void GameAdapter::report_pvp_target_hit(fopAc_ac_c* remoteLinkActor,
                                        fopAc_ac_c* attackActor,
                                        dCcD_GObjInf* attackInfo) {
    if (remoteLinkActor == nullptr || attackActor == nullptr || attackInfo == nullptr) return;

    daAlink_c* link = nullptr;
    fopAc_ac_c* sourceActor = nullptr;
    bool allowSwordReaction = true;
    const int attackActorName = fopAcM_GetName(attackActor);
    if (attackActorName == fpcNm_ALINK_e) {
        link = static_cast<daAlink_c*>(attackActor);
    } else if (attackActorName == fpcNm_ARROW_e) {
        link = daAlink_getAlinkActorClass();
        sourceActor = attackActor;
        allowSwordReaction = false;
    } else if (attackActorName == fpcNm_SPINNER_e) {
        link = daAlink_getAlinkActorClass();
        if (link == nullptr || !link->checkSpinnerRideOwn(attackActor) ||
            !attackInfo->ChkAtType(AT_TYPE_SPINNER)) return;
        sourceActor = attackActor;
        allowSwordReaction = false;
    } else {
        return;
    }
    if (link == nullptr || !attackInfo->ChkAtHit()) return;

    const net::Status status = transport_.status();
    if (!status.welcomed || !net::effective_pvp(status.settings) ||
        !stage_ready() || manualTransitionActive_) return;

    std::string targetPeerId;
    if (!dusk::multiplayer::get_remote_link_dummy_peer_id_for_actor(remoteLinkActor,
                                                                    &targetPeerId) ||
        targetPeerId.empty()) return;
    const std::string localPeerId = status.mode == net::Mode::DirectHost ?
                                        "direct" : status.clientId;
    if (targetPeerId == localPeerId) return;

    // A normal sword swing enables three attack capsules. More than one can
    // touch the same remote target cylinder in a single collision pass; each
    // callback carries a different dCcD_GObjInf and would otherwise become a
    // distinct sequence. Deduplicate only the same attacking actor/target in
    // this game update, preserving separate attacks on later updates.
    const auto contact = std::make_pair(targetPeerId,
        reinterpret_cast<uintptr_t>(attackActor));
    if (!pvpLocalHitContactsThisUpdate_.insert(contact).second) return;

    const bool spinnerHit = attackActorName == fpcNm_SPINNER_e;
    const bool spinnerAttack = spinnerHit && attackInfo->GetAtAtp() >= 2;
    if (spinnerHit) {
        const uintptr_t spinnerActor = reinterpret_cast<uintptr_t>(attackActor);
        if (pvpSpinnerActor_ != spinnerActor || pvpSpinnerAttackActive_ != spinnerAttack) {
            pvpSpinnerAttackTargets_.clear();
        }
        pvpSpinnerActor_ = spinnerActor;
        pvpSpinnerAttackActive_ = spinnerAttack;
        if (spinnerAttack && !pvpSpinnerAttackTargets_.insert(targetPeerId).second) return;
    }

    int attackClass = spinnerAttack ? kPvpAttackHeavy : kPvpAttackLight;
    if (!spinnerHit && !attackInfo->ChkAtType(AT_TYPE_HEAVY_BOOTS) &&
        (attackInfo->GetAtSpl() != dCcG_At_Spl_UNK_0 ||
         attackInfo->ChkAtType(AT_TYPE_IRON_BALL | AT_TYPE_WOLF_CUT_TURN |
                               AT_TYPE_MIDNA_LOCK))) {
        attackClass = kPvpAttackHeavy;
    }
    const bool clawshot = attackInfo->ChkAtType(AT_TYPE_HOOKSHOT);
    const bool slingshot = attackActorName == fpcNm_ARROW_e &&
                           attackInfo->ChkAtType(AT_TYPE_SLINGSHOT);
    const bool ironBallLaunch = attackInfo->ChkAtType(AT_TYPE_IRON_BALL);
    const bool shieldBash = attackInfo->ChkAtType(AT_TYPE_SHIELD_ATTACK);
    const int cutType = allowSwordReaction ? static_cast<int>(link->getCutType()) :
                                             static_cast<int>(daPy_py_c::CUT_TYPE_NONE);
    std::string_view reaction;
    if (ironBallLaunch) {
        reaction = kPvpReactionIronBallLaunch;
    } else if (clawshot) {
        reaction = kPvpReactionClawshot;
    } else if (slingshot) {
        reaction = kPvpReactionSlingshot;
    } else if (shieldBash) {
        reaction = kPvpReactionShieldBash;
    } else if (cutType == daPy_py_c::CUT_TYPE_MORTAL_DRAW_A ||
               cutType == daPy_py_c::CUT_TYPE_MORTAL_DRAW_B) {
        reaction = kPvpReactionMortalDraw;
    } else if (cutType == daPy_py_c::CUT_TYPE_LARGE_TURN_LEFT ||
               cutType == daPy_py_c::CUT_TYPE_LARGE_TURN_RIGHT) {
        reaction = kPvpReactionGreatSpin;
    }
    const bool blocked = !ironBallLaunch && !shieldBash && attackInfo->ChkAtShieldHit();
    const cXyz* sourcePos = sourceActor != nullptr ? &sourceActor->current.pos :
                                                     &link->current.pos;
    if (ironBallLaunch) {
        if (const cXyz* ironBallPos = link->getIronBallCenterPos(); ironBallPos != nullptr) {
            sourcePos = ironBallPos;
        }
    }
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr) return;

    nlohmann::json state = {
        {"target_peer_id", targetPeerId},
        {"stage", stage},
        {"room", static_cast<int>(dComIfGp_roomControl_getStayNo())},
        {"attack_class", attackClass},
        {"blocked", blocked},
        {"source_x", sourcePos->x},
        {"source_y", sourcePos->y},
        {"source_z", sourcePos->z},
    };
    if (!reaction.empty()) state["reaction"] = reaction;
    (void)transport_.send({
        {"type", "pvp_hit"},
        {"sequence", ++localPvpHitSequence_},
        {"state", std::move(state)},
    });
}

void GameAdapter::clear_replaced_save_progression_state() {
    reset_floor_switch_state();
    // These observations and deferred mutations belong to the save data that
    // was just replaced. Keeping them can resurrect another file's item bits
    // or apply an old story mutation after the new file becomes active.
    observedMemoryItems_.clear();
    completedRandomizerChecks_.clear();
    deferredFaronInbound_.clear();
    deferredSwitches_.clear();
    pendingDarkClears_.fill(0);
    pendingOrdonEventBits_.clear();
    ordonReloadSafeTicks_ = 0;
    ordonReloadWaitTicks_ = 0;
    ordonReloadTransitionActive_ = false;
    ordonReloadSawStageLoad_ = false;
    mirrorReloadPending_ = false;
    zoraThawPending_ = nlohmann::json();
    deferredStoryEvents_.clear();
    deferredLocalEvents_.clear();
    faronDayBroadcastHoldTicks_ = 0;
    localFaronCageSequenceActive_ = false;
    localFaronWarpSequenceActive_ = false;
    lastLocalTboxStage_ = -1;
    lastLocalTboxFlag_ = -1;
    lastLocalTboxAt_ = {};
    stableStageName_.clear();
    stableRoom_ = -128;
    stableRoomTicks_ = 0;
    initializedStageName_.clear();
    initializedRoom_ = -128;
    initializedRoomTicks_ = 0;
    localObservedState_ = nlohmann::json();
    observedBombBagRental_.reset();
    pendingBombBagSaveWarp_.reset();
    pendingRupeePublicationToSuppress_.reset();
    observedRupees_.reset();
    pendingMaxLifePublicationToSuppress_.reset();
    pendingManualVibration_.reset();
}

void GameAdapter::notify_local_save_reset() {
    liveFieldMapMarker_.reset();
    // No detached Note/owner data may survive a selected-save boundary.
    localWagonEscortStarted_ = false;
    ooccooState_.reset();
    ooccooBoundToSave_ = false;
    ooccooCatchupPending_ = true;
    ooccooReplyPending_ = false;
    completedBottleSources_.clear();
    bottleSourcesComplete_ = true;
    clear_replaced_save_progression_state();

    // A save observer callback can arrive while a manual sync or prompt from
    // the previous file is pending. It must never overwrite the newly selected
    // file later in the same connection.
    pendingManualInfo_.clear();
    pendingManualFlagsSave_.clear();
    pendingManualVibration_.reset();
    manualTransitionActive_ = false;
    manualReloadPending_ = false;
    progressionPrompt_ = {};
    progressionPromptAcceptHeld_ = false;
    pendingProgressionCues_.clear();
    pendingProgressionPeerId_.clear();
    pendingProgressionCueKey_.clear();
    awaitingManualSyncCueKey_.clear();
    awaitingManualSyncPeerId_.clear();
    awaitingManualSyncRequestId_ = 0;
    handledProgressionCues_.clear();
    manualSyncState_ = ManualSyncState::None;
    manualSyncFlagsOnly_ = false;
    manualSyncRequestedFromTitle_ = false;
    manualSyncPeerId_.clear();
    manualSyncRequestId_ = 0;
    manualSyncWaitTicks_ = 0;
    manualSyncTimedOut_ = false;
}

void GameAdapter::notify_local_save_new(uint32_t slot) {
    const char* name = mDoMemCd_GetFileName();
    activeSaveName_ = name != nullptr ? name : "";
    activeSaveSlot_ = slot;
    saveRecovery_.reset();
    notify_local_save_reset();
}

void GameAdapter::set_save_recovery_policy(bool ready, bool enabled) {
    saveRecoveryPolicyReady_ = ready;
    if (!ready) return;
    if (saveRecoveryEnabled_ == enabled) return;
    saveRecoveryEnabled_ = enabled;
    if (!enabled) saveRecovery_.reset();
}

void GameAdapter::notify_local_save_loaded(uint32_t slot) {
    const char* name = mDoMemCd_GetFileName();
    activeSaveName_ = name != nullptr ? name : "";
    activeSaveSlot_ = slot;
    if (saveRecovery_ && (name == nullptr || saveRecovery_->saveName != name ||
                          saveRecovery_->slot != slot))
        saveRecovery_.reset();
    // More updates can arrive after this callback, before loading finishes.
    if (saveRecovery_) saveRecovery_->restorePending = true;
    svc_log->info(mod_ctx, ("Save recovery load slot=" + std::to_string(slot) +
        " pending=" + (saveRecovery_ && saveRecovery_->restorePending ? "yes" : "no") +
        " title_messages=" +
        std::to_string(saveRecovery_ ? saveRecovery_->titleMessages.size() : 0)).c_str());
    notify_local_save_reset();
    load_bottle_source_state();
    load_bomb_bag_save_warp_candidate();

    size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kOoccooSaveBlob.data(), nullptr, &size) != MOD_OK ||
        size == 0 || size > 4096) return;
    std::string encoded(size, '\0');
    if (svc_save->get_blob(mod_ctx, kOoccooSaveBlob.data(), encoded.data(), &size) != MOD_OK ||
        size > encoded.size()) return;
    try {
        const auto candidate = nlohmann::json::parse(encoded.begin(), encoded.begin() + size);
        if (!candidate.is_object() || !candidate.contains("state") ||
            !candidate.contains("pending")) return;
        const auto state = ooccoo::decode(candidate["state"]);
        const auto pending = ooccoo::bounded_integer(candidate["pending"], 0x1FF);
        if (state && pending) {
            if (candidate.contains("managed_form") && !candidate["managed_form"].is_boolean())
                return;
            const bool legacyNote = candidate["state"].value("version", 0) == 2 &&
                dComIfGs_getItem(SLOT_18, false) == dItemNo_TKS_LETTER_e &&
                state->acquired != 0 && !state->unboundNote;
            ooccooState_.restore(*state, static_cast<uint16_t>(*pending),
                candidate.value("managed_form", legacyNote));
        }
    } catch (const nlohmann::json::exception&) {
        // Ignore corrupt/legacy companion data. Bind from the selected file's
        // proven dungeon flags later; never adopt the preceding save's Note.
    }
}

void GameAdapter::load_bottle_source_state() {
    const int bottles = bottle_slot_count();
    completedBottleSources_ = bottle_sources_from_vanilla_save();
    if (static_cast<int>(completedBottleSources_.size()) > bottles) {
        completedBottleSources_.clear();
    }
    bottleSourcesComplete_ = static_cast<int>(completedBottleSources_.size()) == bottles;

    size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kBottleSourcesSaveBlob.data(), nullptr, &size) != MOD_OK ||
        size == 0 || size > 1024) {
        return;
    }
    std::string encoded(size, '\0');
    if (svc_save->get_blob(mod_ctx, kBottleSourcesSaveBlob.data(), encoded.data(), &size) != MOD_OK) {
        return;
    }
    try {
        const nlohmann::json state = nlohmann::json::parse(encoded.begin(), encoded.begin() + size);
        if (!state.is_object() || state.value("version", 0) != 1) return;
        std::set<uint8_t> sources = parse_bottle_sources(
            state.value("sources", nlohmann::json::array()));
        sources.insert(completedBottleSources_.begin(), completedBottleSources_.end());
        if (static_cast<int>(sources.size()) > bottles) return;
        completedBottleSources_ = std::move(sources);
        bottleSourcesComplete_ =
            static_cast<int>(completedBottleSources_.size()) == bottles;
    } catch (const nlohmann::json::exception&) {
        completedBottleSources_ = bottle_sources_from_vanilla_save();
        if (static_cast<int>(completedBottleSources_.size()) > bottles) {
            completedBottleSources_.clear();
        }
        bottleSourcesComplete_ =
            static_cast<int>(completedBottleSources_.size()) == bottles;
    }
}

void GameAdapter::persist_bottle_source_state() const {
    const nlohmann::json state = {
        {"version", 1},
        {"complete", bottleSourcesComplete_},
        {"sources", completedBottleSources_},
    };
    const std::string encoded = state.dump();
    (void)svc_save->set_blob(mod_ctx, kBottleSourcesSaveBlob.data(),
                             encoded.data(), encoded.size());
}

void GameAdapter::replace_bottle_source_state(const nlohmann::json& message) {
    const int bottles = std::clamp(message.value("bottle_slots", bottle_slot_count()), 0, 4);
    std::set<uint8_t> sources = parse_bottle_sources(
        message.value("bottle_sources", nlohmann::json::array()));
    if (static_cast<int>(sources.size()) > bottles) sources.clear();
    completedBottleSources_ = std::move(sources);
    bottleSourcesComplete_ = message.value("bottle_sources_complete", false) &&
        static_cast<int>(completedBottleSources_.size()) == bottles;
}

void GameAdapter::notify_local_save_written(uint32_t slot) {
    bind_ooccoo_to_save();
    const nlohmann::json state = {
        {"state", ooccoo_snapshot_state()}, {"pending", ooccooState_.pending()},
        {"managed_form", ooccooState_.managed_form()},
    };
    const std::string encoded = state.dump();
    (void)svc_save->set_blob(mod_ctx, kOoccooSaveBlob.data(), encoded.data(), encoded.size());
    (void)svc_save->delete_blob(mod_ctx, kLegacyOoccooSaveBlob.data());
    persist_bottle_source_state();
    persist_bomb_bag_save_warp_candidate();
    const char* name = mDoMemCd_GetFileName();
    activeSaveName_ = name != nullptr ? name : "";
    activeSaveSlot_ = slot;
    // An autosave can occur during the load transition, before the journal's
    // safe stage boundary. Preserve the pending repair instead of discarding
    // the only copy of those post-save updates.
    if (saveRecovery_ && saveRecovery_->restorePending && name != nullptr &&
        saveRecovery_->saveName == name && saveRecovery_->slot == slot) return;
    saveRecovery_.reset();
    const net::Status status = transport_.status();
    if (saveRecoveryEnabled_ && saveRecoveryPolicyReady_ && syncFlagsEnabled_ && status.enabled &&
        (status.mode == net::Mode::DirectHost || status.welcomed) &&
        !randomizer_active() && name != nullptr && *name != '\0') {
        SaveRecoveryJournal journal;
        journal.saveName = name;
        journal.slot = slot;
        journal.savedAt = std::chrono::steady_clock::now();
        journal.captureUntil = journal.savedAt + std::chrono::seconds(60);
        journal.delta.begin(make_recovery_snapshot());
        saveRecovery_ = std::move(journal);
        svc_log->info(mod_ctx, ("Save recovery armed slot=" +
            std::to_string(slot) + " for 60 seconds").c_str());
    }
}

void GameAdapter::persist_bomb_bag_save_warp_candidate() {
    // The save menu calls resetMiniGameItem(true), then may call
    // setMiniGameItem again. That overwrites mSaveBombItem, so use the empty
    // slot observed when the rental began, not the value at save completion.
    pendingBombBagSaveWarp_.reset();
    const int bag = dMeter2Info_getRentalBombBag();
    const int item = bag >= 0 && bag < 3 ? dComIfGs_getItem(SLOT_15 + bag, false) : -1;
    const int count = bag >= 0 && bag < 3 ? dComIfGs_getBombNum(static_cast<u8>(bag)) : 0;
    if (!syncFlagsEnabled_ || !transport_.status().welcomed ||
        g_meter2_info.getMiniGameItemSetFlag() == 0 ||
        !observedBombBagRental_ || observedBombBagRental_->bag != bag) {
        if (bag >= 0 && bag < 3 && syncFlagsEnabled_ && transport_.status().welcomed) {
            svc_log->info(mod_ctx, ("BOMB_BAG save-warp skipped bag=" +
                std::to_string(bag) + " prior=" +
                std::to_string(g_meter2_info.mSaveBombItem) + " item=" +
                std::to_string(item) + " count=" + std::to_string(count) +
                " mini=" + std::to_string(g_meter2_info.getMiniGameItemSetFlag()) +
                " observed=" + (observedBombBagRental_ ?
                    std::to_string(observedBombBagRental_->bag) + ":" +
                    std::to_string(observedBombBagRental_->item) : "none")).c_str());
        }
        (void)svc_save->delete_blob(mod_ctx, kBombBagSaveWarpBlob.data());
        return;
    }
    const std::string encoded = nlohmann::json{{"version", 1}, {"bag", bag},
                                               {"item", observedBombBagRental_->item}}.dump();
    if (svc_save->set_blob(mod_ctx, kBombBagSaveWarpBlob.data(),
            encoded.data(), encoded.size()) == MOD_OK) {
        svc_log->info(mod_ctx, ("BOMB_BAG save-warp candidate bag=" +
            std::to_string(bag) + " item=" +
            std::to_string(observedBombBagRental_->item)).c_str());
    } else {
        svc_log->warn(mod_ctx, "BOMB_BAG save-warp candidate could not be saved");
    }
}

void GameAdapter::observe_bomb_bag_rental() {
    const int bag = dMeter2Info_getRentalBombBag();
    if (!syncFlagsEnabled_ || opening_or_title_active() || randomizer_active() ||
        g_meter2_info.getMiniGameItemSetFlag() == 0 || bag < 0 || bag >= 3) {
        observedBombBagRental_.reset();
        return;
    }
    if (observedBombBagRental_ && observedBombBagRental_->bag == bag) return;
    observedBombBagRental_.reset();
    const int item = dComIfGs_getItem(SLOT_15 + bag, false);
    const int count = dComIfGs_getBombNum(static_cast<u8>(bag));
    if (save_warp_bag_candidate(false, syncable_bomb_item(item), bag,
            g_meter2_info.mSaveBombItem, dItemNo_NONE_e, count)) {
        observedBombBagRental_ = BombBagSaveWarpCandidate{bag, item};
        svc_log->info(mod_ctx, ("BOMB_BAG rental observed empty slot bag=" +
            std::to_string(bag) + " item=" + std::to_string(item)).c_str());
    }
}

void GameAdapter::load_bomb_bag_save_warp_candidate() {
    size_t size = 0;
    if (svc_save->get_blob(mod_ctx, kBombBagSaveWarpBlob.data(), nullptr, &size) != MOD_OK)
        return;
    if (size == 0 || size > 128) {
        (void)svc_save->delete_blob(mod_ctx, kBombBagSaveWarpBlob.data());
        return;
    }
    std::string encoded(size, '\0');
    if (svc_save->get_blob(mod_ctx, kBombBagSaveWarpBlob.data(), encoded.data(), &size) != MOD_OK ||
        size > encoded.size()) return;
    std::optional<BombBagSaveWarpCandidate> candidate;
    try {
        const auto value = nlohmann::json::parse(encoded.begin(), encoded.begin() + size);
        if (value.is_object() && value.size() == 3 && value.contains("version") &&
            value.contains("bag") && value.contains("item") &&
            value["version"].is_number_integer() && value["version"] == 1 &&
            value["bag"].is_number_integer() && value["item"].is_number_integer()) {
            const int bag = value["bag"].get<int>();
            const int item = value["item"].get<int>();
            if (bag >= 0 && bag < 3 && syncable_bomb_item(item))
                candidate = BombBagSaveWarpCandidate{bag, item};
        }
    } catch (const nlohmann::json::exception&) {
        // A corrupt companion blob cannot turn a loaded bag into progression.
    }
    // Consume before sending. A second load cannot replay old ammo over a
    // peer's newer count, even if this game exits immediately afterward.
    if (svc_save->delete_blob(mod_ctx, kBombBagSaveWarpBlob.data()) == MOD_OK) {
        pendingBombBagSaveWarp_ = candidate;
        if (candidate) svc_log->info(mod_ctx, ("BOMB_BAG save-warp loaded candidate bag=" +
            std::to_string(candidate->bag)).c_str());
    } else {
        svc_log->warn(mod_ctx, "BOMB_BAG save-warp candidate could not be consumed");
    }
}

void GameAdapter::flush_bomb_bag_save_warp_candidate() {
    if (!pendingBombBagSaveWarp_ || !syncFlagsEnabled_ ||
        !transport_.status().welcomed || !stage_ready() || opening_or_title_active()) return;
    const auto candidate = *pendingBombBagSaveWarp_;
    const int bag = candidate.bag;
    const int item = dComIfGs_getItem(SLOT_15 + bag, false);
    const int count = dComIfGs_getBombNum(static_cast<u8>(bag));
    if (!save_warp_bag_converted(randomizer_active(), syncable_bomb_item(item),
            bag, candidate.item, dMeter2Info_getRentalBombBag(), item, count)) {
        svc_log->info(mod_ctx, "BOMB_BAG save-warp candidate did not become permanent");
        pendingBombBagSaveWarp_.reset();
        return;
    }
    if (transport_.send({{"type", "bomb_bag_slot"}, {"bag", bag},
                         {"item", item}, {"count", count}})) {
        svc_log->info(mod_ctx, ("BOMB_BAG save-warp published bag=" +
            std::to_string(bag) + " item=" + std::to_string(item) +
            " count=" + std::to_string(count)).c_str());
        pendingBombBagSaveWarp_.reset();
    }
}

void GameAdapter::notify_room_scene_initialized(int room) {
    const char* stage = dComIfGp_getStartStageName();
    if (stage == nullptr || stage[0] == '\0' || room < 0 || room >= 64) return;
    if (initializedStageName_ == stage && initializedRoom_ == room) return;
    initializedStageName_ = stage;
    initializedRoom_ = room;
    initializedRoomTicks_ = 0;
}

bool GameAdapter::request_manual_sync(std::string_view peerId, bool flagsOnly,
                                      std::string* error) {
    return request_manual_sync_impl(peerId, flagsOnly, {}, error);
}

bool GameAdapter::request_manual_sync_impl(std::string_view peerId, bool flagsOnly,
                                           std::string_view cueKey,
                                           std::string* error, bool trackStatus) {
    const net::Status status = transport_.status();
    const std::string target(peerId);
    const bool atTitle = fpcM_SearchByName(fpcNm_TITLE_e) != nullptr;
    if (!status.welcomed) {
        if (error != nullptr) *error = "Not connected.";
        return false;
    }
    if (!syncFlagsEnabled_) {
        if (error != nullptr) *error = "Sync flags is disabled.";
        return false;
    }
    if (target.empty() || transport_.peers().find(target) == transport_.peers().end()) {
        if (error != nullptr) *error = "Choose a connected peer to sync from.";
        return false;
    }
    if (trackStatus && manualSyncState_ == ManualSyncState::Waiting) {
        if (error != nullptr) *error = "A sync request is already pending.";
        return false;
    }
    nlohmann::json request = {
        {"type", "sync_request"}, {"target_client_id", target},
        {"manual_sync_mode", flagsOnly ? "flags" : "warp"},
    };
    uint64_t requestId = ++nextManualSyncRequestId_;
    if (requestId == 0) requestId = ++nextManualSyncRequestId_;
    request["manual_sync_request_id"] = requestId;
    if (!flagsOnly && !cueKey.empty()) request["cue_key"] = cueKey;
    if (!transport_.send_to(target, request)) {
        if (error != nullptr) *error = "Could not send the sync request.";
        return false;
    }
    awaitingManualSyncCueKey_ = flagsOnly ? std::string() : std::string(cueKey);
    awaitingManualSyncPeerId_ = flagsOnly ? std::string() : target;
    awaitingManualSyncRequestId_ = cueKey.empty() ? 0 : requestId;
    if (trackStatus) {
        manualSyncState_ = ManualSyncState::Waiting;
        manualSyncFlagsOnly_ = flagsOnly;
        manualSyncRequestedFromTitle_ = atTitle;
        manualSyncPeerId_ = target;
        manualSyncRequestId_ = requestId;
        manualSyncWaitTicks_ = 0;
        manualSyncTimedOut_ = false;
        svc_log->info(mod_ctx, ("Manual sync requested id=" + std::to_string(requestId) +
            " peer=" + target +
            " mode=" + (flagsOnly ? "flags" : "warp") +
            " origin=" + (atTitle ? "title" : "gameplay")).c_str());
    }
    return true;
}

bool GameAdapter::expecting_manual_sync_reply(std::string_view peerId,
                                               uint64_t requestId) const {
    return requestId != 0 &&
           ((manualSyncState_ == ManualSyncState::Waiting && manualSyncPeerId_ == peerId &&
             manualSyncRequestId_ == requestId) ||
            (!awaitingManualSyncCueKey_.empty() && awaitingManualSyncPeerId_ == peerId &&
             awaitingManualSyncRequestId_ == requestId));
}

bool GameAdapter::applying_remote() const { return applyingRemote_; }

void GameAdapter::maybe_queue_progression_switch_prompt(std::string_view peerId,
                                                        int stage, int flag) {
    if (peerId.empty() || !progressionPromptsEnabled_ || progressionPromptsSuppressed_) return;
    const auto queue = [&](std::string_view cueKey, std::string_view action,
                           std::string_view area) {
        const std::string guard = std::string(peerId) + ':' + std::string(cueKey);
        if (handledProgressionCues_.contains(guard)) return;
        for (const PendingProgressionCue& pending : pendingProgressionCues_) {
            if (pending.peerId == peerId && pending.cueKey == cueKey) return;
        }
        const auto nameIt = peerNames_.find(std::string(peerId));
        const std::string name = nameIt != peerNames_.end() ? nameIt->second :
                                                            std::string(peerId);
        pendingProgressionCues_.push_back({std::string(peerId), std::string(cueKey),
            name + ' ' + std::string(action) + ' ' + std::string(area),
            "Hold D-Pad Down to sync and reload", {}});
    };

    if (stage == kProgressionCueSewersStage && flag == kProgressionCueWakeUpInJailSwitch) {
        const std::string cueKey = "sewers_wake_up_in_jail";
        const std::string guard = std::string(peerId) + ':' + cueKey;
        if (!handledProgressionCues_.contains(guard) &&
            !(progressionPrompt_.active && progressionPrompt_.peerId == peerId &&
              progressionPrompt_.cueKey == cueKey)) {
            const auto nameIt = peerNames_.find(std::string(peerId));
            const std::string name = nameIt != peerNames_.end() ? nameIt->second :
                                                                std::string(peerId);
            progressionPrompt_ = {true, std::string(peerId), name, cueKey,
                name + " entered Hyrule Sewers",
                "Hold D-Pad Down to sync and reload", 0, 0, false};
        }
    } else if (stage == kProgressionCueHyruleFieldStage &&
               flag == kProgressionCueEldinTwilightSwitch) {
        queue("eldin_twilight_entered", "entered", "Eldin Twilight");
    } else if (stage == kProgressionCueHyruleFieldStage &&
               flag == kProgressionCueLanayruTwilightSwitch) {
        queue("lanayru_twilight_entered", "entered", "Lanayru Twilight");
    } else if (stage == kProgressionCueForestTempleStage &&
               flag == kProgressionCueForestTempleSavePromptSwitch) {
        queue("forest_temple_complete", "completed", "Forest Temple");
    } else if (stage == kProgressionCueGoronMinesStage &&
               flag == kProgressionCueGoronMinesSavePromptSwitch) {
        queue("goron_mines_complete", "completed", "Goron Mines");
    } else if (stage == kProgressionCueLakebedTempleStage &&
               flag == kProgressionCueLakebedTempleSavePromptSwitch) {
        queue("lakebed_temple_complete", "completed", "Lakebed Temple");
    } else if (stage == kProgressionCueArbitersGroundsStage &&
               flag == kProgressionCueArbitersGroundsSavePromptSwitch) {
        queue("arbiters_grounds_complete", "completed", "Arbiter's Grounds");
    } else if (stage == kProgressionCueSnowpeakRuinsStage &&
               flag == kProgressionCueSnowpeakRuinsSavePromptSwitch) {
        queue("snowpeak_ruins_complete", "completed", "Snowpeak Ruins");
    } else if (stage == kProgressionCueCityInTheSkyStage &&
               flag == kProgressionCueCityInTheSkySavePromptSwitch) {
        queue("city_in_the_sky_complete", "completed", "City in the Sky");
    } else if (stage == kProgressionCuePalaceOfTwilightStage &&
               flag == kProgressionCuePalaceOfTwilightSavePromptSwitch) {
        queue("palace_of_twilight_complete", "completed", "Palace of Twilight");
    }
}

void GameAdapter::maybe_queue_progression_event_prompt(std::string_view peerId,
                                                       uint16_t flag) {
    if (peerId.empty() || !progressionPromptsEnabled_ || progressionPromptsSuppressed_) return;
    auto queue = [&](std::string_view cueKey, std::string title,
                     std::string_view expectedStage = {}) {
        const std::string guard = std::string(peerId) + ':' + std::string(cueKey);
        if (handledProgressionCues_.contains(guard)) return;
        for (const PendingProgressionCue& pending : pendingProgressionCues_) {
            if (pending.peerId == peerId && pending.cueKey == cueKey) return;
        }
        pendingProgressionCues_.push_back({std::string(peerId), std::string(cueKey),
            std::move(title), "Hold D-Pad Down to sync and reload",
            std::string(expectedStage)});
    };
    const auto nameIt = peerNames_.find(std::string(peerId));
    const std::string name = nameIt != peerNames_.end() ? nameIt->second :
                                                        std::string(peerId);
    if (flag == kProgressionCueSewersCompleteEventBit) {
        queue("sewers_complete", name + " completed Sewers",
              kProgressionCueSewersCompleteDestStage);
    } else if (flag == kProgressionCueFaronTwilightEventBit) {
        queue("faron_twilight_entered", name + " entered Faron Twilight",
              kProgressionCueFaronTwilightDestStage);
    } else if (flag == 0x0610) {
        queue("faron_twilight_complete", name + " completed Faron Twilight");
    } else if (flag == 0x0708) {
        queue("eldin_twilight_complete", name + " completed Eldin Twilight");
    } else if (flag == 0x0C02) {
        queue("lanayru_twilight_complete", name + " completed Lanayru Twilight");
    } else if (flag == kProgressionCueTempleOfTimeClearEventBit) {
        queue("temple_of_time_complete", name + " completed Temple of Time",
              kProgressionCueTempleOfTimeExitStage);
    }
}

void GameAdapter::maybe_queue_progression_pose_prompt(
    std::string_view peerId, const dusk::multiplayer::PeerPoseSnapshot& pose) {
    if (peerId.empty() || !progressionPromptsEnabled_ || progressionPromptsSuppressed_) return;

    constexpr std::string_view cueKey = "final_ganondorf_entered";
    const std::string guard = std::string(peerId) + ':' + std::string(cueKey);
    const bool reachedFinalFight = pose.finalGanondorfReady ||
                                   pose.stage == kProgressionCueFinalGanondorfStage;
    if (!reachedFinalFight) {
        // Allow another prompt after this peer has genuinely left the final
        // encounter. D_MN09B is retained during the transition into readiness.
        if (pose.stage != "D_MN09B") shownPoseProgressionCues_.erase(guard);
        return;
    }
    if (!shownPoseProgressionCues_.insert(guard).second) return;
    dusklight_online::log_info("Ganondorf sync prompt queued from encounter readiness");

    const auto nameIt = peerNames_.find(std::string(peerId));
    const std::string name = nameIt != peerNames_.end() ? nameIt->second :
                                                         std::string(peerId);
    pendingProgressionCues_.push_back({std::string(peerId), std::string(cueKey),
        name + " reached the final Ganondorf fight",
        "Hold D-Pad Down to sync and reload", {}});
}

void GameAdapter::update_progression_prompts() {
    if (!progressionPromptsEnabled_ || progressionPromptsSuppressed_) {
        clear_progression_prompts();
        return;
    }

    auto peer_ready = [&](std::string_view peerId, const nlohmann::json** stateOut = nullptr) {
        const auto stateIt = peerProgressionStates_.find(std::string(peerId));
        const auto ageIt = peerProgressionAges_.find(std::string(peerId));
        if (stateOut != nullptr) {
            *stateOut = stateIt == peerProgressionStates_.end() ? nullptr : &stateIt->second;
        }
        return stateIt != peerProgressionStates_.end() &&
               ageIt != peerProgressionAges_.end() &&
               ageIt->second <= kProgressionStateReadyMaxAgeTicks &&
               stateIt->second.value("manual_sync_ready", false);
    };

    for (auto it = pendingProgressionCues_.begin(); it != pendingProgressionCues_.end();) {
        const nlohmann::json* state = nullptr;
        // Offering the encounter is not a warp. Its pose already proves the
        // destination; only accepting the request must wait for warp safety.
        const bool encounterPrompt = it->cueKey == "final_ganondorf_entered";
        if (progressionPrompt_.active &&
            (encounterPrompt || progressionPrompt_.cueKey == "final_ganondorf_entered") &&
            (progressionPrompt_.peerId != it->peerId || progressionPrompt_.cueKey != it->cueKey)) {
            ++it;
            continue;
        }
        const bool canOffer = encounterPrompt || peer_ready(it->peerId, &state);
        if (!canOffer ||
            (!it->expectedStage.empty() &&
             (state == nullptr || state->value("stage", std::string()) != it->expectedStage))) {
            ++it;
            continue;
        }
        // Do not consume/replace a different visible offer, or expire an
        // encounter invitation behind this client's title/load/cutscene.
        if (encounterPrompt && (!stage_ready() || opening_or_title_active() ||
            (progressionPrompt_.active && (progressionPrompt_.peerId != it->peerId ||
                                          progressionPrompt_.cueKey != it->cueKey)))) {
            ++it;
            continue;
        }
        const std::string guard = it->peerId + ':' + it->cueKey;
        if (!handledProgressionCues_.contains(guard) &&
            !(progressionPrompt_.active && progressionPrompt_.peerId == it->peerId &&
              progressionPrompt_.cueKey == it->cueKey)) {
            const auto nameIt = peerNames_.find(it->peerId);
            progressionPrompt_ = {true, it->peerId,
                nameIt != peerNames_.end() ? nameIt->second : it->peerId,
                it->cueKey, it->title, it->body, 0, 0, false};
            if (encounterPrompt)
                dusklight_online::log_info("Ganondorf sync prompt displayed");
        }
        it = pendingProgressionCues_.erase(it);
    }

    if (progressionPrompt_.active) {
        const bool encounterHidden = progressionPrompt_.cueKey == "final_ganondorf_entered" &&
                                    (!stage_ready() || opening_or_title_active());
        if (!encounterHidden && progressionPrompt_.ageTicks < std::numeric_limits<uint32_t>::max()) {
            ++progressionPrompt_.ageTicks;
        }
        if (!progressionPrompt_.waiting) {
            progressionPrompt_.holdTicks = !encounterHidden && progressionPromptAcceptHeld_ ?
                std::min(progressionPrompt_.holdTicks + 1, kProgressionPromptHoldTicks) : 0;
            if (progressionPrompt_.holdTicks >= kProgressionPromptHoldTicks) {
                pendingProgressionPeerId_ = progressionPrompt_.peerId;
                pendingProgressionCueKey_ = progressionPrompt_.cueKey;
                progressionPrompt_.title = "Waiting for sync...";
                progressionPrompt_.body = "Sync will begin when ready";
                progressionPrompt_.ageTicks = 0;
                progressionPrompt_.holdTicks = 0;
                progressionPrompt_.waiting = true;
            } else if (progressionPrompt_.ageTicks >= kProgressionPromptDurationTicks) {
                progressionPrompt_ = {};
            }
        }
    }

    if (!pendingProgressionPeerId_.empty() && stage_ready() &&
        peer_ready(pendingProgressionPeerId_)) {
        std::string error;
        if (request_manual_sync_impl(pendingProgressionPeerId_, false,
                                     pendingProgressionCueKey_, &error, false)) {
            handledProgressionCues_.insert(pendingProgressionPeerId_ + ':' +
                                           pendingProgressionCueKey_);
            pendingProgressionPeerId_.clear();
            pendingProgressionCueKey_.clear();
            progressionPrompt_ = {};
        } else if (!error.empty()) {
            lastError_ = error;
        }
    }
}

void GameAdapter::set_progression_prompts_enabled(bool enabled) {
    progressionPromptsEnabled_ = enabled;
    if (!enabled) clear_progression_prompts();
}

void GameAdapter::suppress_progression_prompts(bool suppress) {
    progressionPromptsSuppressed_ = suppress;
    if (suppress) clear_progression_prompts();
}

void GameAdapter::clear_progression_prompts() {
    progressionPrompt_ = {};
    progressionPromptAcceptHeld_ = false;
    pendingProgressionCues_.clear();
    pendingProgressionPeerId_.clear();
    pendingProgressionCueKey_.clear();
}

void GameAdapter::consume_progression_prompt_input() {
    progressionPromptAcceptHeld_ = false;
    if (!progressionPromptsEnabled_ || progressionPromptsSuppressed_ || !progressionPrompt_.active) {
        return;
    }

    interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(PAD_1);
    progressionPromptAcceptHeld_ = (pad.mButtonFlags & PAD_BUTTON_DOWN) != 0;
    pad.mButtonFlags &= ~PAD_BUTTON_DOWN;
    pad.mPressedButtonFlags &= ~PAD_BUTTON_DOWN;
}

void GameAdapter::clear_disabled_sync_flags_state() {
    reset_floor_switch_state();
    ooccooState_.reset();
    ooccooBoundToSave_ = false;
    ooccooCatchupPending_ = true;
    ooccooReplyPending_ = false;
    deferredSwitches_.clear();
    deferredFaronInbound_.clear();
    pendingDarkClears_.fill(0);
    deferredStoryEvents_.clear();
    deferredLocalEvents_.clear();
    faronDayBroadcastHoldTicks_ = 0;
    localFaronCageSequenceActive_ = false;
    localFaronWarpSequenceActive_ = false;
    pendingOrdonEventBits_.clear();
    ordonReloadSafeTicks_ = 0;
    ordonReloadWaitTicks_ = 0;
    ordonReloadTransitionActive_ = false;
    ordonReloadSawStageLoad_ = false;
    mirrorReloadPending_ = false;
    zoraThawPending_ = nlohmann::json();
    pendingManualInfo_.clear();
    pendingManualFlagsSave_.clear();
    pendingManualVibration_.reset();
    manualTransitionActive_ = false;
    manualReloadPending_ = false;
    progressionPrompt_ = {};
    progressionPromptAcceptHeld_ = false;
    pendingProgressionCues_.clear();
    pendingProgressionPeerId_.clear();
    pendingProgressionCueKey_.clear();
    awaitingManualSyncCueKey_.clear();
    awaitingManualSyncPeerId_.clear();
    awaitingManualSyncRequestId_ = 0;
    pendingSyncReplies_.clear();
    if (manualSyncState_ == ManualSyncState::Waiting) manualSyncState_ = ManualSyncState::Failed;
    manualSyncFlagsOnly_ = false;
    manualSyncRequestedFromTitle_ = false;
    manualSyncPeerId_.clear();
    manualSyncRequestId_ = 0;
    manualSyncWaitTicks_ = 0;
    manualSyncTimedOut_ = false;
    peerProgressionStates_.clear();
    peerProgressionAges_.clear();
    deferredFaronInbound_.clear();
    progressionTicks_ = 0;
    lastLocalTboxStage_ = -1;
    lastLocalTboxFlag_ = -1;
    lastLocalTboxAt_ = {};
    pendingRupeePublicationToSuppress_.reset();
    pendingMaxLifePublicationToSuppress_.reset();
    // Mutations made while sharing is disabled are local-only. Re-enabling
    // must establish a fresh baseline rather than replaying them as pickups.
    localObservedState_ = nlohmann::json();
    observedRupees_.reset();
}

void GameAdapter::update(bool syncFlagsEnabled, bool syncWorldEnabled, bool remoteModelEnabled,
                         bool nameLabelsEnabled, bool displayMidnaEnabled,
                         bool matrixStreamingEnabled,
                         bool remoteCollisionEnabled,
                         bool pvpEnabled, bool playerListEnabled) {
    coop::tick();
    pvpLocalHitContactsThisUpdate_.clear();
    daAlink_c* localLink = daAlink_getAlinkActorClass();
    fopAc_ac_c* rideActor = localLink != nullptr ? localLink->mRideAcKeep.getActor() : nullptr;
    const bool ownsSpinner = rideActor != nullptr &&
        fopAcM_GetName(rideActor) == fpcNm_SPINNER_e &&
        localLink->checkSpinnerRideOwn(rideActor);
    const uintptr_t spinnerActor = ownsSpinner ? reinterpret_cast<uintptr_t>(rideActor) : 0;
    const bool spinnerAttackActive = ownsSpinner &&
        static_cast<daSpinner_c*>(rideActor)->reflectAccept();
    if (spinnerActor != pvpSpinnerActor_ || !spinnerAttackActive) {
        pvpSpinnerAttackTargets_.clear();
    }
    pvpSpinnerActor_ = spinnerActor;
    pvpSpinnerAttackActive_ = spinnerAttackActive;
    const bool syncFlagsWereEnabled = syncFlagsEnabled_;
    syncFlagsEnabled_ = syncFlagsEnabled;
    syncWorldEnabled_ = syncWorldEnabled;
    observe_bomb_bag_rental();
    const bool randomizerActive = randomizer_active();
    set_randomizer_audio_filter(randomizerActive);
    sWorldSyncEnabled = syncWorldEnabled && transport_.status().welcomed;
    if (!syncFlagsEnabled_) {
        if (syncFlagsWereEnabled) clear_disabled_sync_flags_state();
    } else {
        if (!syncFlagsWereEnabled) localObservedState_ = nlohmann::json();
        if (manualSyncState_ == ManualSyncState::Waiting &&
            ++manualSyncWaitTicks_ >= kManualSyncRequestTimeoutTicks) {
            manualSyncState_ = ManualSyncState::Failed;
            manualSyncTimedOut_ = true;
            manualSyncRequestedFromTitle_ = false;
            svc_log->warn(mod_ctx,
                ("Manual sync timed out peer=" + manualSyncPeerId_).c_str());
        }
        tick_manual_transition();
        update_pending_sync_replies();
    }
    if (ordonReloadTransitionActive_) {
        if (!engine_stage_ready()) {
            ordonReloadSawStageLoad_ = true;
        } else if (ordonReloadSawStageLoad_) {
            ordonReloadTransitionActive_ = false;
            ordonReloadSawStageLoad_ = false;
        }
    }
    const net::Status status = transport_.status();
    // Hidden Village has two overlapping field exits. Only the exit gated by
    // the pendant event sets Impaz's house switch; a Dusk warp bypasses both.
    // Commit the same durable switch when a completed quest leaves the village
    // and send it through the existing peer switch path before stage unload.
    if (syncFlagsEnabled_ && status.welcomed && !randomizerActive &&
        dComIfGp_isEnableNextStage()) {
        const char* currentStage = dComIfGp_getStartStageName();
        const char* nextStage = dComIfGp_getNextStageName();
        if (currentStage != nullptr && std::strcmp(currentStage, "F_SP128") == 0 &&
            nextStage != nullptr && nextStage[0] != '\0' &&
            std::strcmp(nextStage, "F_SP128") != 0 &&
            std::strcmp(nextStage, "R_SP128") != 0 &&
            std::strcmp(nextStage, kTitleDemoStage.data()) != 0 &&
            dComIfGs_isEventBit(dSv_event_flag_c::saveBitLabels[0x116]) &&
            !dComIfGs_isSaveSwitch(0x61)) {
            {
                RemoteApplicationGuard applying(applyingRemote_);
                dComIfGs_onStageSwitch(dStage_SaveTbl_ELDIN, 0x61);
            }
            publish_local({{"type", "switch_bit"}, {"stage", dStage_SaveTbl_ELDIN},
                           {"flag", 0x61}, {"set", true}});
        }
    }
    sample_live_field_map_marker();
    if (!opening_or_title_active() && dComIfGp_getStageStagInfo() != nullptr) {
        (void)local_wagon_escort_unfinished();
    }
    if (!status.welcomed) clear_local_audio_events();
    set_bomb_sync_enabled(status.welcomed && syncWorldEnabled);
    for (auto& [peerId, age] : peerProgressionAges_) {
        (void)peerId;
        if (age < std::numeric_limits<uint32_t>::max()) ++age;
    }
    if (syncFlagsEnabled_) update_progression_prompts();
    ProgressionPromptView promptView;
    promptView.active = progressionPromptsEnabled_ && !progressionPromptsSuppressed_ &&
                        progressionPrompt_.active;
    promptView.waiting = progressionPrompt_.waiting;
    promptView.title = progressionPrompt_.title;
    promptView.body = progressionPrompt_.body;
    promptView.ageSeconds = static_cast<float>(progressionPrompt_.ageTicks) / 30.0f;
    promptView.remainingRatio = progressionPrompt_.active && !progressionPrompt_.waiting ?
        std::clamp(float(kProgressionPromptDurationTicks -
                         std::min(progressionPrompt_.ageTicks,
                                  kProgressionPromptDurationTicks)) /
                       float(kProgressionPromptDurationTicks), 0.0f, 1.0f) : 0.0f;
    promptView.holdRatio = progressionPrompt_.active && !progressionPrompt_.waiting ?
        std::clamp(float(progressionPrompt_.holdTicks) /
                       float(kProgressionPromptHoldTicks), 0.0f, 1.0f) : 0.0f;
    const bool remoteGameplayReady = remote_link_gameplay_ready(
        manualTransitionActive_ || ordonReloadTransitionActive_);
    // A direct host is already an active session while listening, before any
    // peer has completed the welcome handshake. Passive local UI such as the
    // player list should work in that state; remote visuals remain separately
    // guarded by remoteGameplayReady.
    std::map<std::string, PlayerLocationView> playerLocations;
    for (const auto& [peerId, name] : transport_.peers()) {
        (void)name;
        const auto presence = peerPresence_.find(peerId);
        if (presence == peerPresence_.end()) continue;
        const auto stage = presence->second.find("stage");
        const auto room = presence->second.find("room");
        if (stage == presence->second.end() || !stage->is_string() ||
            room == presence->second.end() || !room->is_number_integer()) continue;
        const int64_t roomNumber = room->get<int64_t>();
        if (roomNumber < -128 || roomNumber > 127) continue;
        PlayerLocationView location{stage->get<std::string>(), static_cast<int>(roomNumber)};
        const auto fieldMap = presence->second.find("field_map");
        if (fieldMap != presence->second.end() && fieldMap->is_object()) {
            const auto mapStage = fieldMap->find("stage");
            const auto region = fieldMap->find("region");
            const auto x = fieldMap->find("x");
            const auto z = fieldMap->find("z");
            const auto angle = fieldMap->find("angle_y");
            if (mapStage != fieldMap->end() && mapStage->is_string() &&
                region != fieldMap->end() && region->is_number_integer() &&
                x != fieldMap->end() && x->is_number() &&
                z != fieldMap->end() && z->is_number() &&
                angle != fieldMap->end() && angle->is_number_integer()) {
                const std::string mapStageName = mapStage->get<std::string>();
                const int64_t mapRegion = region->get<int64_t>();
                const float mapX = x->get<float>();
                const float mapZ = z->get<float>();
                const int64_t mapAngle = angle->get<int64_t>();
                if (!mapStageName.empty() && mapStageName.size() < 8 &&
                    mapRegion >= 1 && mapRegion <= 8 &&
                    std::isfinite(mapX) && std::isfinite(mapZ) &&
                    std::fabs(mapX) < 10000000.0f && std::fabs(mapZ) < 10000000.0f &&
                    mapAngle >= -32768 && mapAngle <= 32767) {
                    location.fieldMapMarker = PlayerLocationView::FieldMapMarker{
                        mapStageName, static_cast<int>(mapRegion), mapX, mapZ,
                        static_cast<int>(mapAngle)};
                }
            }
        }
        playerLocations.emplace(peerId, std::move(location));
    }
    const bool chatAvailable = status.enabled &&
        (status.mode == net::Mode::DirectHost ?
            (status.state == net::State::Listening || status.state == net::State::Connected) :
            status.welcomed);
    std::map<std::string, uint32_t> playerLatencies;
    if (playerListEnabled) {
        for (const auto& [peerId, name] : transport_.peers()) {
            (void)name;
            if (auto latency = peer_latency_ms(peerId))
                playerLatencies.emplace(peerId, *latency);
        }
    }
    update_visual_overlays(status.enabled, chatAvailable, remoteGameplayReady, nameLabelsEnabled,
                           remoteModelEnabled, playerListEnabled, status.room,
                           (status.mode == net::Mode::DirectHost || status.isOwner) ?
                               "hosting" : "connected",
                           status.name,
                           peerPoses_, transport_.peers(), playerLocations, playerLatencies,
                           promptView);
    dusk::multiplayer::set_remote_actor_options(
                                                dusk::multiplayer::kRemoteMidnaStreamingEnabled &&
                                                    displayMidnaEnabled,
                                                status.welcomed && syncWorldEnabled &&
                                                status.settings.syncWorld,
                                                remoteCollisionEnabled && !dComIfGp_event_runCheck(),
                                                pvpEnabled,
                                                !matrixStreamingEnabled);
    for (auto& [peerId, pose] : peerPoses_) {
        (void)peerId;
        if (pose.valid && pose.ageTicks < std::numeric_limits<uint32_t>::max()) ++pose.ageTicks;
    }
    const auto timingNow = std::chrono::steady_clock::now();
    if (poseTimingUpdate_.time_since_epoch().count() != 0) {
        poseTimingMaxUpdateMs_ = std::max(poseTimingMaxUpdateMs_,
            std::chrono::duration<double, std::milli>(timingNow - poseTimingUpdate_).count());
    }
    poseTimingUpdate_ = timingNow;
    const bool visualReceiveActive = status.welcomed && remoteModelEnabled;
    if (!visualReceiveActive) {
        peerPoses_.clear();
        posePlayback_.clear();
        presentedPoses_.clear();
    }
    if (visualReceiveActive && !remoteGameplayReady) {
        // Do not replay pre-load poses after the local scene becomes ready.
        posePlayback_.clear();
        presentedPoses_.clear();
    }
    if (visualReceiveActive && remoteGameplayReady) {
        for (auto& [peerId, playback] : posePlayback_) {
            const auto predictor = [](const auto& before, const auto& latest, int64_t ahead, auto& out) {
                if (latest.procId != daAlink_c::PROC_MOVE &&
                    latest.procId != daAlink_c::PROC_WOLF_MOVE) return false;
                return predict_pose(before, latest, ahead, out);
            };
            if (auto next = playback.update(predictor)) {
                if (++posePresentationSequence_ == 0) ++posePresentationSequence_;
                next->presentationSequence = posePresentationSequence_;
                presentedPoses_[peerId] = std::move(*next);
                if (playback.predicted()) ++poseTiming_[peerId].predicted;
            }
        }
        for (auto& [peerId, pose] : presentedPoses_) {
            if (pose.ageTicks < std::numeric_limits<uint32_t>::max()) ++pose.ageTicks;
            const auto latest = peerPoses_.find(peerId);
            if (latest != peerPoses_.end()) {
                pose.ageTicks = std::max(pose.ageTicks, latest->second.ageTicks);
            }
        }
        if (randomizerActive) {
            // Foolish Item plays this level sound locally when rando_item_get is
            // applied. Reject a streamed copy as well, including packets from
            // older peers that do not have the sender-side filter.
            const uint32_t foolishSound =
                static_cast<uint32_t>(Z2SE_WL_V_LAND_DAMAGE);
            for (auto& [peerId, pose] : presentedPoses_) {
                (void)peerId;
                const auto isFoolishSound = [foolishSound](const auto& event) {
                    return event.soundId == foolishSound;
                };
                std::erase_if(pose.audioEvents, isFoolishSound);
                std::erase_if(pose.activeAudioEvents, isFoolishSound);
            }
        }
        if (visual_wire_trace_enabled()) {
            ++poseTimingTicks_;
            for (auto& [peerId, timing] : poseTiming_) {
                timing.burst = std::max(timing.burst, timing.pending);
                timing.pending = 0;
                const auto shown = presentedPoses_.find(peerId);
                if (shown != presentedPoses_.end()) {
                    const auto seq = shown->second.sequence;
                    if (seq == timing.displayedSequence) ++timing.repeats;
                    else if (timing.displayedSequence && seq > timing.displayedSequence + 1)
                        timing.skipped += seq - timing.displayedSequence - 1;
                    timing.displayedSequence = seq;
                }
                if (poseTimingTicks_ % 150 == 0) {
                    std::ostringstream line;
                    line << "POSE_TIMING peer=" << peerId
                         << " rx=" << timing.packets << " rx_missing=" << timing.missing
                         << " rx_max_gap_ms=" << timing.maxReceiveGapMs
                         << " max_rx_per_tick=" << timing.burst
                         << " playback_repeats=" << timing.repeats
                         << " playback_skipped=" << timing.skipped
                         << " queue=" << posePlayback_[peerId].size()
                         << " buffer_ticks=" << posePlayback_[peerId].delay_ticks()
                         << " predicted_ticks=" << timing.predicted
                         << " update_max_gap_ms=" << poseTimingMaxUpdateMs_;
                    dusklight_online::log_info(line.str());
                    timing.packets = timing.missing = timing.repeats = timing.skipped = timing.burst = timing.predicted = 0;
                    timing.maxReceiveGapMs = 0;
                }
            }
            if (poseTimingTicks_ % 150 == 0) poseTimingMaxUpdateMs_ = 0;
        }
        dusk::multiplayer::sync_remote_link_actor_dummies(presentedPoses_);
        // presentedPoses_ retains the displayed snapshot between playback steps. Active
        // level sounds are a per-snapshot refresh, so consuming them once lets
        // the actor's existing timeout stop a sound when packets cease instead
        // of refreshing a stale event forever on every game tick.
        for (auto& [peerId, pose] : presentedPoses_) {
            (void)peerId;
            pose.activeAudioEvents.clear();
        }
    } else if (!visualReceiveActive) {
        // Disconnecting or disabling remote models is owned by the mod and
        // requires explicit cleanup. During room/stage transitions, however,
        // the scene owns actor teardown; touching the same actors here races
        // the process deletion list (the recurring Ordon/load crash family).
        dusk::multiplayer::destroy_all_remote_link_dummies();
    }
    // The remote-model option controls what this client receives/renders. It
    // must not stop this client's own pose/audio stream: other peers can still
    // have their models enabled and need our state.
    if (status.welcomed) {
        const net::udp::PacketType activePoseType = matrixStreamingEnabled ?
            net::udp::PacketType::PoseMsgpack :
            net::udp::PacketType::SemanticPoseMsgpack;
        // Both visual representations are deliberately sampled every game tick.
        // Semantic rendering is the normal bandwidth-efficient representation;
        // matrix streaming is an explicit developer comparison mode.
        nlohmann::json poseMessage;
        LocalPoseDiagnostics poseDiagnostics;
        const uint32_t nextSequence = localPoseSequence_ + 1;
        if (build_local_pose(nextSequence, stage_ready() && !manualTransitionActive_,
                             matrixStreamingEnabled, poseMessage,
                             &poseDiagnostics)) {
                if (transport_.send_visual(poseMessage, activePoseType)) {
                    localPoseSequence_ = nextSequence;
                    const net::VisualSendStats wireStats =
                        transport_.last_visual_send_stats();
                    if (visual_wire_trace_enabled() && wireStats.sequence == nextSequence &&
                        wireStats.recipients > 0) {
                        const char* matrixScope = "none";
                        if (poseDiagnostics.matrixScope == LocalPoseMatrixScope::Attachments) {
                            matrixScope = "attachments";
                        } else if (poseDiagnostics.matrixScope ==
                                   LocalPoseMatrixScope::FullBody) {
                            matrixScope = "full_body";
                        }
                        if (sVisualWireTrace.samples == 0 ||
                            sVisualWireTrace.scope != poseDiagnostics.matrixScope) {
                            sVisualWireTrace = {};
                            sVisualWireTrace.scope = poseDiagnostics.matrixScope;
                        }
                        const uint64_t normalizedBytes =
                            wireStats.wireBytes / wireStats.recipients;
                        ++sVisualWireTrace.samples;
                        sVisualWireTrace.normalizedWireBytes += normalizedBytes;
                        sVisualWireTrace.peakNormalizedWireBytes = std::max(
                            sVisualWireTrace.peakNormalizedWireBytes, normalizedBytes);
                        sVisualWireTrace.fullMsgpackBytes +=
                            wireStats.fullMsgpackBytes / wireStats.recipients;
                        sVisualWireTrace.preparedMsgpackBytes +=
                            wireStats.preparedMsgpackBytes / wireStats.recipients;
                        sVisualWireTrace.deltaSamples +=
                            wireStats.snapshotDeltas > 0 ? 1 : 0;
                        sVisualWireTrace.fullSamples +=
                            wireStats.snapshotFulls > 0 ? 1 : 0;
                        const uint64_t normalizedFullSnapshotWireBytes =
                            wireStats.fullSnapshotWireBytes / wireStats.recipients;
                        sVisualWireTrace.fullSnapshotWireBytes +=
                            normalizedFullSnapshotWireBytes;
                        const uint64_t normalizedPreparedBytes =
                            wireStats.preparedMsgpackBytes / wireStats.recipients;
                        const int preparedSizeBand = normalizedPreparedBytes <= 128 ? 0 :
                            (normalizedPreparedBytes >= 500 ? 2 : 1);
                        sVisualWireTrace.preparedSizeBand = preparedSizeBand;
                        if (sVisualWireTrace.samples <= 5 ||
                            (sVisualWireTrace.samples % 300) == 0) {
                            std::ostringstream line;
                            line << "VISUAL_WIRE_TX seq=" << nextSequence
                                 << " udp_type=" << static_cast<int>(activePoseType)
                                 << " matrix_scope=" << matrixScope
                                 << " matrix_slots="
                                 << static_cast<int>(poseDiagnostics.matrixPresentSlots)
                                 << " matrix_packed=" << poseDiagnostics.matrixPackedBytes
                                 << " msgpack="
                                 << nlohmann::json::to_msgpack(poseMessage).size()
                                 << " recipients=" << wireStats.recipients
                                 << " datagrams=" << wireStats.datagrams
                                 << " wire_bytes=" << wireStats.wireBytes
                                 << " wire_per_recipient=" << normalizedBytes
                                 << " avg_wire_per_recipient="
                                 << (sVisualWireTrace.normalizedWireBytes /
                                     sVisualWireTrace.samples)
                                 << " peak_wire_per_recipient="
                                 << sVisualWireTrace.peakNormalizedWireBytes
                                 << " snapshot_delta="
                                 << (wireStats.snapshotDeltas > 0 ? 1 : 0)
                                 << " snapshot_base=" << wireStats.snapshotBaseline
                                 << " snapshot_base_age="
                                 << (wireStats.snapshotBaseline != 0 &&
                                             nextSequence > wireStats.snapshotBaseline ?
                                         nextSequence - wireStats.snapshotBaseline : 0)
                                 << " snapshot_decision=" << wireStats.snapshotDecision
                                 << " full_msgpack=" << wireStats.fullMsgpackBytes
                                 << " prepared_msgpack=" << wireStats.preparedMsgpackBytes
                                 << " full_snapshot_prepared_msgpack="
                                 << wireStats.fullSnapshotPreparedMsgpackBytes
                                 << " full_snapshot_wire_bytes="
                                 << wireStats.fullSnapshotWireBytes
                                 << " full_snapshot_wire_per_recipient="
                                 << normalizedFullSnapshotWireBytes
                                 << " exact_wire_saved="
                                 << (normalizedFullSnapshotWireBytes > normalizedBytes ?
                                         normalizedFullSnapshotWireBytes - normalizedBytes : 0)
                                 << " changed_count="
                                 << wireStats.snapshotChangedKeys.size()
                                 << " changed_keys="
                                 << visual_wire_trace_keys(wireStats.snapshotChangedKeys)
                                 << " baseline_equal_count="
                                 << wireStats.snapshotUnchangedKeys.size()
                                 << " baseline_equal_keys="
                                 << visual_wire_trace_keys(wireStats.snapshotUnchangedKeys)
                                 << " removed_count="
                                 << wireStats.snapshotRemovedKeys.size()
                                 << " removed_keys="
                                 << visual_wire_trace_keys(wireStats.snapshotRemovedKeys)
                                 << " avg_full_msgpack="
                                 << (sVisualWireTrace.fullMsgpackBytes /
                                     sVisualWireTrace.samples)
                                 << " avg_prepared_msgpack="
                                 << (sVisualWireTrace.preparedMsgpackBytes /
                                     sVisualWireTrace.samples)
                                 << " avg_full_snapshot_wire_per_recipient="
                                 << (sVisualWireTrace.fullSnapshotWireBytes /
                                     sVisualWireTrace.samples)
                                 << " delta_samples=" << sVisualWireTrace.deltaSamples
                                 << " full_samples=" << sVisualWireTrace.fullSamples
                                 << " samples=" << sVisualWireTrace.samples
                                 << " send_interval=1";
                            dusklight_online::log_info(line.str());
                        }
                }
            }
        }
    } else {
        dusk::multiplayer::destroy_all_remote_link_dummies();
    }
    update_remote_tbox_animations(stage_ready() && !opening_or_title_active());
    if (!status.welcomed) {
        progressionTicks_ = 0;
        presenceTicks_ = 0;
        return;
    }

    if (syncFlagsEnabled) {
        update_local_faron_cage_sequence_state();
        // As in AIO, the broadcast fence elapses during cutscenes/loading.
        // Only its clock runs here; story application and reloads below
        // still require stage readiness and their participant safety checks.
        if (faronDayBroadcastHoldTicks_ > 0) --faronDayBroadcastHoldTicks_;
    }
    // AIO releases network/story fences during demos too. Stage mutations
    // remain gated individually; room reloads stay in the ready block below.
    if (syncFlagsEnabled && !opening_or_title_active()) flush_story_messages();
    const char* stage = dComIfGp_getStartStageName();
    const bool hasStage = stage != nullptr && stage[0] != '\0';
    if (syncFlagsEnabled && hasStage && stage_ready() && !opening_or_title_active()) {
        const int room = static_cast<int>(dComIfGp_roomControl_getStayNo());
        if (stableStageName_ == stage && stableRoom_ == room) {
            if (stableRoomTicks_ < std::numeric_limits<uint32_t>::max()) ++stableRoomTicks_;
            if (initializedStageName_ == stage && initializedRoom_ == room &&
                initializedRoomTicks_ < kRemoteSwitchRoomInitTicks) {
                ++initializedRoomTicks_;
            }
        } else {
            stableStageName_ = stage;
            stableRoom_ = room;
            stableRoomTicks_ = 0;
            initializedStageName_.clear();
            initializedRoom_ = -128;
            initializedRoomTicks_ = 0;
        }
        flush_deferred_switches();
        flush_pending_dark_clears();
        flush_story_events();
        poll_local_state(true);
        flush_bomb_bag_save_warp_candidate();
        apply_shared_ooccoo_local_form();
        flush_ooccoo_catchup();
        if (++collectibleRepairTicks_ >= 60) {
            collectibleRepairTicks_ = 0;
            reapply_observed_memory_items_for_current_stage();
            repair_current_stage_collectibles();
        }
    } else {
        collectibleRepairTicks_ = 0;
        stableStageName_.clear();
        stableRoom_ = -128;
        stableRoomTicks_ = 0;
        initializedStageName_.clear();
        initializedRoom_ = -128;
        initializedRoomTicks_ = 0;
    }
    if (syncFlagsEnabled) send_progression_state(false);

    if (++presenceTicks_ >= 30) {
        presenceTicks_ = 0;
        publish_player_color();
        transport_.send({{"type", "ping"}});
    }
}

void GameAdapter::capture_local_mutations_before_remote(bool syncFlagsEnabled) {
    // Native contact capture precedes packet ingestion. Never echo injected
    // peer pressure or enqueue transient holds behind stage readiness.
    if (syncFlagsEnabled) flush_floor_switch_state();
    else reset_floor_switch_state();
    const net::Status status = transport_.status();
    // The AIO hooked the inline wallet setter itself. Observe direct writes
    // here as well as queued meter changes, before incoming state can hide
    // them. Do not broadcast save loading/reset values or local-only changes.
    if (!status.welcomed || !syncFlagsEnabled || opening_or_title_active() ||
        manualTransitionActive_ || dComIfGp_getStageStagInfo() == nullptr) {
        observedRupees_.reset();
    } else {
        const int value = dComIfGs_getRupee();
        if (observedRupees_ && *observedRupees_ != value)
            notify_local_rupees(*observedRupees_, value);
        observedRupees_ = value;
    }
    if (!status.welcomed || !syncFlagsEnabled || !stage_ready() ||
        opening_or_title_active()) {
        return;
    }
    poll_local_state(true);
}

bool GameAdapter::stage_ready() const {
    return engine_stage_ready() && !manualTransitionActive_;
}

bool GameAdapter::engine_stage_ready() const {
    return dComIfGp_getStageStagInfo() != nullptr && !dComIfGp_event_runCheck() &&
           !dComIfGp_isEnableNextStage() && !fopOvlpM_IsPeek() && !fopOvlpM_IsDoingReq();
}

bool GameAdapter::allow_stage_unready(const RoutedMessage& message) const {
    return fpcM_SearchByName(fpcNm_TITLE_e) != nullptr &&
           message.payload.value("type", std::string()) == "save_snapshot" &&
           message.payload.value("manual_sync", false) &&
           message.payload.contains("full_state") &&
           message.payload.value("manual_sync_mode", "warp") != "flags" &&
           manualSyncRequestedFromTitle_ &&
           manualSyncState_ == ManualSyncState::Waiting &&
           manualSyncPeerId_ == message.peerId &&
           manualSyncRequestId_ == manual_sync_request_id(message.payload);
}

bool GameAdapter::discard_stage_message(const RoutedMessage& message) const {
    if (message.payload.value("type", std::string()) != "save_snapshot" ||
        !message.payload.value("manual_sync", false) ||
        !message.payload.contains("full_state")) return false;
    const uint64_t requestId = manual_sync_request_id(message.payload);
    if (!expecting_manual_sync_reply(message.peerId, requestId)) {
        svc_log->warn(mod_ctx,
            ("Discarded expired manual sync snapshot id=" +
             std::to_string(requestId) + " peer=" + message.peerId).c_str());
        return true;
    }
    return false;
}

bool GameAdapter::retain_for_save_recovery(const RoutedMessage& message) {
    if (!saveRecoveryEnabled_ || !saveRecoveryPolicyReady_ || !saveRecovery_ ||
        !syncFlagsEnabled_ || !activeSaveSlot_ ||
        saveRecovery_->slot != *activeSaveSlot_ ||
        saveRecovery_->saveName != activeSaveName_ ||
        message.receivedAt < saveRecovery_->savedAt ||
        message.receivedAt >= saveRecovery_->captureUntil ||
        (!opening_or_title_active() && !saveRecovery_->restorePending) ||
        randomizer_active()) return false;

    const std::string type = message.payload.value("type", std::string());
    // Requests and live room effects are not save facts. Currency deltas are
    // non-idempotent; their absolute companion is recoverable.
    if (type == "sync_request" || type == "floor_switch_state" ||
        type == "room_actor_action" || type == "web_timer" ||
        type == "room_switch_bit" || type == "rupee_delta" ||
        (type == "save_snapshot" && message.payload.value("manual_sync", false)))
        return false;

    constexpr size_t kMaxTitleMessages = 32768;
    constexpr size_t kMaxTitleBytes = 8U * 1024U * 1024U;
    size_t bytes = 0;
    try {
        bytes = message.payload.dump().size();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (saveRecovery_->titleMessages.size() >= kMaxTitleMessages ||
        bytes > kMaxTitleBytes - saveRecovery_->titleMessageBytes) {
        svc_log->warn(mod_ctx, "Save recovery title inbox overflowed; discarding journal");
        saveRecovery_.reset();
        return true;
    }
    saveRecovery_->titleMessageBytes += bytes;
    saveRecovery_->titleMessages.push_back(message);
    svc_log->info(mod_ctx, ("Save recovery retained title message type=" + type +
        " count=" + std::to_string(saveRecovery_->titleMessages.size())).c_str());
    return true;
}

ApplyResult GameAdapter::consume(const RoutedMessage& message) {
    try {
        const std::string type = message.payload.at("type").get<std::string>();
        switch (message.spec.domain) {
        case MessageDomain::Session:
            if (type == "welcome") {
                consume_welcome_membership(message.payload);
                const bool enabled = message.ingress.settings.syncFlags;
                if (syncFlagsEnabled_ && !enabled) clear_disabled_sync_flags_state();
                if (!syncFlagsEnabled_ && enabled) localObservedState_ = nlohmann::json();
                syncFlagsEnabled_ = enabled;
            }
            return ApplyResult::Applied;
        case MessageDomain::Membership:
            if (type == "sync_flags" || type == "room_settings") {
                const bool enabled = message.ingress.settings.syncFlags;
                // Apply this edge now, in wire order. Waiting until the end of
                // the receive batch lets false->event->true retain mutations
                // and fails to clear old specialized queues at the false edge.
                if (syncFlagsEnabled_ && !enabled) clear_disabled_sync_flags_state();
                if (!syncFlagsEnabled_ && enabled) localObservedState_ = nlohmann::json();
                syncFlagsEnabled_ = enabled;
            }
            return ApplyResult::Applied;
        case MessageDomain::Presence:
            if (type == "presence") {
                observe_latency_presence(message);
                const auto field = message.payload.find("player_color");
                if (field != message.payload.end()) {
                    if (!field->is_string()) return reject("invalid player colour");
                    const auto color = appearance::parse_color(field->get_ref<const std::string&>());
                    if (!color) return reject("invalid player colour");
                    // Older presence packets supplied a single colour for both.
                    auto outfit = color;
                    if (const auto field = message.payload.find("outfit_color");
                        field != message.payload.end()) {
                        if (!field->is_string()) return reject("invalid outfit colour");
                        outfit = appearance::parse_color(field->get_ref<const std::string&>());
                        if (!outfit) return reject("invalid outfit colour");
                    }
                    // Older clients sent one outfit colour for both garments.
                    auto zora = outfit;
                    if (const auto field = message.payload.find("zora_color");
                        field != message.payload.end()) {
                        if (!field->is_string()) return reject("invalid Zora Armour colour");
                        zora = appearance::parse_color(field->get_ref<const std::string&>());
                        if (!zora) return reject("invalid Zora Armour colour");
                    }
                    appearance::set_peer(message.peerId, *color, *outfit, *zora);
                }
            }
            if (type == "progression_state") {
                peerProgressionStates_[message.peerId] = message.payload;
                peerProgressionAges_[message.peerId] = 0;
                // Encounter invitations must also work when the entering
                // player's visual stream is unavailable (for example demos).
                // Direct-host reliable messages are attributed to "direct"
                // by Transport, exactly like their actor-state messages.
                if (message.payload.contains("final_ganondorf_ready")) {
                    dusk::multiplayer::PeerPoseSnapshot readiness;
                    readiness.stage = message.payload.value("stage", std::string());
                    readiness.finalGanondorfReady =
                        message.payload.value("final_ganondorf_ready", false);
                    maybe_queue_progression_pose_prompt(message.peerId, readiness);
                }
            } else peerPresence_[message.peerId] = message.payload;
            return ApplyResult::Applied;
        case MessageDomain::Chat:
        {
            const auto peer = peerNames_.find(message.peerId);
            if (message.peerId.empty() || peer == peerNames_.end()) {
                return reject("chat message came from an unknown peer");
            }
            const auto textField = message.payload.find("text");
            if (textField == message.payload.end() || !textField->is_string()) {
                return reject("chat message is missing text");
            }
            std::string text;
            if (!normalize_chat_text(textField->get_ref<const std::string&>(), text)) {
                return reject("chat message text is invalid");
            }

            constexpr size_t kChatBurstLimit = 6;
            constexpr auto kChatBurstWindow = std::chrono::seconds(5);
            const auto now = std::chrono::steady_clock::now();
            auto& recent = chatMessageTimes_[message.peerId];
            while (!recent.empty() && now - recent.front() >= kChatBurstWindow) {
                recent.pop_front();
            }
            if (recent.size() >= kChatBurstLimit) return ApplyResult::IgnoredByPolicy;
            recent.push_back(now);
            push_chat_message(peer->second, std::move(text),
                              appearance::peer_color(message.peerId));
            return ApplyResult::Applied;
        }
        case MessageDomain::Progression:
        {
            if (type == "floor_switch_state") {
                if (!syncFlagsEnabled_ || !peerNames_.contains(message.peerId))
                    return ApplyResult::IgnoredByPolicy;
                return receive_floor_switch_state(message.peerId, message.payload);
            }
            const bool record = saveRecoveryEnabled_ && saveRecoveryPolicyReady_ && saveRecovery_ &&
                message.receivedAt >= saveRecovery_->savedAt &&
                message.receivedAt < saveRecovery_->captureUntil &&
                activeSaveSlot_ && saveRecovery_->slot == *activeSaveSlot_ &&
                saveRecovery_->saveName == activeSaveName_ &&
                !opening_or_title_active() &&
                !randomizer_active() && type != "sync_request" &&
                !(type == "save_snapshot" && message.payload.value("manual_sync", false)) &&
                type != "room_actor_action" &&
                type != "web_timer" && type != "room_switch_bit";
            // Replay is captured once as a remote-only batch, not one full
            // native save scan for every retained flag.
            const bool captureNow = record && !replayingSaveRecovery_;
            const nlohmann::json before = captureNow ? make_recovery_snapshot() : nlohmann::json();
            RemoteApplicationGuard applying(applyingRemote_);
            const ApplyResult result = consume_progression(message);
            if (captureNow) record_received_progress(before, result);
            if (record && (result == ApplyResult::Applied || result == ApplyResult::Retained) &&
                (type == "event_bit" || type == "switch_bit" || type == "dark_clear_lv")) {
                const nlohmann::json value = message.payload.value("set", true) ?
                    nlohmann::json(true) : nlohmann::json(nullptr);
                if (type == "event_bit")
                    record_recovery_intent(message, {"event_flags", message.payload.at("flag").get<int>()}, value);
                else if (type == "switch_bit")
                    record_recovery_intent(message, {"switches", message.payload.at("stage").get<int>(),
                                                     message.payload.at("flag").get<int>()}, value);
                else if (type == "dark_clear_lv")
                    record_recovery_intent(message, {"dark_clear_levels", message.payload.at("no").get<int>()}, true);
            }
            if (!replayingSaveRecovery_ && stage_ready() && !opening_or_title_active())
                poll_local_state(false);
            return result;
        }
        case MessageDomain::OptionalRandomizer:
        {
            RemoteApplicationGuard applying(applyingRemote_);
            return consume_randomizer(message);
        }
        case MessageDomain::Interaction:
            if (coop::handles(type)) return coop::consume(message);
            return type == "pvp_hit" ? consume_pvp_hit(message) :
                                        reject("unknown interaction message: " + type);
        case MessageDomain::Visual:
            // Protocol 2 uses UDP exclusively for Link/Midna visuals. A TCP
            // copy must not clog the stage deferral queue.
            return ApplyResult::IgnoredByPolicy;
        case MessageDomain::Ganondorf:
            // Ganondorf synchronization is not implemented by this mod.
            // Consume these packets as policy-disabled so they cannot fill
            // the queue.
            return ApplyResult::IgnoredByPolicy;
        default:
            return reject("unclassified message reached game adapter: " + type);
        }
    } catch (const nlohmann::json::exception& error) {
        return reject(std::string("malformed gameplay message: ") + error.what());
    }
}

ApplyResult GameAdapter::consume_randomizer(const RoutedMessage& message) {
    if (!randomizer_active()) return ApplyResult::IgnoredByPolicy;
    if (!message.payload.is_object() ||
        message.payload.value("type", std::string()) != "rando_item_get") {
        return reject("malformed randomizer item message");
    }
    const auto itemValue = message.payload.find("item_id");
    if (itemValue == message.payload.end() || !itemValue->is_number_integer()) {
        return reject("randomizer item message is missing item_id");
    }
    const int itemId = itemValue->get<int>();
    if (itemId < 0 || itemId > 0xFF) {
        return reject("randomizer item_id is out of range");
    }
    const uint8_t itemToApply = static_cast<uint8_t>(itemId);

    const bool ooccooGrant = ooccoo::is_grant(itemToApply);
    if (ooccooGrant && !ooccoo_sync_active()) return ApplyResult::IgnoredByPolicy;
    if (ooccooGrant && (!message.payload.contains("ooccoo_state") ||
        !ooccoo::decode(message.payload["ooccoo_state"]))) {
        return reject("randomizer Ooccoo reward requires version 2 acquisition metadata");
    }

    std::string checkName;
    if (const auto check = message.payload.find("check_name");
        check != message.payload.end()) {
        if (!check->is_string()) return reject("randomizer check_name is not a string");
        checkName = check->get<std::string>();
        if (checkName.empty() || checkName.size() > 255) {
            return reject("randomizer check_name has an invalid length");
        }
        // Older senders may still attach the shared :255 sentinel name.
        // It is not a one-time location and must not suppress another pickup.
        if (repeatable_pickup_check(checkName)) checkName.clear();
        if (!checkName.empty() && !completedRandomizerChecks_.insert(checkName).second) {
            svc_log->info(mod_ctx,
                ("Ignored duplicate remote randomizer check '" + checkName + "'").c_str());
            return ApplyResult::Applied;
        }
    }

    // Apply the same policy to older peers which still send ordinary drops.
    if (local_only_randomizer_pickup(itemToApply, checkName))
        return ApplyResult::IgnoredByPolicy;

    // Freestanding checks normally publish their item bit at the end of the
    // get-item demo. Carry it with the resolved reward as well, so another
    // player cannot collect the actor during that cutscene-sized window.
    if (!checkName.empty() && checkName.starts_with("freestanding:") &&
        message.payload.contains("location_stage") &&
        message.payload.contains("location_flag")) {
        const int stage = message.payload.value("location_stage", -1);
        const int flag = message.payload.value("location_flag", -1);
        if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::DAN_ITEM) {
            completedRandomizerChecks_.erase(checkName);
            return reject("randomizer freestanding location is out of range");
        }
        remember_memory_item(stage, flag);
        stage_bits(stage).onItem(flag);
        repair_remote_memory_item_collectible(stage, flag);
    }

    // ItemService observers report the sender's already-resolved reward.
    // Apply that exact item, matching execResolvedItemGet in the combined
    // implementation; never resolve the remote check a second time.
    // Currency is applied by rupee_count, not twice via this item grant too.
    // Still retain the check's collected flag and its player notification.
    if (ooccooGrant) {
        if (!accept_ooccoo_state(message.payload["ooccoo_state"]))
            return ApplyResult::IgnoredByPolicy;
        apply_shared_ooccoo_local_form();
    } else if (remote_pickup_requires_grant(itemToApply))
        execItemGet(itemToApply, 0, nullptr);
    else if (rupee_pickup_amount(itemToApply) != 0)
        dComIfGs_onItemFirstBit(itemToApply);

    const std::string localPeerId = message.ingress.mode == net::Mode::DirectHost ?
                                        "direct" : message.ingress.clientId;
    if (!message.peerId.empty() && message.peerId != localPeerId &&
        itemToApply != dItemNo_HEART_e && itemToApply != dItemNo_NONE_e) {
        const auto name = peerNames_.find(message.peerId);
        const std::string peerName = name != peerNames_.end() ? name->second : message.peerId;
        const auto check = randomizer_check_display_name(checkName);
        push_online_player_notification(
            peerName, std::string(randomizer_item_name(itemToApply)),
            std::string(check), appearance::peer_color(message.peerId));
    }

    std::ostringstream log;
    log << (rupee_pickup_amount(itemToApply) != 0 ?
                "Received randomizer rupee reward (wallet total applied separately) 0x" :
                "Applied resolved randomizer item 0x") << std::hex << std::setw(2)
        << std::setfill('0') << static_cast<int>(itemToApply);
    if (!checkName.empty()) log << " for check '" << checkName << "'";
    if (message.payload.contains("location_stage") &&
        message.payload.contains("location_flag")) {
        log << std::dec << " location_stage=" << message.payload.value("location_stage", -1)
            << " location_flag=" << message.payload.value("location_flag", -1);
    }
    svc_log->info(mod_ctx, log.str().c_str());
    return ApplyResult::Applied;
}

ApplyResult GameAdapter::consume_pvp_hit(const RoutedMessage& message) {
    const net::EventContext& ingress = message.ingress;
    const std::string localPeerId = ingress.mode == net::Mode::DirectHost ?
                                        "direct" : ingress.clientId;
    if (message.peerId.empty() || message.peerId == localPeerId) {
        return ApplyResult::IgnoredByPolicy;
    }

    const nlohmann::json state = message.payload.value("state", nlohmann::json::object());
    if (!state.is_object()) return reject("PvP hit state is not an object");
    if (state.value("target_peer_id", std::string()) != localPeerId) {
        return ApplyResult::Applied;
    }
    if (!ingress.welcomed || !net::effective_pvp(ingress.settings) ||
        !stage_ready() || manualTransitionActive_) {
        return ApplyResult::IgnoredByPolicy;
    }

    const uint32_t sequence = message.payload.value("sequence", 0U);
    const auto last = pvpRemoteHitLastSequence_.find(message.peerId);
    if (last != pvpRemoteHitLastSequence_.end() && last->second >= sequence) {
        return ApplyResult::IgnoredByPolicy;
    }
    const char* localStage = dComIfGp_getStartStageName();
    if (localStage == nullptr || state.value("stage", std::string()) != localStage) {
        return ApplyResult::IgnoredByPolicy;
    }

    const int attackClass = state.value("attack_class", kPvpAttackLight);
    if (attackClass != kPvpAttackLight && attackClass != kPvpAttackHeavy) {
        return reject("invalid PvP attack class");
    }
    const std::string reaction = state.value("reaction", std::string());
    const bool ironBallLaunch = attackClass == kPvpAttackHeavy &&
                                reaction == kPvpReactionIronBallLaunch;
    const bool specialTechnique = attackClass == kPvpAttackHeavy &&
        (reaction == kPvpReactionMortalDraw || reaction == kPvpReactionGreatSpin);
    const bool clawshot = attackClass == kPvpAttackLight &&
                          reaction == kPvpReactionClawshot;
    const bool slingshot = attackClass == kPvpAttackLight &&
                           reaction == kPvpReactionSlingshot;
    const bool shieldBash = reaction == kPvpReactionShieldBash;
    int damage = attackClass == kPvpAttackHeavy ? kPvpHeavyDamage : kPvpLightDamage;
    if (ironBallLaunch) damage = kPvpIronBallDamage;
    else if (clawshot) damage = kPvpClawshotDamage;
    else if (slingshot) damage = kPvpSlingshotDamage;
    else if (shieldBash) damage = 0;
    else if (specialTechnique) damage = kPvpSpecialTechniqueDamage;
    const bool blocked = !ironBallLaunch && !shieldBash && state.value("blocked", false);
    const float sourceX = state.value("source_x", 0.0f);
    const float sourceZ = state.value("source_z", 0.0f);

    daAlink_c* player = static_cast<daAlink_c*>(daPy_getPlayerActorClass());
    if (player != nullptr &&
        (player->checkCameraLargeDamage() || player->getDamageWaitTimer() != 0)) {
        pvpRemoteHitLastSequence_[message.peerId] = sequence;
        return ApplyResult::IgnoredByPolicy;
    }
    if (shieldBash) {
        (void)apply_pvp_player_shield_bash(sourceX, sourceZ);
    } else if (blocked) {
        (void)apply_pvp_player_shield_block(attackClass, sourceX, sourceZ);
    } else {
        (void)apply_pvp_player_damage(attackClass, ironBallLaunch, damage, sourceX, sourceZ);
    }
    pvpRemoteHitLastSequence_[message.peerId] = sequence;
    return ApplyResult::Applied;
}

ApplyResult GameAdapter::consume_udp(const net::Event& event) {
    if (event.kind == net::EventKind::UdpMessage) {
        // Transport already expands and records the matrix baseline before a
        // DirectHost can forward the pose. Re-expanding here duplicates work
        // and makes correctness depend on that operation staying idempotent.
        const nlohmann::json& expandedMessage = event.message;
        const std::string type = expandedMessage.value("type", std::string());
        if (type == "midna_pose") {
            auto existing = peerPoses_.find(event.peerId);
            if (existing == peerPoses_.end()) return ApplyResult::Deferred;
            std::string error;
            if (!merge_midna_pose(expandedMessage, existing->second, error)) {
                return error.empty() ? ApplyResult::IgnoredByPolicy : reject(std::move(error));
            }
            return ApplyResult::Applied;
        }
        if (type != "pose") return reject("unexpected UDP visual message type: " + type);
        auto existing = peerPoses_.find(event.peerId);
        const auto* previous = existing == peerPoses_.end() ? nullptr : &existing->second;
        dusk::multiplayer::PeerPoseSnapshot pose;
        std::string error;
        if (!decode_peer_pose(expandedMessage, event.peerId, previous, pose, error)) {
            if (error == "stale pose sequence") return ApplyResult::IgnoredByPolicy;
            return reject(std::move(error));
        }
        if (event.udpType == net::udp::PacketType::SemanticPoseMsgpack &&
            !enforce_semantic_pose_invariants(pose, error)) {
            return reject(std::move(error));
        }
        maybe_queue_progression_pose_prompt(event.peerId, pose);
        // Scene changes and teleports must never blend through old locations.
        if (previous != nullptr) {
            const float dx = pose.x - previous->x;
            const float dy = pose.y - previous->y;
            const float dz = pose.z - previous->z;
            if (pose.stage != previous->stage || pose.room != previous->room ||
                pose.layer != previous->layer || dx * dx + dy * dy + dz * dz > 600.0f * 600.0f) {
                posePlayback_.erase(event.peerId);
                presentedPoses_.erase(event.peerId);
            }
        }
        if (visual_wire_trace_enabled()) {
            auto& timing = poseTiming_[event.peerId];
            const auto now = std::chrono::steady_clock::now();
            if (timing.received.time_since_epoch().count() != 0) {
                timing.maxReceiveGapMs = std::max(timing.maxReceiveGapMs,
                    std::chrono::duration<double, std::milli>(now - timing.received).count());
            }
            if (timing.receivedSequence && pose.sequence > timing.receivedSequence + 1)
                timing.missing += pose.sequence - timing.receivedSequence - 1;
            timing.received = now;
            timing.receivedSequence = pose.sequence;
            ++timing.packets;
            ++timing.pending;
        }
        posePlayback_[event.peerId].push(pose.sequence, pose);
        peerPoses_[event.peerId] = std::move(pose);
        return ApplyResult::Applied;
    }
    if (event.kind == net::EventKind::UdpRemoteObject) {
        if (!event.ingress.welcomed || !event.ingress.settings.syncWorld) {
            return ApplyResult::IgnoredByPolicy;
        }
        const auto& packet = event.remoteObject;
        dusk::multiplayer::RemoteBombObjectSnapshot object;
        object.valid = true;
        object.peerId = event.peerId;
        object.objectKind = packet.objectKind;
        object.objectId = packet.objectId;
        object.sequence = packet.sequence;
        object.stage.assign(packet.stageName,
            std::find(packet.stageName, packet.stageName + sizeof(packet.stageName), '\0'));
        object.room = packet.room;
        object.x = packet.x; object.y = packet.y; object.z = packet.z;
        object.angleY = packet.angleY;
        object.kind = packet.kind;
        object.exTime = packet.exTime;
        object.active = (packet.flags & net::udp::ObjectActive) != 0;
        object.exploding = (packet.flags & net::udp::ObjectExploding) != 0;
        dusk::multiplayer::set_remote_bomb_object(object);
        return ApplyResult::Applied;
    }
    const std::string localSender = event.ingress.mode == net::Mode::DirectHost ?
                                        "direct" : event.ingress.clientId;
    if (event.kind != net::EventKind::UdpAck || event.detail != localSender) {
        return ApplyResult::IgnoredByPolicy;
    }
    // Preserve local-sender ACK sequence state for acknowledged snapshot deltas.
    std::ostringstream key;
    key << event.peerId << ':' << static_cast<int>(event.udpType);
    uint32_t& last = latestAckSequence_[key.str()];
    if (event.udpSequence <= last) return ApplyResult::IgnoredByPolicy;
    last = event.udpSequence;
    return ApplyResult::Applied;
}

void GameAdapter::set_player_color(uint32_t color, uint32_t outfit, uint32_t zora) {
    if (appearance::local_color() == color && appearance::local_outfit_color() == outfit &&
        appearance::local_zora_color() == zora) return;
    appearance::set_local(color, outfit, zora);
    publish_player_color();
}

void GameAdapter::sample_live_field_map_marker() {
    // The pause menu moves Link's actor into menu space. Keep the last sampled
    // gameplay map position until normal play resumes.
    if (dComIfGp_isPauseFlag() || dMeter2Info_getPauseStatus() != 0 ||
        opening_or_title_active() ||
        dComIfGp_getStageStagInfo() == nullptr || dComIfGp_isEnableNextStage() ||
        !dComIfGs_isPlayerFieldLastStayFieldDataExistFlag()) return;

    const char* stage = dComIfGp_getStartStageName();
    const int region = dComIfGp_getNowLevel();
    fopAc_ac_c* player = daPy_getPlayerActorClass();
    if (stage == nullptr || stage[0] == '\0' || std::strlen(stage) >= 8 ||
        region < 1 || region > 8 || player == nullptr) return;

    const Vec pos = dMapInfo_n::getMapPlayerPos();
    if (!std::isfinite(pos.x) || !std::isfinite(pos.z)) return;
    liveFieldMapMarker_ = PlayerLocationView::FieldMapMarker{
        stage, region, pos.x, pos.z, static_cast<s16>(player->shape_angle.y)};
}

std::optional<PlayerLocationView::FieldMapMarker> GameAdapter::local_field_map_marker() const {
    if (opening_or_title_active()) return std::nullopt;
    if (dComIfGs_isPlayerFieldLastStayFieldDataExistFlag()) {
        const char* stage = dComIfGp_getStartStageName();
        const int region = dComIfGp_getNowLevel();
        if (liveFieldMapMarker_ && stage != nullptr &&
            liveFieldMapMarker_->stage == stage && liveFieldMapMarker_->region == region) {
            return liveFieldMapMarker_;
        }
        return std::nullopt;
    }

    const char* stage = dComIfGs_getPlayerFieldLastStayName();
    const int region = dComIfGs_getPlayerFieldLastStayRegionNo();
    const cXyz pos = dComIfGs_getPlayerFieldLastStayPos();
    if (stage == nullptr || stage[0] == '\0' || std::strlen(stage) >= 8 ||
        region < 1 || region > 8 || !std::isfinite(pos.x) || !std::isfinite(pos.z)) {
        return std::nullopt;
    }
    return PlayerLocationView::FieldMapMarker{
        stage, region, pos.x, pos.z, dComIfGs_getPlayerFieldLastStayAngleY()};
}

void GameAdapter::publish_player_color() {
    if (!transport_.status().enabled) return;
    const auto now = std::chrono::steady_clock::now();
    nlohmann::json message = {{"type", "presence"},
                             {"player_color", appearance::color_string(appearance::local_color())},
                             {"outfit_color", appearance::color_string(appearance::local_outfit_color())},
                             {"zora_color", appearance::color_string(appearance::local_zora_color())},
                             {"latency_probe", ++nextLatencyNonce_}};
    nlohmann::json echoes = nlohmann::json::object();
    for (const auto& [peerId, probe] : latencyEchoes_) {
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - probe.receivedAt).count();
        if (wait >= 0 && wait <= 10000 && peerNames_.contains(peerId)) {
            echoes[peerId] = {{"nonce", probe.nonce}, {"wait_ms", wait}};
        }
    }
    if (!echoes.empty()) message["latency_echoes"] = std::move(echoes);
    const char* stage = dComIfGp_getStartStageName();
    if (stage && stage[0]) {
        message["stage"] = stage;
        message["room"] = static_cast<int>(dComIfGp_roomControl_getStayNo());
        message["layer"] = static_cast<int>(dComIfGp_getStartStageLayer());
    }
    if (const auto marker = local_field_map_marker()) {
        message["field_map"] = {{"stage", marker->stage}, {"region", marker->region},
                                {"x", marker->x}, {"z", marker->z},
                                {"angle_y", marker->angleY}};
    }
    if (transport_.send(message)) {
        latencySent_.emplace_back(nextLatencyNonce_, now);
        if (latencySent_.size() > 16) latencySent_.pop_front();
    }
}

void GameAdapter::observe_latency_presence(const RoutedMessage& message) {
    if (!peerNames_.contains(message.peerId)) return;
    const auto now = std::chrono::steady_clock::now();
    const auto probe = message.payload.find("latency_probe");
    if (probe != message.payload.end() && probe->is_number_unsigned()) {
        const uint64_t nonce = probe->get<uint64_t>();
        if (nonce != 0) latencyEchoes_[message.peerId] = {nonce, now};
    }

    const auto echoes = message.payload.find("latency_echoes");
    if (echoes == message.payload.end() || !echoes->is_object()) return;
    const net::Status status = transport_.status();
    const std::string localPeerId = status.mode == net::Mode::DirectHost ?
        "direct" : status.clientId;
    const auto echo = echoes->find(localPeerId);
    if (echo == echoes->end() || !echo->is_object()) return;
    const auto nonceField = echo->find("nonce");
    const auto waitField = echo->find("wait_ms");
    if (nonceField == echo->end() || !nonceField->is_number_unsigned() ||
        waitField == echo->end() || !waitField->is_number_integer()) return;
    const uint64_t nonce = nonceField->get<uint64_t>();
    const int64_t wait = waitField->get<int64_t>();
    if (wait < 0 || wait > 10000) return;
    const auto previous = latencySamples_.find(message.peerId);
    if (previous != latencySamples_.end() && nonce <= previous->second.nonce) return;
    const auto sent = std::find_if(latencySent_.begin(), latencySent_.end(),
        [nonce](const auto& value) { return value.first == nonce; });
    if (sent == latencySent_.end()) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - sent->second).count();
    if (elapsed < wait || elapsed - wait > 10000) return;
    latencySamples_[message.peerId] = {
        nonce, static_cast<uint32_t>(elapsed - wait), now};
}

std::optional<uint32_t> GameAdapter::current_peer_latency_ms(
    std::string_view peerId, std::chrono::steady_clock::time_point now) const {
    if (auto direct = transport_.direct_peer_rtt_ms(peerId)) return direct;
    if (transport_.status().mode == net::Mode::CloudRoom) return std::nullopt;
    const auto sample = latencySamples_.find(std::string(peerId));
    if (sample == latencySamples_.end() ||
        now - sample->second.receivedAt > std::chrono::seconds(5)) return std::nullopt;
    return sample->second.milliseconds;
}

std::optional<uint32_t> GameAdapter::peer_latency_ms(std::string_view peerId) const {
    const auto now = std::chrono::steady_clock::now();
    if (displayedLatenciesAt_ == std::chrono::steady_clock::time_point{} ||
        now - displayedLatenciesAt_ >= std::chrono::seconds(1)) {
        displayedLatenciesAt_ = now;
        displayedLatencies_.clear();
        for (const auto& [id, name] : transport_.peers()) {
            (void)name;
            if (auto latency = current_peer_latency_ms(id, now))
                displayedLatencies_.emplace(id, *latency);
        }
    }
    const auto display = displayedLatencies_.find(std::string(peerId));
    return display == displayedLatencies_.end() ? std::nullopt :
        std::optional<uint32_t>(display->second);
}

std::optional<net::udp::VoicePosition> GameAdapter::voice_position() const {
    if (opening_or_title_active()) {
        lastVoicePosition_.reset();
        return std::nullopt;
    }
    // Gameplay sync waits for events to finish, but Link can still be present
    // and moving during doors, cutscenes, and the Game Over sequence.
    if (dComIfGp_getStageStagInfo() != nullptr &&
        !dComIfGp_isEnableNextStage() &&
        !fopOvlpM_IsPeek() && !fopOvlpM_IsDoingReq()) {
        const auto* local = dComIfGp_getPlayer(0);
        const char* stage = dComIfGp_getStartStageName();
        if (local != nullptr && stage != nullptr && stage[0] != '\0' &&
            std::strlen(stage) < 8 &&
            std::isfinite(local->current.pos.x) &&
            std::isfinite(local->current.pos.y) &&
            std::isfinite(local->current.pos.z)) {
            net::udp::VoicePosition position;
            std::memcpy(position.stageName, stage, std::strlen(stage));
            position.x = local->current.pos.x;
            position.y = local->current.pos.y;
            position.z = local->current.pos.z;
            position.room = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
            lastVoicePosition_ = position;
            return position;
        }
    }
    // During a room load the actor may disappear briefly. Keep the last safe
    // position until Link returns, the title loads, or the lobby is left.
    return lastVoicePosition_;
}

float GameAdapter::voice_gain(const net::udp::VoicePosition& position,
                              int rangePercent) const {
    const auto local = voice_position();
    if (!local ||
        std::strncmp(position.stageName, local->stageName, 8) != 0 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) ||
        !std::isfinite(local->x) || !std::isfinite(local->y) ||
        !std::isfinite(local->z)) return 0.0f;
    const float dx = position.x - local->x;
    const float dy = position.y - local->y;
    const float dz = position.z - local->z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Scale the entire fade so 50% preserves the original 4000-unit reach.
    const float rangeScale = std::clamp(rangePercent, 0, 200) / 50.0f;
    if (rangeScale <= 0.0f) return 0.0f;
    const float fadeDistance = 4000.0f * rangeScale;
    const float tailDistance = 500.0f * rangeScale;
    if (distance >= fadeDistance) return 0.0f;
    const float attenuation = std::exp2(-6.0f * distance / fadeDistance);
    const float tail = std::min(1.0f, (fadeDistance - distance) / tailDistance);
    return attenuation * tail;
}

float GameAdapter::voice_pan(const net::udp::VoicePosition& position) const {
    if (dComIfGp_getStageStagInfo() == nullptr ||
        dComIfGp_isEnableNextStage() ||
        fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq()) return 0.5f;
    const char* stage = dComIfGp_getStartStageName();
    Z2AudioMgr* audio = Z2GetAudioMgr();
    if (stage == nullptr || audio == nullptr ||
        std::strncmp(position.stageName, stage, 8) != 0 ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) return 0.5f;
    Vec absolute{position.x, position.y, position.z};
    Vec relative{};
    Z2Audience& audience = audio->mAudience;
    (void)audience.convertAbsToRel(absolute, &relative, 0);
    return std::clamp(audience.calcRelPosPan(relative, 0), 0.0f, 1.0f);
}

void GameAdapter::consume_welcome_membership(const nlohmann::json& message) {
    appearance::reset_peers();
    peerNames_.clear();
    const std::string directHost = message.value("direct_peer_name", std::string());
    if (!directHost.empty()) {
        peerNames_["direct"] = directHost;
    }
    for (const auto& peer : message.value("peers", nlohmann::json::array())) {
        if (!peer.is_object()) continue;
        const std::string id = peer.value("client_id", std::string());
        if (id.empty()) continue;
        peerNames_[id] = peer.value("name", id);
    }
    publish_player_color();
}

void GameAdapter::peer_joined(std::string_view peerId, std::string_view name) {
    // Coalesce membership changes; read the save only on a ready game tick.
    ooccooCatchupPending_ = true;
    peerNames_[std::string(peerId)] = std::string(name);
    publish_player_color();
    push_online_notification((name.empty() ? std::string(peerId) : std::string(name)) +
                             " joined the lobby.");
}

void GameAdapter::peer_left(std::string_view peerId) {
    coop::peer_left(std::string(peerId));
    forget_floor_switch_peer(peerId);
    const std::string key(peerId);
    const auto nameIt = peerNames_.find(key);
    push_online_notification((nameIt != peerNames_.end() ? nameIt->second : key) +
                             " left the lobby.");
    peerNames_.erase(key);
    appearance::forget_peer(key);
    peerPresence_.erase(key);
    latencyEchoes_.erase(key);
    latencySamples_.erase(key);
    displayedLatencies_.erase(key);
    peerProgressionStates_.erase(key);
    peerProgressionAges_.erase(key);
    peerPoses_.erase(key);
    posePlayback_.erase(key);
    poseTiming_.erase(key);
    presentedPoses_.erase(key);
    dusk::multiplayer::destroy_remote_link_dummy(key);
    dusk::multiplayer::erase_remote_actor_peer(key);
    clear_remote_pose_history(key);
    pendingProgressionCues_.erase(
        std::remove_if(pendingProgressionCues_.begin(), pendingProgressionCues_.end(),
                       [&](const PendingProgressionCue& cue) { return cue.peerId == key; }),
        pendingProgressionCues_.end());
    if (progressionPrompt_.peerId == key) progressionPrompt_ = {};
    if (pendingProgressionPeerId_ == key) {
        pendingProgressionPeerId_.clear();
        pendingProgressionCueKey_.clear();
    }
    if (awaitingManualSyncPeerId_ == key) {
        awaitingManualSyncPeerId_.clear();
        awaitingManualSyncCueKey_.clear();
        awaitingManualSyncRequestId_ = 0;
    }
    if (manualSyncPeerId_ == key && manualSyncState_ == ManualSyncState::Waiting) {
        manualSyncState_ = ManualSyncState::Failed;
        manualSyncRequestedFromTitle_ = false;
    }
    pvpRemoteHitLastSequence_.erase(key);
    chatMessageTimes_.erase(key);
    pvpLocalHitContactsThisUpdate_.clear();
    pvpSpinnerAttackTargets_.erase(key);
    fishCatchSequence_.erase(key);
    const std::string prefix = key + ':';
    const auto erasePrefixedMap = [&](auto& values) {
        for (auto it = values.begin(); it != values.end();) {
            if (it->first.rfind(prefix, 0) == 0) {
                it = values.erase(it);
            } else {
                ++it;
            }
        }
    };
    erasePrefixedMap(latestAckSequence_);
    erasePrefixedMap(permanentPickupSequence_);
    erasePrefixedMap(appliedTearEvents_);
    pendingSyncReplies_.erase(
        std::remove_if(pendingSyncReplies_.begin(), pendingSyncReplies_.end(),
                       [&](const PendingSyncReply& reply) { return reply.peerId == key; }),
        pendingSyncReplies_.end());
    for (auto it = handledProgressionCues_.begin(); it != handledProgressionCues_.end();) {
        if (it->rfind(prefix, 0) == 0) {
            it = handledProgressionCues_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = shownPoseProgressionCues_.begin();
         it != shownPoseProgressionCues_.end();) {
        if (it->rfind(prefix, 0) == 0) {
            it = shownPoseProgressionCues_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameAdapter::reset_session() {
    coop::reset();
    saveRecovery_.reset();
    saveRecoveryEnabled_ = false;
    saveRecoveryPolicyReady_ = false;
    activeSaveName_.clear();
    activeSaveSlot_.reset();
    liveFieldMapMarker_.reset();
    lastVoicePosition_.reset();
    reset_floor_switch_state(true);
    sVisualWireTrace = {};
    reset_local_pose_state();
    dusk::multiplayer::destroy_all_remote_link_dummies();
    dusk::multiplayer::reset_remote_actor_bridge();
    reset_bomb_sync_state();
    peerNames_.clear();
    appearance::reset_peers();
    peerPresence_.clear();
    latencyEchoes_.clear();
    latencySamples_.clear();
    latencySent_.clear();
    nextLatencyNonce_ = 0;
    displayedLatencies_.clear();
    displayedLatenciesAt_ = {};
    peerProgressionStates_.clear();
    peerProgressionAges_.clear();
    peerPoses_.clear();
    posePlayback_.clear();
    presentedPoses_.clear();
    clear_remote_pose_history();
    poseTiming_.clear();
    poseTimingUpdate_ = {};
    poseTimingTicks_ = 0;
    poseTimingMaxUpdateMs_ = 0;
    latestAckSequence_.clear();
    pvpRemoteHitLastSequence_.clear();
    chatMessageTimes_.clear();
    pvpLocalHitContactsThisUpdate_.clear();
    pvpSpinnerAttackTargets_.clear();
    pvpSpinnerActor_ = 0;
    pvpSpinnerAttackActive_ = false;
    localPvpHitSequence_ = 0;
    reset_visual_overlays();
    clear_replaced_save_progression_state();
    deferredSwitches_.clear();
    pendingDarkClears_.fill(0);
    permanentPickupSequence_.clear();
    fishCatchSequence_.clear();
    appliedTearEvents_.clear();
    completedRandomizerChecks_.clear();
    lastError_.clear();
    progressionTicks_ = 0;
    presenceTicks_ = 0;
    localPoseSequence_ = 0;
    ooccooState_.reset();
    ooccooBoundToSave_ = false;
    ooccooCatchupPending_ = true;
    ooccooReplyPending_ = false;
    localObservedState_ = nlohmann::json();
    stableStageName_.clear();
    stableRoom_ = -128;
    stableRoomTicks_ = 0;
    initializedStageName_.clear();
    initializedRoom_ = -128;
    initializedRoomTicks_ = 0;
    localPermanentSequence_ = 0;
    localFishSequence_ = 0;
    pendingManualInfo_.clear();
    pendingManualFlagsSave_.clear();
    manualTransitionActive_ = false;
    manualReloadPending_ = false;
    pendingOrdonEventBits_.clear();
    ordonReloadSafeTicks_ = 0;
    ordonReloadWaitTicks_ = 0;
    ordonReloadTransitionActive_ = false;
    ordonReloadSawStageLoad_ = false;
    mirrorReloadPending_ = false;
    zoraThawPending_ = nlohmann::json();
    deferredStoryEvents_.clear();
    deferredLocalEvents_.clear();
    faronDayBroadcastHoldTicks_ = 0;
    localFaronWarpSequenceActive_ = false;
    lastLocalTboxStage_ = -1;
    lastLocalTboxFlag_ = -1;
    lastLocalTboxAt_ = {};
    progressionPrompt_ = {};
    progressionPromptsSuppressed_ = false;
    pendingProgressionCues_.clear();
    pendingProgressionPeerId_.clear();
    pendingProgressionCueKey_.clear();
    awaitingManualSyncCueKey_.clear();
    awaitingManualSyncPeerId_.clear();
    awaitingManualSyncRequestId_ = 0;
    handledProgressionCues_.clear();
    shownPoseProgressionCues_.clear();
    pendingSyncReplies_.clear();
    manualSyncState_ = ManualSyncState::None;
    manualSyncFlagsOnly_ = false;
    manualSyncRequestedFromTitle_ = false;
    manualSyncPeerId_.clear();
    manualSyncRequestId_ = 0;
    manualSyncWaitTicks_ = 0;
    manualSyncTimedOut_ = false;
}

const std::string& GameAdapter::last_error() const {
    return lastError_;
}

std::string GameAdapter::manual_sync_status_text() const {
    switch (manualSyncState_) {
    case ManualSyncState::Waiting:
        return std::string("waiting for ") +
            (manualSyncFlagsOnly_ ? "flags from " : "sync from ") + manualSyncPeerId_;
    case ManualSyncState::Succeeded:
        return manualSyncFlagsOnly_ ? "flags sync applied" : "sync-and-warp applied";
    case ManualSyncState::Failed:
        return "sync request timed out or peer disconnected";
    default:
        return {};
    }
}

bool GameAdapter::manual_sync_waiting() const {
    return manualSyncState_ == ManualSyncState::Waiting;
}

bool GameAdapter::manual_sync_failed() const {
    return manualSyncState_ == ManualSyncState::Failed;
}

bool GameAdapter::manual_sync_timed_out() const {
    return manualSyncState_ == ManualSyncState::Failed && manualSyncTimedOut_;
}

ApplyResult GameAdapter::apply_switch_bit(const nlohmann::json& message,
                                          std::string_view peerId) {
    const int stage = message.value("stage", -1);
    const int flag = message.value("flag", -1);
    if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::MEMORY_SWITCH) {
        return reject("invalid switch_bit bounds");
    }
    if (is_unsynced_switch_bit(stage, flag) || loaded_floor_switch_owns_flag(stage, flag))
        return ApplyResult::IgnoredByPolicy;
    const bool set = message.value("set", true);

    // Completion first lowers this client's participant state and advertises
    // that edge immediately. The completion mutation itself remains behind
    // the shared fence until every other current participant is also safe.
    if (stage == dStage_SaveTbl_FARON && flag == 71 && set) {
        set_local_faron_warp_sequence_active(false);
    }
    if (is_faron_warp_sequence_switch(stage, flag) && should_defer_faron_warp_sequence()) {
        // Event bits and switch bits share one ordered queue. Keep the full
        // message and sender so cross-type ordering, repeated edges, prompts,
        // and source policy survive replay.
        deferredFaronInbound_.push_back(
            {std::string(peerId), message,
             {MessageDomain::Progression, true, true}});
        return ApplyResult::Retained;
    }

    if (set) maybe_queue_progression_switch_prompt(peerId, stage, flag);

    const int sourceActor = message.value("source_actor", -1);
    if (is_group2_lifecycle_actor(sourceActor)) {
        const int sourceRoom = message.value("source_room", -128);
        const uint32_t sourceParams = message.value("source_params", 0U);
        const bool bridgeCompletion = message.contains("source_params") &&
            is_eldin_gorge_bridge_completion(stage, flag, sourceActor, sourceRoom, sourceParams);
        // Validate against the sender's cave, not the recipient's current
        // stage, so a reveal also survives travel or arrives while elsewhere.
        // The sender already checked the false-to-true edge; applying it twice
        // is harmless and must never turn a section back off.
        const bool caveMap = is_cave_map_reveal(sourceActor == fpcNm_SWC00_e,
            message.value("source_stage", std::string()), stage, sourceRoom, flag,
            sourceParams, message.value("source_angle_x", -1), false, set);
        const bool sacredBlock = is_sacred_grove_block_switch(
            message.value("source_stage", std::string()), stage, sourceRoom, flag,
            sourceActor, sourceParams, message.value("source_angle_x", -1), set);
        if (!bridgeCompletion && !is_sewers_progression_switch(stage, flag) &&
            !caveMap && !sacredBlock)
            return ApplyResult::IgnoredByPolicy;
    }

    if (!set) {
        dComIfGs_offStageSwitch(stage, flag);
        return ApplyResult::Applied;
    }

    const RemoteSwitchPolicy* policy = remote_switch_policy(stage, flag);
    if (policy != nullptr && policy->mode == RemoteSwitchPolicyMode::SuppressRemote) {
        return ApplyResult::IgnoredByPolicy;
    }
    if (policy != nullptr && policy->mode == RemoteSwitchPolicyMode::DeferUntilRoomInit) {
        const bool roomMatches = policy->room < 0 || stableRoom_ == policy->room;
        const bool initializedRoomMatches = policy->room < 0 || initializedRoom_ == policy->room;
        const bool ready = stableStageName_ == policy->stageName && roomMatches &&
                           stableRoomTicks_ >= kRemoteSwitchRoomInitTicks &&
                           initializedStageName_ == policy->stageName && initializedRoomMatches &&
                           initializedRoomTicks_ >= kRemoteSwitchRoomInitTicks;
        if (!ready) {
            const bool duplicate = std::any_of(deferredSwitches_.begin(), deferredSwitches_.end(),
                [&](const nlohmann::json& queued) {
                    return queued.value("stage", -1) == stage &&
                           queued.value("flag", -1) == flag;
                });
            if (!duplicate) {
                deferredSwitches_.push_back(message);
            }
            return ApplyResult::Retained;
        }
    }
    dComIfGs_onStageSwitch(stage, flag);
    repair_remote_switch_actors(stage, flag);
    repair_floor_switch_completion(stage, flag);
    return ApplyResult::Applied;
}

ApplyResult GameAdapter::apply_snapshot_switch_bit(int stage, int flag) {
    if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::MEMORY_SWITCH) {
        return reject("invalid snapshot switch_bit bounds");
    }
    if (is_unsynced_switch_bit(stage, flag) || loaded_floor_switch_owns_flag(stage, flag))
        return ApplyResult::IgnoredByPolicy;

    // Snapshot hydration uses the audited remote-switch safety policy, but it
    // must not enter live progression prompts, Faron sequencing, or source-
    // actor filtering. Those are event semantics, not state-union semantics.
    const RemoteSwitchPolicy* policy = remote_switch_policy(stage, flag);
    if (policy != nullptr && policy->mode == RemoteSwitchPolicyMode::SuppressRemote) {
        return ApplyResult::IgnoredByPolicy;
    }
    if (policy != nullptr && policy->mode == RemoteSwitchPolicyMode::DeferUntilRoomInit) {
        const bool ready = stableStageName_ == policy->stageName &&
            (policy->room < 0 || stableRoom_ == policy->room) &&
            stableRoomTicks_ >= kRemoteSwitchRoomInitTicks &&
            initializedStageName_ == policy->stageName &&
            (policy->room < 0 || initializedRoom_ == policy->room) &&
            initializedRoomTicks_ >= kRemoteSwitchRoomInitTicks;
        if (!ready) {
            const bool duplicate = std::any_of(deferredSwitches_.begin(), deferredSwitches_.end(),
                [&](const nlohmann::json& queued) {
                    return queued.value("stage", -1) == stage &&
                           queued.value("flag", -1) == flag;
                });
            if (!duplicate) {
                deferredSwitches_.push_back(
                    {{"type", "switch_bit"}, {"stage", stage}, {"flag", flag}, {"set", true}});
            }
            return ApplyResult::Retained;
        }
    }
    dComIfGs_onStageSwitch(stage, flag);
    repair_remote_switch_actors(stage, flag);
    repair_floor_switch_completion(stage, flag);
    return ApplyResult::Applied;
}

void GameAdapter::flush_deferred_switches() {
    for (auto it = deferredSwitches_.begin(); it != deferredSwitches_.end();) {
        const int stage = it->value("stage", -1);
        const int flag = it->value("flag", -1);
        const RemoteSwitchPolicy* policy = remote_switch_policy(stage, flag);
        const bool ready = policy == nullptr ||
            (stableStageName_ == policy->stageName &&
             (policy->room < 0 || stableRoom_ == policy->room) &&
             stableRoomTicks_ >= kRemoteSwitchRoomInitTicks &&
             initializedStageName_ == policy->stageName &&
             (policy->room < 0 || initializedRoom_ == policy->room) &&
             initializedRoomTicks_ >= kRemoteSwitchRoomInitTicks);
        if (!ready) {
            ++it;
            continue;
        }
        if (loaded_floor_switch_owns_flag(stage, flag)) {
            it = deferredSwitches_.erase(it);
            continue;
        }
        if (it->value("set", true)) {
            dComIfGs_onStageSwitch(stage, flag);
            repair_remote_switch_actors(stage, flag);
            repair_floor_switch_completion(stage, flag);
        } else {
            dComIfGs_offStageSwitch(stage, flag);
        }
        it = deferredSwitches_.erase(it);
    }
}

ApplyResult GameAdapter::apply_dark_clear(int level) {
    if (level < 0 || level > 3) return reject("invalid dark_clear_lv index");
    if (dComIfGs_isDarkClearLV(level)) {
        if (level < 3) pendingDarkClears_[level] = 0;
        return ApplyResult::Applied;
    }
    const char* currentStage = dComIfGp_getStartStageName();
    const std::string_view stage = stage_ready() && currentStage != nullptr ?
        std::string_view(currentStage) : std::string_view(stableStageName_);
    const int room = stage_ready() ? dComIfGp_roomControl_getStayNo() : stableRoom_;
    if (level == 2 && stage == "R_SP107") {
        pendingDarkClears_[level] = 2;
        return ApplyResult::Retained;
    }
    if (level < 3 && dKy_darkworld_check() &&
        twilight_completion_level_for_stage(stage, room) == level) {
        pendingDarkClears_[level] = 1;
        return ApplyResult::Retained;
    }
    if (level < 3) pendingDarkClears_[level] = 0;
    dComIfGs_onDarkClearLV(level);
    return ApplyResult::Applied;
}

void GameAdapter::flush_pending_dark_clears() {
    for (int level = 0; level < 3; ++level) {
        if (pendingDarkClears_[level] == 0) continue;
        if (dComIfGs_isDarkClearLV(level)) {
            pendingDarkClears_[level] = 0;
        } else if (pendingDarkClears_[level] == 2 && stableStageName_ != "R_SP107") {
            (void)apply_dark_clear(level);
        }
    }
}

bool GameAdapter::should_defer_faron_warp_sequence() const {
    if (localFaronWarpSequenceActive_) return true;
    for (const auto& [peer, state] : peerProgressionStates_) {
        const auto age = peerProgressionAges_.find(peer);
        if (age == peerProgressionAges_.end() ||
            age->second > kProgressionStateReadyMaxAgeTicks) continue;
        if (state.value("faron_warp_sequence_active", false)) return true;
    }
    return false;
}

void GameAdapter::send_progression_state(bool force) {
    if (!syncFlagsEnabled_ || !transport_.status().welcomed) return;
    if (!force && ++progressionTicks_ < 15) return;
    progressionTicks_ = 0;

    const char* stage = dComIfGp_getStartStageName();
    const bool hasStage = stage != nullptr && stage[0] != '\0';
    const bool finalGanondorfReady = hasStage && std::strcmp(stage, "D_MN09B") == 0 &&
                                    dComIfGs_isSaveDunSwitch(1);
    fopAc_ac_c* player = dComIfGp_getPlayer(0);
    const int room = player != nullptr ? static_cast<int>(fopAcM_GetRoomNo(player)) :
                                        static_cast<int>(dComIfGp_roomControl_getStayNo());
    transport_.send({
        {"type", "progression_state"},
        {"stage", hasStage ? stage : ""},
        {"room", room},
        {"layer", static_cast<int>(dComIfGp_getStartStageLayer())},
        {"manual_sync_ready", stage_ready()},
        {"final_ganondorf_ready", finalGanondorfReady},
        {"faron_cage_sequence_active", localFaronCageSequenceActive_},
        {"faron_warp_sequence_active", localFaronWarpSequenceActive_},
    });
}

void GameAdapter::set_local_faron_warp_sequence_active(bool active) {
    if (localFaronWarpSequenceActive_ == active) return;
    localFaronWarpSequenceActive_ = active;
    send_progression_state(true);
}

void GameAdapter::update_local_faron_cage_sequence_state() {
    const char* stage = dComIfGp_getStartStageName();
    const bool active = stage != nullptr && std::string_view(stage) == "F_SP108" &&
                        !stage_ready();
    if (localFaronCageSequenceActive_ == active) return;
    localFaronCageSequenceActive_ = active;
    send_progression_state(true);
}

bool GameAdapter::has_active_faron_cage_sequence_peer() const {
    for (const auto& [peer, state] : peerProgressionStates_) {
        const auto age = peerProgressionAges_.find(peer);
        if (age == peerProgressionAges_.end() ||
            age->second > kProgressionStateReadyMaxAgeTicks) continue;
        if (state.contains("faron_cage_sequence_active")) {
            if (state.value("faron_cage_sequence_active", false)) return true;
        } else if (state.value("stage", std::string()) == "F_SP108" &&
                   !state.value("manual_sync_ready", false)) {
            // Mixed-version compatibility: older peers expose the same state
            // through their stage/readiness tuple.
            return true;
        }
    }
    return false;
}

ApplyResult GameAdapter::apply_event_bit(const RoutedMessage& routed) {
    const nlohmann::json& message = routed.payload;
    const int rawFlag = message.value("flag", -1);
    if (rawFlag < 0 || rawFlag > 0xFFFF) return reject("invalid event_bit flag");
    const uint16_t flag = static_cast<uint16_t>(rawFlag);
    if (is_unsynced_event_bit(flag)) return ApplyResult::IgnoredByPolicy;
    const bool set = message.value("set", true);
    if (flag == dSv_event_flag_c::M_035) {
        if (!set) {
            std::erase_if(deferredStoryEvents_, [](const nlohmann::json& queued) {
                return queued.value("flag", -1) == dSv_event_flag_c::M_035;
            });
        } else if (local_wagon_escort_unfinished()) {
            nlohmann::json queued = message;
            queued["_peer_id"] = routed.peerId;
            enqueue_unique_deferred_mutation(deferredStoryEvents_, std::move(queued));
            return ApplyResult::Retained;
        }
    }
    if (is_faron_warp_sequence_event_bit(flag) && should_defer_faron_warp_sequence()) {
        deferredFaronInbound_.push_back(routed);
        return ApplyResult::Retained;
    }
    if (!set) {
        if (flag == 0x2B08) mirrorReloadPending_ = false;
        if (flag == 0x0880) zoraThawPending_ = nlohmann::json();
        if (pendingOrdonEventBits_.erase(flag) != 0 && pendingOrdonEventBits_.empty()) {
            ordonReloadSafeTicks_ = 0;
            ordonReloadWaitTicks_ = 0;
            ordonReloadTransitionActive_ = false;
            ordonReloadSawStageLoad_ = false;
        }
        dComIfGs_offEventBit(flag);
        return ApplyResult::Applied;
    }
    maybe_queue_progression_event_prompt(routed.peerId, flag);
    const char* eventStage = dComIfGp_getStartStageName();
    if (flag == 0x2B08 && !dComIfGs_isEventBit(flag) && eventStage != nullptr &&
        is_mirror_complete_reload_stage(eventStage)) {
        mirrorReloadPending_ = true;
        return ApplyResult::Retained;
    }
    if (dComIfGs_isEventBit(flag)) return ApplyResult::IgnoredByPolicy;
    if (flag == 0x0880) {
        const std::string_view stage = stage_ready() && eventStage != nullptr ?
            std::string_view(eventStage) : std::string_view(stableStageName_);
        const int room = stage_ready() ? dComIfGp_roomControl_getStayNo() : stableRoom_;
        if (!is_zora_thaw_reload_area(stage, room)) {
            dComIfGs_onEventBit(flag);
            return ApplyResult::Applied;
        }
        zoraThawPending_ = message;
        return ApplyResult::Retained;
    }
    if (is_ordon_day_boundary_event_bit(flag)) {
        const char* current = dComIfGp_getStartStageName();
        if (current != nullptr && std::string_view(current) == "F_SP108" && !stage_ready()) {
            nlohmann::json queued = message;
            queued["_peer_id"] = routed.peerId;
            // Only fixed, validated idempotent story bits reach this queue;
            // coalesce their duplicate network copies without losing state.
            enqueue_unique_deferred_mutation(deferredStoryEvents_, std::move(queued));
            return ApplyResult::Retained;
        }
        if (current != nullptr && is_ordon_day_boundary_stage(current)) {
            if (pendingOrdonEventBits_.insert(flag).second) {
                ordonReloadSafeTicks_ = 0;
                ordonReloadWaitTicks_ = 0;
                ordonReloadTransitionActive_ = true;
                ordonReloadSawStageLoad_ = false;
            }
            // Day 3 must close old-day progression triggers even while a
            // cutscene delays the room reload. Keep the reload queued, but
            // retain the existing deferred application for Day 1 -> 2.
            if (flag == 0x4510) {
                dComIfGs_onEventBit(flag);
                return ApplyResult::Applied;
            }
            return ApplyResult::Retained;
        }
    }
    dComIfGs_onEventBit(flag);
    return ApplyResult::Applied;
}

void GameAdapter::flush_story_messages() {
    bool appliedDeferredRemote = false;
    const bool cageSequenceActive = localFaronCageSequenceActive_ ||
                                    has_active_faron_cage_sequence_peer();

    // Final applies retained inbound day-boundary events before releasing the
    // corresponding outbound broadcasts.
    if (!cageSequenceActive && faronDayBroadcastHoldTicks_ == 0 &&
        !deferredStoryEvents_.empty()) {
        std::deque<nlohmann::json> pending = std::move(deferredStoryEvents_);
        deferredStoryEvents_.clear();
        RemoteApplicationGuard applying(applyingRemote_);
        for (nlohmann::json& payload : pending) {
            if (payload.value("flag", -1) == dSv_event_flag_c::M_035 &&
                local_wagon_escort_unfinished()) {
                deferredStoryEvents_.push_back(std::move(payload));
                continue;
            }
            const std::string peer = payload.value("_peer_id", std::string());
            payload.erase("_peer_id");
            RoutedMessage routed{peer, std::move(payload),
                                 {MessageDomain::Progression, false, true}};
            (void)apply_event_bit(routed);
            appliedDeferredRemote = true;
        }
    }

    // Faron warp switch/event mutations are one ordered stream. Replay every
    // full routed message through the live dispatcher only after all current
    // participants have lowered their fence.
    if (!should_defer_faron_warp_sequence() && !deferredFaronInbound_.empty()) {
        std::deque<RoutedMessage> pending = std::move(deferredFaronInbound_);
        deferredFaronInbound_.clear();
        RemoteApplicationGuard applying(applyingRemote_);
        for (RoutedMessage& routed : pending) {
            // AIO re-enters its dispatcher: switches wait for stage readiness,
            // while safe event bits can proceed during a cutscene. Retain the
            // switch order without blocking those independent event bits.
            if (routed.spec.stageDependent && !stage_ready()) {
                deferredFaronInbound_.push_back(std::move(routed));
                continue;
            }
            (void)consume(routed);
        }
        appliedDeferredRemote = true;
    }

    if (appliedDeferredRemote && stage_ready() && !opening_or_title_active()) {
        poll_local_state(false);
    }

    for (auto it = deferredLocalEvents_.begin(); it != deferredLocalEvents_.end();) {
        const int raw = it->value("flag", -1);
        const bool ordon = it->value("type", std::string()) == "event_bit" && raw >= 0 &&
                           is_ordon_day_boundary_event_bit(static_cast<uint16_t>(raw));
        const bool faron = (it->value("type", std::string()) == "event_bit" && raw >= 0 &&
                            is_faron_warp_sequence_event_bit(static_cast<uint16_t>(raw))) ||
                           (it->value("type", std::string()) == "switch_bit" &&
                            is_faron_warp_sequence_switch(it->value("stage", -1),
                                                          it->value("flag", -1)));
        if ((ordon && (faronDayBroadcastHoldTicks_ != 0 || cageSequenceActive)) ||
            (faron && should_defer_faron_warp_sequence())) {
            ++it;
            continue;
        }
        transport_.send(*it);
        it = deferredLocalEvents_.erase(it);
    }

}

void GameAdapter::flush_story_events() {
    RemoteApplicationGuard applying(applyingRemote_);
    if (mirrorReloadPending_) {
        mirrorReloadPending_ = false;
        dComIfGs_onEventBit(0x2B08);
        if (is_mirror_complete_reload_stage(stableStageName_)) {
            daPy_py_c::forceRestartRoom(0, 5, 0xC9);
            return;
        }
    }

    if (zoraThawPending_.is_object()) {
        nlohmann::json pending = std::move(zoraThawPending_);
        zoraThawPending_ = nlohmann::json();
        dComIfGs_onEventBit(0x0880);
        if (is_zora_thaw_reload_area(stableStageName_, stableRoom_)) {
            const auto destination = pending.value("zora_thaw_destination",
                                                   nlohmann::json::object());
            const std::string stage = destination.value("stage", std::string());
            const int room = destination.value("room", -1);
            const int layer = destination.value("layer", -2);
            const int point = destination.value("start_point", -1);
            const bool valid = stage == "F_SP113" && (room == 0 || room == 1) &&
                               layer >= -1 && layer < 15 && point >= 0 && point <= 255;
            if (valid) {
                dComIfGp_setNextStage(stage.c_str(), point, room, layer, 0.0f, 0, 1, 0, 0, 1, 3);
            } else if (stableStageName_ == "F_SP126" ||
                       (stableStageName_ == "F_SP115" && stableRoom_ == 0)) {
                dComIfGp_setNextStage("F_SP113", 10, 1, -1, 0.0f, 0, 1, 0, 0, 1, 3);
            } else {
                daPy_py_c::forceRestartRoom(0, 5, 0xC9);
            }
            return;
        }
    }

    if (!pendingOrdonEventBits_.empty()) {
        if (!is_ordon_day_boundary_stage(stableStageName_)) {
            pendingOrdonEventBits_.clear();
            ordonReloadSafeTicks_ = 0;
            ordonReloadWaitTicks_ = 0;
            ordonReloadTransitionActive_ = false;
            ordonReloadSawStageLoad_ = false;
            return;
        }
        if (ordonReloadWaitTicks_ < std::numeric_limits<uint32_t>::max()) {
            ++ordonReloadWaitTicks_;
        }
        bool peerUnsafe = false;
        for (const auto& [peer, state] : peerProgressionStates_) {
            const auto age = peerProgressionAges_.find(peer);
            if (age == peerProgressionAges_.end() ||
                age->second > kProgressionStateReadyMaxAgeTicks) continue;
            if (is_ordon_day_boundary_stage(state.value("stage", std::string())) &&
                !state.value("manual_sync_ready", false)) {
                peerUnsafe = true;
                break;
            }
        }
        const bool deleteQueueBusy = !fpcDt_IsComplete();
        if (peerUnsafe || deleteQueueBusy) {
            ordonReloadSafeTicks_ = 0;
            if (ordonReloadWaitTicks_ == kOrdonReloadWarningTicks) {
                svc_log->warn(mod_ctx,
                    "Ordon day-boundary reload remains deferred for safety");
            }
            return;
        }
        if (++ordonReloadSafeTicks_ < 3) {
            return;
        }
        for (const uint16_t flag : pendingOrdonEventBits_) {
            if (!dComIfGs_isEventBit(flag)) dComIfGs_onEventBit(flag);
        }
        pendingOrdonEventBits_.clear();
        ordonReloadSafeTicks_ = 0;
        ordonReloadWaitTicks_ = 0;
        daPy_py_c::forceRestartRoom(0, 5, 0xC9);
    }
}

ApplyResult GameAdapter::consume_progression(const RoutedMessage& routed) {
    const nlohmann::json& message = routed.payload;
    const std::string type = message.at("type").get<std::string>();

    // These have story-sequence, reload, full-state or shared-warp semantics.
    // They remain in the router instead of receiving an unsafe partial apply.
    if (type == "save_snapshot") {
        return apply_save_snapshot(routed);
    }
    if (type == "sync_request") {
        if (routed.peerId.empty()) {
            return ApplyResult::IgnoredByPolicy;
        }
        const std::string cueKey = message.value("cue_key", std::string());
        const bool flagsOnly = message.value("manual_sync_mode", "warp") == "flags";
        const uint64_t requestId = manual_sync_request_id(message);
        const bool safeNow = stage_ready() && !opening_or_title_active() &&
                             fpcM_SearchByName(fpcNm_GAMEOVER_e) == nullptr &&
                             local_state_ready_for_cue(cueKey);
        if (!safeNow) {
            const auto duplicate = std::find_if(
                pendingSyncReplies_.begin(), pendingSyncReplies_.end(),
                [&](const PendingSyncReply& reply) {
                    return reply.peerId == routed.peerId && reply.cueKey == cueKey &&
                           reply.flagsOnly == flagsOnly;
                });
            if (duplicate == pendingSyncReplies_.end()) {
                pendingSyncReplies_.push_back({routed.peerId, cueKey, flagsOnly, 0, requestId});
                std::ostringstream line;
                line << "Online sync request queued peer=" << routed.peerId
                     << " mode=" << (flagsOnly ? "flags" : "warp")
                     << " cue=" << (cueKey.empty() ? "manual" : cueKey);
                dusklight_online::log_info(line.str());
            } else {
                duplicate->requestId = requestId;
                duplicate->waitTicks = 0;
            }
            return ApplyResult::Retained;
        }
        if (!cueKey.empty()) handledProgressionCues_.insert(routed.peerId + ':' + cueKey);
        send_snapshot_to(routed.peerId, true, flagsOnly, requestId);
        return ApplyResult::Applied;
    }
    if (type == "dark_clear_lv") return apply_dark_clear(message.value("no", -1));

    if (type == "switch_bit") return apply_switch_bit(message, routed.peerId);
    if (type == "web_timer") {
        const int stage = message.value("stage", -1);
        const int room = message.value("room", -1);
        const int actorName = message.value("source_actor", -1);
        const uint32_t params = message.value("source_params", 0xFFFFFFFFU);
        const int timer = message.value("timer", -1);
        if (!valid_stage(stage) || room < 0 || room >= 64 ||
            !is_web_switch_actor(actorName) ||
            (timer != 1 && !(actorName == fpcNm_OBJ_WEB0_e && timer == 41))) {
            return reject("invalid web_timer payload");
        }
        const char* currentStage = dComIfGp_getStartStageName();
        if (stage != current_stage_table() || currentStage == nullptr ||
            message.value("source_stage", std::string()) != currentStage ||
            dComIfGp_roomControl_getStayNo() != room) {
            return ApplyResult::IgnoredByPolicy;
        }
        return apply_remote_web_timer(actorName, room, params, timer)
                   ? ApplyResult::Applied
                   : ApplyResult::IgnoredByPolicy;
    }
    if (type == "room_actor_action") {
        const int stage = message.value("stage", -1);
        const int room = message.value("room", -1);
        const int actorName = message.value("source_actor", -1);
        const uint32_t params = message.value("source_params", 0xFFFFFFFFU);
        const int actionValue = message.value("action", -1);
        const int actionArgument = message.value("action_arg", 0);
        if (!valid_stage(stage) || room < 0 || room >= 64 || actionValue < 1 ||
            actionValue > 7 || !is_room_action_actor(actorName)) {
            return reject("invalid room_actor_action payload");
        }
        const auto action = static_cast<RoomActorAction>(actionValue);
        if (!valid_room_actor_action(actorName, action)) {
            return reject("invalid room_actor_action actor/action");
        }
        const char* currentStage = dComIfGp_getStartStageName();
        if (stage != current_stage_table() || currentStage == nullptr ||
            message.value("source_stage", std::string()) != currentStage ||
            dComIfGp_roomControl_getStayNo() != room) {
            return ApplyResult::IgnoredByPolicy;
        }
        if ((action == RoomActorAction::MoveStep &&
             (actionArgument < 8 || (actionArgument >> 3) > 300)) ||
            (action == RoomActorAction::RotateTo &&
             (actionArgument < 0 || actionArgument >= 4)) ||
            (action == RoomActorAction::Slide &&
             (actionArgument < 4 || actionArgument > 11 ||
              (actionArgument >> 2) > 2))) {
            return reject("invalid room_actor_action argument");
        }
        return apply_remote_room_actor_action(actorName, room, params, action, actionArgument)
                   ? ApplyResult::Applied
                   : ApplyResult::IgnoredByPolicy;
    }
    if (type == "room_switch_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        const int room = message.value("room", -1);
        const int sourceActor = message.value("source_actor", -1);
        if (!valid_stage(stage) || flag < 0 || flag >= 0xFF || room < 0 || room >= 64) {
            return reject("invalid room_switch_bit bounds");
        }
        if (is_local_escort_gate_switch(stage, flag, sourceActor, room,
                                        message.value("source_stage", std::string()))) {
            return ApplyResult::IgnoredByPolicy;
        }
        const bool webSwitch = is_web_switch_actor(sourceActor);
        const bool permanentActor = is_permanent_room_actor(sourceActor);
        const bool liveTorch = is_live_torch_switch_actor(sourceActor);
        if (!is_small_key_door_switch_actor(sourceActor) && !webSwitch && !permanentActor &&
            !liveTorch) {
            return ApplyResult::IgnoredByPolicy;
        }
        if (!webSwitch && !permanentActor && !liveTorch &&
            flag < dSv_info_c::MEMORY_SWITCH) {
            return reject("invalid key-door room_switch_bit flag");
        }
        if (stage != current_stage_table()) return ApplyResult::IgnoredByPolicy;
        if (webSwitch || permanentActor || liveTorch) {
            const char* currentStage = dComIfGp_getStartStageName();
            const std::string sourceStage = message.value("source_stage", std::string());
            const uint32_t sourceParams = message.value("source_params", 0xFFFFFFFFU);
            const int sourceRoom = message.value("source_room", -1);
            if (currentStage == nullptr || sourceStage != currentStage || sourceRoom != room ||
                (webSwitch && static_cast<int>((sourceParams >> 24) & 0xFF) != flag) ||
                (liveTorch && (flag < dSv_info_c::MEMORY_SWITCH + dSv_info_c::DAN_SWITCH ||
                               static_cast<int>(sourceParams & 0xFF) != flag)) ||
                (permanentActor &&
                 permanent_room_actor_switch_flag(sourceActor, sourceParams) != flag) ||
                dComIfGp_roomControl_getStayNo() != room) {
                return ApplyResult::IgnoredByPolicy;
            }
        }
        if (permanentActor) {
            const int fallbackValue = message.value("source_action", -1);
            if (fallbackValue < 1 || fallbackValue > 4 ||
                !valid_room_actor_action(sourceActor,
                                         static_cast<RoomActorAction>(fallbackValue))) {
                return reject("invalid room actor switch fallback");
            }
            return apply_remote_room_actor_switch(
                       sourceActor, room, flag,
                       message.value("source_params", 0xFFFFFFFFU),
                       static_cast<RoomActorAction>(fallbackValue))
                       ? ApplyResult::Applied
                       : ApplyResult::IgnoredByPolicy;
        }
        if (liveTorch && !message.value("set", true)) {
            dComIfGs_offSwitch(flag, room);
        } else {
            dComIfGs_onSwitch(flag, room);
        }
        if (webSwitch) {
            repair_remote_web_actor(sourceActor, room, flag,
                                    message.value("source_params", 0xFFFFFFFFU));
        }
        return ApplyResult::Applied;
    }
    if (type == "ooccoo_state") {
        if (message.contains("request_state") && !message["request_state"].is_boolean())
            return reject("Ooccoo request_state must be boolean");
        if (!accept_ooccoo_state(message.value("state", nlohmann::json::object()))) {
            return ApplyResult::IgnoredByPolicy;
        }
        if (message.value("request_state", false)) ooccooReplyPending_ = true;
        apply_shared_ooccoo_local_form();
        return ApplyResult::Applied;
    }

    if (type == "event_bit") return apply_event_bit(routed);

    if (type == "tbox_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        if (!valid_stage(stage) || flag < 0 || flag >= 64) {
            return reject("invalid tbox_bit bounds");
        }
        const bool newlySet = !stage_bits(stage).isTbox(flag);
        stage_bits(stage).onTbox(flag);
        repair_remote_tbox_collectible(stage, flag, newlySet);
        return ApplyResult::Applied;
    }
    if (type == "dungeon_item_bit") {
        const int stage = message.value("stage", -1);
        const int kind = message.value("kind", -1);
        if (!valid_stage(stage) || kind < 0 || kind > 7) {
            return reject("invalid dungeon_item_bit bounds");
        }
        switch (kind) {
        case 0: dComIfGs_onDungeonItemMap(stage); break;
        case 1: dComIfGs_onDungeonItemCompass(stage); break;
        case 2: dComIfGs_onDungeonItemBossKey(stage); break;
        case 3: dComIfGs_onStageBossEnemy(stage); break;
        case 4: dComIfGs_onStageLife(stage); break;
        case 5: dComIfGs_onStageBossDemo(stage); break;
        case 6: return ApplyResult::IgnoredByPolicy; // Use versioned Ooccoo receipts.
        case 7: dComIfGs_onStageMiddleBoss(stage); break;
        }
        return ApplyResult::Applied;
    }
    if (type == "item_bit") {
        const int stage = message.value("stage", -1);
        const int flag = message.value("flag", -1);
        if (!valid_stage(stage) || flag < 0 || flag >= dSv_info_c::DAN_ITEM) {
            return reject("invalid item_bit bounds");
        }
        stage_bits(stage).onItem(flag);
        remember_memory_item(stage, flag);
        repair_remote_memory_item_collectible(stage, flag);
        return ApplyResult::Applied;
    }
    if (type == "item_get") {
        const int itemId = message.value("item_id", -1);
        if (itemId < 0 || itemId > 0xFF || !is_synced_key_item(itemId)) {
            return reject("invalid or unsynchronized item_get item_id");
        }
        if (!dComIfGs_isItemFirstBit(static_cast<u8>(itemId))) {
            // Record an older quest grant without putting its already-traded
            // item back into the shared slot.
            if (!randomizer_active() && trade_item::item_phase(itemId) >= 0 &&
                observed_trade_phase() > trade_item::item_phase(itemId))
                record_older_trade_item(itemId);
            else
                execItemGet(static_cast<u8>(itemId), 0, nullptr);
        }
        if (itemId == dItemNo_KANTERA_e) repair_lantern_item_state();
        return ApplyResult::Applied;
    }
    if (type == "trade_item") {
        if (randomizer_active()) return ApplyResult::IgnoredByPolicy;
        const int phase = message.value("phase", -1);
        const int item = message.value("item", -1);
        if (!apply_trade_state(phase, item)) return reject("invalid trade_item state");
        return ApplyResult::Applied;
    }
    if (type == "item_first_bit") {
        const int itemId = message.value("item_id", -1);
        if (itemId < 0 || itemId > 0xFF || !is_synced_item_first_bit(itemId)) {
            return reject("invalid or unsynchronized item_first_bit item_id");
        }
        if (message.value("owned", true)) {
            dComIfGs_onItemFirstBit(static_cast<u8>(itemId));
        } else {
            dComIfGs_offItemFirstBit(static_cast<u8>(itemId));
        }
        return ApplyResult::Applied;
    }
    if (type == "collect_crystal" || type == "collect_mirror") {
        const int item = message.value("item", -1);
        if (item < 0 || item >= 8) {
            return reject("invalid collect shard index");
        }
        if (type == "collect_crystal") {
            dComIfGs_onCollectCrystal(static_cast<u8>(item));
        } else {
            dComIfGs_onCollectMirror(static_cast<u8>(item));
        }
        return ApplyResult::Applied;
    }
    if (type == "transform_lv") {
        const int level = message.value("no", -1);
        if (level != 3 || !dComIfGs_isEventBit(dSv_event_flag_c::M_071)) {
            // Baseline keeps every unaudited layer selector behind an opt-in.
            return ApplyResult::IgnoredByPolicy;
        }
        dComIfGs_onTransformLV(level);
        return ApplyResult::Applied;
    }
    if (type == "region_bit") {
        const int region = message.value("region", -1);
        if (region < 0 || region >= 8) {
            return reject("invalid region_bit index");
        }
        dComIfGs_onRegionBit(region);
        return ApplyResult::Applied;
    }
    if (type == "collect") {
        const int collectType = message.value("collect_type", -1);
        const int item = message.value("item", -1);
        if (collectType < 0 || collectType > B_BUTTON_ITEM || item < 0 || item >= 8) {
            return reject("invalid collect record");
        }
        if (randomizer_active()) {
            return ApplyResult::IgnoredByPolicy;
        }
        g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(
            collectType, static_cast<u8>(item));
        return ApplyResult::Applied;
    }
    if (type == "visited_room") {
        const int stage = message.value("stage", -1);
        const int room = message.value("room", -1);
        if (stage < 0 || stage >= dSv_save_c::STAGE2_MAX || room < 0 || room >= 64) {
            return reject("invalid visited_room bounds");
        }
        dComIfGs_onSaveVisitedRoom(stage, room);
        return ApplyResult::Applied;
    }
    if (type == "letter_get") {
        const int number = message.value("no", -1);
        if (number < 0 || number >= LETTER_INFO_BIT) {
            return reject("invalid letter_get index");
        }
        dComIfGs_onLetterGetFlag(number);
        return ApplyResult::Applied;
    }
    if (type == "key_num") {
        const int stage = message.value("stage", -1);
        const int count = message.value("count", -1);
        if (!valid_stage(stage) || count < 0 || count > 99) {
            return reject("invalid key_num bounds");
        }
        if (is_local_escort_key_count(stage)) return ApplyResult::IgnoredByPolicy;
        if (randomizer_active() && count > stage_bits(stage).getKeyNum()) {
            // The reward event owns positive gains; key expenditure still
            // travels as an absolute decrease so doors stay synchronized.
            return ApplyResult::IgnoredByPolicy;
        }
        dComIfGs_setKeyNum(stage, static_cast<u8>(count));
        return ApplyResult::Applied;
    }
    if (type == "light_drop_num") {
        const int area = message.value("area", -1);
        const int count = message.value("count", -1);
        const int previous = message.value("previous_count", -1);
        const int tearStage = message.value("tear_stage", -1);
        const int tearFlag = message.value("tear_flag", -1);
        if (area < 0 || area > 0xFF || count < 0 || count > 0xFF) {
            return reject("invalid light_drop_num bounds");
        }
        const int current = dComIfGs_getLightDropNum(static_cast<u8>(area));
        int merged = std::max(current, count);
        if (previous >= 0 && previous <= 0xFF && tearStage >= 0 && tearFlag >= 0 &&
            count == previous + 1) {
            std::ostringstream key;
            key << routed.peerId << ':' << area << ':' << tearStage << ':' << tearFlag;
            if (appliedTearEvents_.emplace(key.str(), true).second &&
                current > previous && current < 0xFF) {
                merged = current + 1;
            }
        }
        dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(merged));
        if (area == 2 && merged == 15) {
            dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[9]);
        }
        return ApplyResult::Applied;
    }
    if (type == "light_drop_get_flag") {
        const int area = message.value("area", -1);
        if (area < 0 || area >= 3) {
            return reject("invalid light_drop_get_flag area");
        }
        dComIfGs_onLightDropGetFlag(static_cast<u8>(area));
        dMeter2Info_setLightDropGetFlag(area, 0xFF);
        return ApplyResult::Applied;
    }
    if (type == "max_life_update") {
        // Randomizer Heart Pieces and Containers are authoritative through
        // rando_item_get. Applying this live vanilla companion as well grants
        // the same pickup twice; ignore packets from older peers too.
        if (randomizer_active()) return ApplyResult::IgnoredByPolicy;
        const int value = message.value("value", -1);
        const int previous = message.value("previous_value", -1);
        const uint32_t sequence = message.value("event_sequence", 0U);
        if (value < 0 || value > 100) {
            return reject("invalid max_life_update value");
        }
        bool newPickup = false;
        if (sequence != 0 && previous >= 0 && value > previous) {
            uint32_t& last = permanentPickupSequence_[sequence_key(routed, "max_life")];
            if (sequence > last) {
                last = sequence;
                newPickup = true;
            }
        }
        if (sequence != 0 && !newPickup) {
            return ApplyResult::IgnoredByPolicy;
        }
        const int pending = dComIfGp_getItemMaxLifeCount();
        const int projected = std::clamp<int>(dComIfGs_getMaxLife() + pending, 15, 100);
        if (pending != 0 && value == projected) {
            pendingMaxLifePublicationToSuppress_ = static_cast<uint8_t>(value);
            return ApplyResult::IgnoredByPolicy;
        }
        const int current = dComIfGs_getMaxLife();
        int merged = std::max(current, value);
        if (newPickup && current > previous) {
            merged = std::min(current + (value - previous), 100);
        }
        if (merged > current) {
            dComIfGs_setMaxLife(static_cast<u8>(merged));
            // Match moveLife's heart-piece/container refill through TP's
            // pending health counter, without granting the capacity again.
            dComIfGp_setItemLifeCount(
                static_cast<f32>(dComIfGs_getMaxLifeGauge() - dComIfGs_getLife()), 0);
        }
        return ApplyResult::Applied;
    }
    if (type == "bottle_slots") {
        const int count = message.value("count", -1);
        const int previous = message.value("previous_count", -1);
        const uint32_t sequence = message.value("event_sequence", 0U);
        if (count < 0 || count > 4) {
            return reject("invalid bottle_slots count");
        }
        if (randomizer_active()) {
            return ApplyResult::IgnoredByPolicy;
        }
        bool newPickup = false;
        if (sequence != 0 && previous >= 0 && count > previous) {
            uint32_t& last = permanentPickupSequence_[sequence_key(routed, "bottle_slots")];
            if (sequence > last) {
                last = sequence;
                newPickup = true;
            }
        }
        if (sequence != 0 && !newPickup) {
            return ApplyResult::IgnoredByPolicy;
        }

        const int local = bottle_slot_count();
        int merged = std::max(local, count);
        const bool hasSource = message.contains("source_item") &&
            message["source_item"].is_number_integer();
        const int source = hasSource ? message["source_item"].get<int>() : -1;
        if (hasSource && !is_vanilla_bottle_source(source)) {
            return reject("invalid bottle_slots source");
        }

        const bool exactBeforeMerge = bottle_sources_exact(
            bottleSourcesComplete_, completedBottleSources_.size(), local);
        bool distinctSource = false;
        if (newPickup && hasSource) {
            distinctSource = completedBottleSources_.insert(
                static_cast<uint8_t>(source)).second;
            remember_vanilla_bottle_source(source);
        } else if (newPickup && !hasSource) {
            // Older peers cannot identify which fixed vanilla reward produced
            // the slot. Absolute max is safe; additive merging is not.
            merged = std::max(local, count);
        }
        merged = merged_bottle_count(local, count, completedBottleSources_.size());
        for (int slot = local; slot < merged; ++slot) {
            dComIfGs_setEmptyBottle();
        }
        if (hasSource && exactBeforeMerge) {
            bottleSourcesComplete_ = static_cast<int>(completedBottleSources_.size()) == merged;
        } else if (hasSource) {
            bottleSourcesComplete_ = false;
        }

        std::ostringstream log;
        log << "Bottle source merge peer=" << routed.peerId
            << " source=" << source << " local=" << local
            << " remote=" << count << " merged=" << merged
            << " distinct=" << (distinctSource ? "yes" : "no")
            << " additive=no sources=" << completedBottleSources_.size()
            << " exact=" << (bottleSourcesComplete_ ? "yes" : "no");
        svc_log->info(mod_ctx, log.str().c_str());
        return ApplyResult::Applied;
    }
    if (type == "rupee_count") {
        // AIO wallet semantics apply in both vanilla and Randomizer modes.
        const int value = message.value("value", -1);
        const int pending = dComIfGp_getItemRupeeCount();
        const auto action = wallet_total_action(value, dComIfGs_getRupee(), pending,
                                                dComIfGs_getRupeeMax());
        if (action == WalletTotalAction::Reject) {
            return reject("invalid rupee_count value");
        }
        if (action == WalletTotalAction::Coalesce) {
            pendingRupeePublicationToSuppress_ = static_cast<uint16_t>(value);
            dusklight_online::log_info("Coalesced rupee total " + std::to_string(value) +
                                      " with pending change " + std::to_string(pending));
            return ApplyResult::IgnoredByPolicy;
        }
        dComIfGs_setRupee(static_cast<u16>(value));
        observedRupees_ = value;
        dusklight_online::log_info("Applied rupee total " + std::to_string(value) +
                                  " from " + routed.peerId);
        return ApplyResult::Applied;
    }
    if (type == "rupee_delta") {
        if (!randomizer_active()) return ApplyResult::IgnoredByPolicy;
        const int delta = message.value("delta", 0);
        const uint32_t sequence = message.value("event_sequence", 0U);
        if (delta >= 0 || delta < -dComIfGs_getRupeeMax()) {
            return reject("invalid rupee_delta value");
        }
        if (sequence == 0) {
            return reject("rupee_delta is missing event_sequence");
        }
        uint32_t& last = permanentPickupSequence_[sequence_key(routed, "rupee_delta")];
        if (sequence <= last) return ApplyResult::IgnoredByPolicy;
        last = sequence;
        const int value = std::clamp<int>(dComIfGs_getRupee() + delta, 0,
                                          dComIfGs_getRupeeMax());
        dComIfGs_setRupee(static_cast<u16>(value));
        observedRupees_ = value;
        return ApplyResult::Applied;
    }
    if (type == "poe_count") {
        const int value = message.value("value", -1);
        const int previous = message.value("previous_value", -1);
        const uint32_t sequence = message.value("event_sequence", 0U);
        if (value < 0 || value > MAX_POH_NUM) {
            return reject("invalid poe_count value");
        }
        if (randomizer_active()) {
            return ApplyResult::IgnoredByPolicy;
        }
        bool newPickup = false;
        if (sequence != 0 && previous >= 0 && value > previous) {
            uint32_t& last = permanentPickupSequence_[sequence_key(routed, "poe_count")];
            if (sequence > last) {
                last = sequence;
                newPickup = true;
            }
        }
        if (sequence != 0 && !newPickup) {
            return ApplyResult::IgnoredByPolicy;
        }
        const int current = dComIfGs_getPohSpiritNum();
        const int merged = merge_poe_pickup(current, {previous, value, sequence}, MAX_POH_NUM);
        if (merged > current) {
            dComIfGs_setPohSpiritNum(static_cast<u8>(merged));
            dusklight_online::log_info("Applied Poe count from " + routed.peerId +
                " previous=" + std::to_string(current) + " value=" + std::to_string(merged) +
                " sequence=" + std::to_string(sequence));
        }
        return ApplyResult::Applied;
    }
    if (type == "malo_fundraising") {
        const int phase = message.value("phase", -1);
        const int value = message.value("value", -1);
        if (phase < 0 || phase > 2 || value < 0 || value > kMaxSyncedDonationTotal) {
            return reject("invalid malo_fundraising value");
        }
        if (phase == malo_fundraising_phase()) {
            dMsgObject_setFundRaising(static_cast<u16>(value));
        }
        return ApplyResult::Applied;
    }
    if (type == "charlo_offering") {
        const int value = message.value("value", -1);
        if (value < 0 || value > kMaxSyncedDonationTotal) {
            return reject("invalid charlo_offering value");
        }
        if (value > dMsgObject_getOffering()) {
            dMsgObject_setOffering(static_cast<u16>(value));
        }
        return ApplyResult::Applied;
    }
    if (type == "fish_record") {
        const int index = message.value("index", -1);
        const int remoteCount = message.value("count", -1);
        const int remoteSize = message.value("max_size", -1);
        const uint32_t sequence = message.value("catch_sequence", 0U);
        if (index < 0 || index >= kSyncedFishSpeciesCount || remoteCount < 0 ||
            remoteCount > kMaxSyncedFishCount || remoteSize < 0 || remoteSize > 0xFF) {
            return reject("invalid fish_record value");
        }
        dSv_fishing_info_c& fish = g_dComIfG_gameInfo.info.getPlayer().getFishingInfo();
        const int localCount = fish.getFishCount(index);
        int mergedCount = std::max(localCount, remoteCount);
        if (sequence != 0) {
            uint32_t& last = fishCatchSequence_[routed.peerId];
            if (sequence > last) {
                last = sequence;
                mergedCount = std::max(mergedCount,
                                       std::min(localCount + 1, kMaxSyncedFishCount));
            }
        }
        fish.mFishCount[index] = static_cast<u16>(mergedCount);
        fish.setMaxSize(index, static_cast<u8>(std::max<int>(fish.getMaxSize(index), remoteSize)));
        return ApplyResult::Applied;
    }
    if (type == "collect_smell") {
        const int smell = message.value("value", -1);
        if (!valid_collect_smell(smell)) {
            return reject("invalid collect_smell value");
        }
        if (collect_smell_priority(smell) > collect_smell_priority(raw_collect_smell())) {
            if (smell != dItemNo_NONE_e) {
                dComIfGs_onItemFirstBit(static_cast<u8>(smell));
            }
            dComIfGs_setCollectSmell(static_cast<u8>(smell));
        }
        return ApplyResult::Applied;
    }
    if (type == "bomb_bag_slot") {
        if (randomizer_active()) {
            return ApplyResult::IgnoredByPolicy;
        }
        return apply_bomb_bag_slot(message.value("bag", -1), message.value("item", -1),
                                   message.value("count", -1)) ?
                   ApplyResult::Applied : ApplyResult::IgnoredByPolicy;
    }

    return reject("unhandled progression type: " + type);
}

bool GameAdapter::floor_switch_sync_active() const {
    // A native plate may itself request a pause/demo. Do not drop its pressure
    // just because event_runCheck() becomes true; only transitions are unsafe.
    return syncFlagsEnabled_ && transport_.status().welcomed && !opening_or_title_active() &&
           !manualTransitionActive_ && !manualReloadPending_ &&
           dComIfGp_getStageStagInfo() != nullptr && dComIfGp_getPlayer(0) != nullptr &&
           !dComIfGp_isEnableNextStage() && !fopOvlpM_IsPeek() && !fopOvlpM_IsDoingReq();
}

bool GameAdapter::ooccoo_sync_active() const {
    return syncFlagsEnabled_ && transport_.status().welcomed && !opening_or_title_active();
}

void GameAdapter::bind_ooccoo_to_save() {
    if (ooccooBoundToSave_ || opening_or_title_active()) return;
    const auto facts = native_ooccoo_facts(true, stage_ready());
    ooccooState_.observe_native_completion(facts.completed);
    ooccooState_.seed(facts);
    ooccooBoundToSave_ = true;
}

void GameAdapter::flush_ooccoo_catchup() {
    if ((!ooccooCatchupPending_ && !ooccooReplyPending_) || !ooccoo_sync_active() ||
        !stage_ready() || manualReloadPending_) return;
    nlohmann::json packet = {{"type", "ooccoo_state"}, {"state", ooccoo_snapshot_state()}};
    // The request is an envelope field, never part of the saved receipt set.
    // A newcomer that joined at the title screen requests again on save load.
    // Replies do not request another reply; simultaneous joins cannot loop.
    if (ooccooCatchupPending_) packet["request_state"] = true;
    publish_local(std::move(packet));
    ooccooCatchupPending_ = false;
    ooccooReplyPending_ = false;
}

nlohmann::json GameAdapter::ooccoo_snapshot_state() {
    bind_ooccoo_to_save();
    // Snapshot serialization does not infer an owner from the displayed item
    // or clear a receipt. The native warp-out hook records its mark explicitly.
    auto snapshot = ooccooState_.progress();
    snapshot.completed = native_ooccoo_facts(false, stage_ready()).completed;
    return ooccoo::encode(snapshot);
}

bool GameAdapter::accept_ooccoo_state(const nlohmann::json& state) {
    const auto decoded = ooccoo::decode(state);
    if (!decoded || !ooccoo_sync_active()) return false;
    bind_ooccoo_to_save();
    ooccooState_.observe_native_completion(
        native_ooccoo_facts(false, stage_ready()).completed);
    if (ooccooState_.merge_remote(*decoded)) {
        const auto merged = ooccooState_.progress();
        svc_log->info(mod_ctx, ("OOCCOO remote progress acquired=" +
            std::to_string(merged.acquired) + " completed=" +
            std::to_string(merged.completed) + " pending=" +
            std::to_string(ooccooState_.pending())).c_str());
    }
    return true;
}

void GameAdapter::apply_shared_ooccoo_local_form() {
    // Metadata can arrive during loading. Only this safe, game-thread boundary
    // writes local flags/inventory; never fight a get-item demo or warp menu.
    if (!ooccoo_sync_active() || !stage_ready() || manualReloadPending_ ||
        stableRoomTicks_ < kRemoteSwitchRoomInitTicks || dComIfGp_isPauseFlag() ||
        dMeter2Info_getPauseStatus() != 0 || dMeter2Info_getWarpStatus() != 0 ||
        dComIfGp_isEnableNextStage() || fopOvlpM_IsPeek() || fopOvlpM_IsDoingReq() ||
        dComIfGp_getPlayer(0) == nullptr) return;
    const auto scene = current_ooccoo_scene();
    if (scene.name.empty() || scene.room < 0 || scene.room > 63 ||
        stableStageName_ != scene.name || stableRoom_ != scene.room) return;
    bind_ooccoo_to_save();
    const auto nativeFacts = native_ooccoo_facts(false, true);
    const bool newlyCompleted =
        (nativeFacts.completed & ~ooccooState_.progress().completed) != 0;
    ooccooState_.observe_native_completion(nativeFacts.completed);
    ooccooState_.seed(nativeFacts);
    if (newlyCompleted && !applyingRemote_) {
        svc_log->info(mod_ctx, ("OOCCOO local completion mask=" +
            std::to_string(nativeFacts.completed)).c_str());
        publish_local({{"type", "ooccoo_state"}, {"state", ooccoo_snapshot_state()}});
    }
    const auto progress = ooccooState_.progress();
    RemoteApplicationGuard applying(applyingRemote_);
    const int here = ooccoo::scene_dungeon(scene);
    for (int stage = ooccoo::FirstDungeon; stage <= ooccoo::CityDungeon; ++stage) {
        const auto bit = ooccoo::dungeon_bit(stage);
        auto applyFacts = [&](dSv_memBit_c& bits) {
            // Ooccoo completion controls her form, not the game's boss-clear flag.
            // A newly loaded save may legitimately have that flag unset.
            if ((progress.completed & bit) == 0 && (progress.acquired & bit) != 0) {
                if (!bits.isDungeonItemWarp()) bits.onDungeonItemWarp();
            }
        };
        applyFacts(g_dComIfG_gameInfo.info.getSavedata().getSave(stage).getBit());
        // Don't use the broad dComIfGs stage wrapper: only an identified
        // dungeon scene may receive live Ooccoo flags, even if a modded cave
        // reuses that save-table number.
        if (here == stage) applyFacts(g_dComIfG_gameInfo.info.getMemory().getBit());
    }
    if (progress.acquired != 0 && !dComIfGs_isItemFirstBit(dItemNo_DUNGEON_EXIT_e))
        dComIfGs_onItemFirstBit(dItemNo_DUNGEON_EXIT_e);
    if (progress.citySpecial && !dComIfGs_isItemFirstBit(dItemNo_LV7_DUNGEON_EXIT_e))
        dComIfGs_onItemFirstBit(dItemNo_LV7_DUNGEON_EXIT_e);
    if (progress.unboundNote && !dComIfGs_isItemFirstBit(dItemNo_TKS_LETTER_e))
        dComIfGs_onItemFirstBit(dItemNo_TKS_LETTER_e);

    const int item = dComIfGs_getItem(SLOT_18, false);
    const auto projection = ooccooState_.reconcile(
        scene, item, local_ooccoo_return_owner(), true);
    if (projection.resetReturn) dComIfGs_resetLastWarpAcceptStage();
    if (ooccoo::is_dungeon(projection.installReturnStage)) {
        const int stage = projection.installReturnStage;
        const auto& anchor = progress.anchors[stage - ooccoo::FirstDungeon];
        if (ooccoo::valid_anchor(stage, anchor)) {
            std::string name(ooccoo::DungeonNames[stage - ooccoo::FirstDungeon]);
            if (anchor.variant == 1) name.push_back('B');
            dComIfGs_setLastWarpAcceptStage(static_cast<s8>(stage));
            dComIfGs_setWarpItemData(name.c_str(), cXyz(anchor.x, anchor.y, anchor.z),
                                     anchor.angle, static_cast<s8>(anchor.room), 0, 1);
            svc_log->info(mod_ctx, ("OOCCOO installed Jr return stage=" +
                std::to_string(stage) + " room=" + std::to_string(anchor.room)).c_str());
        }
    }
    if (projection.applied && projection.item != item) {
        svc_log->info(mod_ctx, ("OOCCOO projected item " + std::to_string(item) +
            " -> " + std::to_string(projection.item) +
            " managed=" + std::to_string(ooccooState_.managed_form())).c_str());
        dComIfGs_setItem(SLOT_18, static_cast<u8>(projection.item));
    }
}

nlohmann::json GameAdapter::make_save_snapshot() {
    using nlohmann::json;
    json eventFlags = json::array();
    for (int byte = 0; byte < 256; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            // TP encodes the byte index in the high byte and a bit mask
            // in the low byte, rather than a sequential bit number.
            const auto flag = static_cast<uint16_t>((byte << 8) | (1U << bit));
            if (!is_unsynced_event_bit(flag) && dComIfGs_isEventBit(flag))
                eventFlags.push_back(flag);
        }
    }

    json chestStages = json::array();
    json switchStages = json::array();
    json itemStages = json::array();
    json dungeonStages = json::array();
    json keyCounts = json::array();
    for (int stage = 0; stage < dSv_save_c::STAGE_MAX; ++stage) {
        dSv_memBit_c& bits = stage_bits(stage);
        json chests = json::array();
        for (int flag = 0; flag < 64; ++flag) {
            if (bits.isTbox(flag)) chests.push_back(flag);
        }
        if (!chests.empty()) chestStages.push_back({{"stage", stage}, {"flags", chests}});

        json switches = json::array();
        for (int flag = 0; flag < dSv_info_c::MEMORY_SWITCH; ++flag) {
            if (!is_unsynced_switch_bit(stage, flag) && bits.isSwitch(flag) &&
                !loaded_floor_switch_owns_flag(stage, flag)) {
                switches.push_back(flag);
            }
        }
        if (!switches.empty()) switchStages.push_back({{"stage", stage}, {"flags", switches}});

        std::set<int> itemFlags;
        for (int flag = 0; flag < dSv_info_c::DAN_ITEM; ++flag) {
            if (bits.isItem(flag)) itemFlags.insert(flag);
        }
        const auto observed = observedMemoryItems_.find(stage);
        if (observed != observedMemoryItems_.end()) {
            itemFlags.insert(observed->second.begin(), observed->second.end());
        }
        json items = json::array();
        for (const int flag : itemFlags) items.push_back(flag);
        if (!items.empty()) itemStages.push_back({{"stage", stage}, {"flags", items}});

        json kinds = json::array();
        if (dComIfGs_isDungeonItemMap(stage)) kinds.push_back(0);
        if (dComIfGs_isDungeonItemCompass(stage)) kinds.push_back(1);
        if (dComIfGs_isDungeonItemBossKey(stage)) kinds.push_back(2);
        if (dComIfGs_isStageBossEnemy(stage)) kinds.push_back(3);
        if (dComIfGs_isStageLife(stage)) kinds.push_back(4);
        if (dComIfGs_isStageBossDemo(stage)) kinds.push_back(5);
        if (dComIfGs_isStageMiddleBoss(stage)) kinds.push_back(7);
        if (!kinds.empty()) dungeonStages.push_back({{"stage", stage}, {"kinds", kinds}});

        const int keys = bits.getKeyNum();
        if (keys > 0 && !is_local_escort_key_count(stage))
            keyCounts.push_back({{"stage", stage}, {"count", keys}});
    }

    json lightCounts = json::array();
    json lightFlags = json::array();
    for (int area = 0; area < 4; ++area) {
        const int count = dComIfGs_getLightDropNum(static_cast<u8>(area));
        if (count > 0) lightCounts.push_back({{"area", area}, {"count", count}});
        if (area < 3 && dComIfGs_isLightDropGetFlag(static_cast<u8>(area))) {
            lightFlags.push_back(area);
        }
    }

    json keyItems = json::array();
    for (int item = 0; item < 256; ++item) {
        if (is_synced_key_item(item) && dComIfGs_isItemFirstBit(static_cast<u8>(item))) {
            keyItems.push_back(item);
        }
    }

    json fish = json::array();
    for (int index = 0; index < kSyncedFishSpeciesCount; ++index) {
        fish.push_back({{"index", index},
                        {"count", dComIfGs_getFishNum(static_cast<u8>(index))},
                        {"max_size", dComIfGs_getFishSize(static_cast<u8>(index))}});
    }

    json crystals = json::array(), mirrors = json::array();
    json dark = json::array(), transforms = json::array(), regions = json::array();
    json clothing = json::array(), swords = json::array(), shields = json::array();
    for (int index = 0; index < 8; ++index) {
        if (dComIfGs_isCollectCrystal(static_cast<u8>(index))) crystals.push_back(index);
        if (dComIfGs_isCollectMirror(static_cast<u8>(index))) mirrors.push_back(index);
        if (index <= 3 && dComIfGs_isDarkClearLV(index)) dark.push_back(index);
        if (index == 3 && dComIfGs_isEventBit(dSv_event_flag_c::M_071) &&
            dComIfGs_isTransformLV(index)) transforms.push_back(index);
        if (dComIfGs_isRegionBit(index)) regions.push_back(index);
        if (dComIfGs_isCollectClothing(static_cast<u8>(index))) clothing.push_back(index);
        if (dComIfGs_isCollectSword(static_cast<u8>(index))) swords.push_back(index);
        if (dComIfGs_isCollectShield(static_cast<u8>(index))) shields.push_back(index);
    }

    json letters = json::array();
    for (int index = 0; index < LETTER_INFO_BIT; ++index) {
        if (dComIfGs_isLetterGetFlag(index)) letters.push_back(index);
    }

    json snapshot = {
        {"type", "save_snapshot"}, {"event_flags", eventFlags},
        {"chests", chestStages}, {"switches", switchStages}, {"items", itemStages},
        {"dungeon_items", dungeonStages}, {"key_counts", keyCounts},
        {"light_drop_counts", lightCounts}, {"light_drop_get_flags", lightFlags},
        {"key_items", keyItems}, {"bomb_bag_slots", bomb_bag_slots_snapshot()},
        {"crystals", crystals}, {"mirrors", mirrors}, {"dark_clear_levels", dark},
        {"transform_levels", transforms}, {"region_bits", regions},
        {"collect_clothing", clothing}, {"collect_sword", swords},
        {"collect_shield", shields}, {"letter_get_flags", letters},
        {"max_life", dComIfGs_getMaxLife()}, {"bottle_slots", bottle_slot_count()},
        {"bottle_sources", completedBottleSources_},
        {"bottle_sources_complete", bottleSourcesComplete_},
        {"rupees", dComIfGs_getRupee()}, {"poe_count", dComIfGs_getPohSpiritNum()},
        {"malo_fundraising", {{"phase", malo_fundraising_phase()},
                               {"value", dMsgObject_getFundRaising()}}},
        {"charlo_offering", dMsgObject_getOffering()}, {"fish_records", fish},
        {"collect_smell", raw_collect_smell()},
    };
    if (!randomizer_active()) {
        const int tradePhase = observed_trade_phase();
        if (tradePhase >= 0) {
            snapshot["trade_item"] = {{"phase", tradePhase},
                                      {"item", trade_item::item_for_phase(tradePhase)}};
        }
    }

    snapshot["ooccoo_state"] = ooccoo_snapshot_state();
    return snapshot;
}

nlohmann::json GameAdapter::make_recovery_snapshot() {
    nlohmann::json snapshot = make_save_snapshot();
    snapshot["item_first_bits"] = nlohmann::json::array();
    for (int item = 0; item < 256; ++item)
        if (is_synced_item_first_bit(item) &&
            dComIfGs_isItemFirstBit(static_cast<u8>(item)))
            snapshot["item_first_bits"].push_back(item);
    snapshot["visited_rooms"] = nlohmann::json::array();
    for (int stage = 0; stage < dSv_save_c::STAGE2_MAX; ++stage)
        for (int room = 0; room < 64; ++room)
            if (dComIfGs_isSaveVisitedRoom(stage, room))
                snapshot["visited_rooms"].push_back({{"stage", stage}, {"room", room}});
    return snapshot;
}

void GameAdapter::record_recovery_intent(const RoutedMessage& message, const RecoveryFact& fact,
                                        const nlohmann::json& value) {
    if (!saveRecoveryEnabled_ || !saveRecoveryPolicyReady_ || !saveRecovery_ ||
        !activeSaveSlot_ || saveRecovery_->slot != *activeSaveSlot_ ||
        saveRecovery_->saveName != activeSaveName_ ||
        message.receivedAt < saveRecovery_->savedAt ||
        message.receivedAt >= saveRecovery_->captureUntil || randomizer_active()) return;
    saveRecovery_->delta.observe_intent(fact, value);
    if (replayingSaveRecovery_) recoveryReplayIntents_[fact] = value;
    if (saveRecovery_->delta.size() > kMaxRecoveryFacts) {
        svc_log->warn(mod_ctx, "Save recovery journal exceeded its fact limit; discarding it");
        saveRecovery_.reset();
    }
}

void GameAdapter::record_received_progress(const nlohmann::json& before,
                                            ApplyResult result) {
    if (result != ApplyResult::Applied || !saveRecovery_) return;
    const nlohmann::json after = make_recovery_snapshot();
    saveRecovery_->delta.observe(before, after);
    if (saveRecovery_->delta.size() > kMaxRecoveryFacts) {
        svc_log->warn(mod_ctx, "Save recovery journal exceeded its fact limit; discarding it");
        saveRecovery_.reset();
    }
}

void GameAdapter::restore_saved_progress_if_ready() {
    if (!saveRecoveryEnabled_ || !saveRecovery_ || !saveRecovery_->restorePending ||
        !stage_ready() || opening_or_title_active() || manualTransitionActive_ ||
        manualReloadPending_) return;
    const char* name = mDoMemCd_GetFileName();
    if (name == nullptr || saveRecovery_->saveName != name) {
        saveRecovery_->restorePending = false;
        return;
    }
    saveRecovery_->restorePending = false;
    const RecoveryFacts changes = saveRecovery_->delta.applicable(make_recovery_snapshot());
    std::deque<RoutedMessage> titleMessages = std::move(saveRecovery_->titleMessages);
    saveRecovery_->titleMessages.clear();
    saveRecovery_->titleMessageBytes = 0;
    if (changes.empty() && titleMessages.empty()) return;

    nlohmann::json snapshot = {{"type", "save_snapshot"}};
    RemoteApplicationGuard applying(applyingRemote_);
    RemoteApplicationGuard replaying(replayingSaveRecovery_);
    recoveryReplayIntents_.clear();
    for (const auto& [fact, value] : changes) {
        const std::string& field = fact.field;
        if (field == "event_flags") {
            if (fact.key < 0 || fact.key > 0xFFFF ||
                is_unsynced_event_bit(static_cast<uint16_t>(fact.key))) continue;
            RoutedMessage event{"save_recovery",
                                {{"type", "event_bit"}, {"flag", fact.key}, {"set", !value.is_null()}},
                                {MessageDomain::Progression, false, true}, {}};
            (void)apply_event_bit(event);
        } else if (field == "switches") {
            if (!valid_stage(fact.key) || fact.subkey < 0 ||
                fact.subkey >= dSv_info_c::MEMORY_SWITCH ||
                is_unsynced_switch_bit(fact.key, fact.subkey) ||
                loaded_floor_switch_owns_flag(fact.key, fact.subkey)) continue;
            (void)apply_switch_bit({{"type", "switch_bit"}, {"stage", fact.key},
                                    {"flag", fact.subkey}, {"set", !value.is_null()}},
                                   "save_recovery");
        } else if (field == "item_first_bits") {
            if (fact.key < 0 || fact.key > 0xFF || !is_synced_item_first_bit(fact.key)) continue;
            if (value.is_null()) dComIfGs_offItemFirstBit(static_cast<u8>(fact.key));
            else dComIfGs_onItemFirstBit(static_cast<u8>(fact.key));
        } else if (field == "visited_rooms") {
            if (!value.is_null() && fact.key >= 0 && fact.key < dSv_save_c::STAGE2_MAX &&
                fact.subkey >= 0 && fact.subkey < 64)
                dComIfGs_onSaveVisitedRoom(fact.key, fact.subkey);
        } else if (field == "chests" || field == "items" || field == "dungeon_items") {
            if (value.is_null() || !valid_stage(fact.key)) continue;
            const char* member = field == "dungeon_items" ? "kinds" : "flags";
            snapshot[field].push_back({{"stage", fact.key}, {member, {fact.subkey}}});
        } else if (field == "key_counts" || field == "light_drop_counts") {
            const char* index = field == "key_counts" ? "stage" : "area";
            snapshot[field].push_back({{index, fact.key},
                                       {"count", value.is_null() ? 0 : value.value("count", 0)}});
        } else if (field == "bomb_bag_slots" || field == "fish_records") {
            if (!value.is_null()) snapshot[field].push_back(value);
        } else if (field == "ooccoo_state") {
            if (!value.is_object()) continue;
            const auto decoded = ooccoo::decode(value);
            if (!decoded) continue;
            bind_ooccoo_to_save();
            ooccooState_.observe_native_completion(
                native_ooccoo_facts(false, stage_ready()).completed);
            ooccooState_.merge_remote(*decoded);
            snapshot[field] = value;
        } else if (field == "key_items" || field == "crystals" || field == "mirrors" ||
                   field == "dark_clear_levels" || field == "transform_levels" ||
                   field == "region_bits" || field == "collect_clothing" ||
                   field == "collect_sword" || field == "collect_shield" ||
                   field == "letter_get_flags" || field == "light_drop_get_flags") {
            if (!value.is_null()) snapshot[field].push_back(fact.key);
        } else if (!value.is_null()) {
            snapshot[field] = value;
        }
    }
    if ((snapshot.contains("bottle_sources") ||
         snapshot.contains("bottle_sources_complete")) &&
        !snapshot.contains("bottle_slots"))
        snapshot["bottle_slots"] = bottle_slot_count();
    if (snapshot.size() > 1) {
        RoutedMessage routed{"save_recovery", std::move(snapshot),
                             {MessageDomain::Progression, true, true}, {}};
        const ApplyResult result = apply_save_snapshot(routed);
        if (result != ApplyResult::Applied)
            svc_log->warn(mod_ctx, "Save recovery snapshot could not be applied");
    }
    size_t replayed = 0;
    const nlohmann::json beforeTitle = titleMessages.empty() ? nlohmann::json() : make_recovery_snapshot();
    for (const RoutedMessage& message : titleMessages) {
        if (consume(message) == ApplyResult::Applied) ++replayed;
    }
    if (!titleMessages.empty()) record_received_progress(beforeTitle, ApplyResult::Applied);
    // A later deferred update wins over an earlier applied value in this
    // batch, even though it is not visible in native memory yet.
    if (saveRecovery_)
        for (const auto& [fact, value] : recoveryReplayIntents_)
            saveRecovery_->delta.observe_intent(fact, value);
    recoveryReplayIntents_.clear();
    poll_local_state(false);
    svc_log->info(mod_ctx, ("Save recovery applied facts=" +
        std::to_string(changes.size()) + " title_messages=" +
        std::to_string(replayed) + "/" +
        std::to_string(titleMessages.size())).c_str());
}

void GameAdapter::send_snapshot_to(std::string_view peerId, bool manual, bool flagsOnly,
                                   uint64_t requestId) {
    nlohmann::json snapshot = make_save_snapshot();
    if (manual) {
        const std::string fullState = encode_manual_full_state();
        if (fullState.empty()) {
            lastError_ = "could not encode manual sync state";
            svc_log->warn(mod_ctx, "Manual sync reply could not encode save state");
            return;
        }
        snapshot["manual_sync"] = true;
        snapshot["manual_sync_mode"] = flagsOnly ? "flags" : "warp";
        snapshot["full_state"] = fullState;
        if (requestId != 0) snapshot["manual_sync_request_id"] = requestId;
    }
    if (peerId.empty()) {
        transport_.send(snapshot);
    } else {
        const bool sent = transport_.send_to(std::string(peerId), snapshot);
        if (manual) {
            const std::string line = "Manual sync reply id=" + std::to_string(requestId) +
                " peer=" + std::string(peerId) +
                " mode=" + (flagsOnly ? "flags" : "warp") +
                " send=" + (sent ? "ok" : "failed");
            if (sent) svc_log->info(mod_ctx, line.c_str());
            else svc_log->warn(mod_ctx, line.c_str());
        }
    }
}

std::string GameAdapter::encode_manual_full_state() {
    const char* stageName = dComIfGp_getStartStageName();
    if (stageName == nullptr || stageName[0] == '\0') return {};
    ManualSyncStatePacket packet{};
    std::strncpy(packet.stageName, stageName, sizeof(packet.stageName) - 1);
    packet.roomNo = static_cast<int8_t>(dComIfGp_roomControl_getStayNo());
    if (packet.roomNo < 0) packet.roomNo = dComIfGp_getStartStageRoomNo();
    packet.layer = dComIfGp_getStartStageLayer();
    packet.startPoint = -1;
    if (packet.roomNo < 0 || packet.roomNo >= 64 || packet.layer < -1 || packet.layer >= 15)
        return {};

    dSv_info_c snapshotInfo = g_dComIfG_gameInfo.info;
    sanitize_floor_switch_snapshot(snapshotInfo);
    const int stage = current_stage_table();
    if (valid_stage(stage)) snapshotInfo.getSavedata().putSave(stage, snapshotInfo.getMemory());

    auto& snapshotPlayer = snapshotInfo.getPlayer();
    snapshotPlayer.getPlayerLastMarkInfo().init();
    auto& snapshotItem = snapshotPlayer.getItem().mItems[SLOT_18];
    if (ooccoo::is_form(snapshotItem)) snapshotItem = dItemNo_TKS_LETTER_e;

    std::vector<uint8_t> raw(kManualSyncStatePacketSize);
    std::memcpy(raw.data(), &packet, sizeof(packet));
    std::memcpy(raw.data() + sizeof(packet), &snapshotInfo, sizeof(snapshotInfo));
    std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const size_t size = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
    if (ZSTD_isError(size)) return {};
    compressed.resize(size);
    return base64_encode(compressed);
}

bool GameAdapter::apply_manual_full_state(const std::string& encoded, bool flagsOnly,
                                          std::string_view peerId) {
    if (manualTransitionActive_ || manualReloadPending_ || !pendingManualInfo_.empty() ||
        !pendingManualFlagsSave_.empty()) return false;
    std::vector<uint8_t> compressed;
    if (!base64_decode(encoded, compressed)) return false;
    const unsigned long long frameSize =
        ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (frameSize != kManualSyncStatePacketSize) return false;
    std::vector<uint8_t> raw(static_cast<size_t>(frameSize));
    const size_t decoded = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(decoded) || decoded != raw.size()) return false;

    ManualSyncStatePacket packet{};
    std::memcpy(&packet, raw.data(), sizeof(packet));
    packet.stageName[sizeof(packet.stageName) - 1] = '\0';
    if (packet.stageName[0] == '\0' || packet.roomNo < 0 || packet.roomNo >= 64 ||
        packet.layer < -1 || packet.layer >= 15 || packet.startPoint < -4 ||
        packet.startPoint > 255) return false;

    const bool expectedCueReply = !flagsOnly && !awaitingManualSyncCueKey_.empty() &&
        awaitingManualSyncPeerId_ == peerId;
    if (expectedCueReply) {
        if (const ProgressionCueDescriptor* descriptor =
                progression_cue_descriptor(awaitingManualSyncCueKey_)) {
            std::memset(packet.stageName, 0, sizeof(packet.stageName));
            std::memcpy(packet.stageName, descriptor->warpStage.data(),
                        std::min(descriptor->warpStage.size(), sizeof(packet.stageName) - 1));
            packet.roomNo = descriptor->warpRoom;
            packet.layer = descriptor->warpLayer;
            packet.startPoint = descriptor->warpStartPoint;
        }
        handledProgressionCues_.insert(awaitingManualSyncPeerId_ + ':' +
                                       awaitingManualSyncCueKey_);
        awaitingManualSyncCueKey_.clear();
        awaitingManualSyncPeerId_.clear();
        awaitingManualSyncRequestId_ = 0;
    }

    dSv_info_c peerInfo{};
    std::memcpy(&peerInfo, raw.data() + sizeof(packet), sizeof(peerInfo));
    int currentStage = -1;
    if (flagsOnly) {
        if (!stage_ready() || opening_or_title_active()) return false;
        currentStage = current_stage_table();
        if (!valid_stage(currentStage)) return false;
    }

    // Raw save transfer must not copy the donor's personal return mark. The
    // separate Ooccoo receipt can provide a bounded dungeon anchor instead.
    // Sanitize before copying, so deferred replay buffers are safe too.
    auto& donorPlayer = peerInfo.getPlayer();
    auto& donorItem = donorPlayer.getItem().mItems[SLOT_18];
    if (ooccoo::is_form(donorItem)) donorItem = dItemNo_TKS_LETTER_e;
    donorPlayer.getPlayerLastMarkInfo().init();
    if (flagsOnly && dComIfGs_getItem(SLOT_18, false) == dItemNo_DUNGEON_BACK_e &&
        local_ooccoo_return_owner() >= 0) {
        donorPlayer.getPlayerLastMarkInfo() =
            g_dComIfG_gameInfo.info.getPlayer().getPlayerLastMarkInfo();
        donorItem = dItemNo_DUNGEON_BACK_e;
    }

    // Manual full/flags sync replaces save data without invoking the save
    // observer. Clear observations and deferred work from the previous state
    // before installing the peer state, or periodic repair can undo the sync.
    // Other items/equipment retain exact replacement semantics. The deliberate
    // Ooccoo exception above prevents importing another player's return point.
    localWagonEscortStarted_ = false;
    saveRecovery_.reset();
    clear_replaced_save_progression_state();
    if (flagsOnly) {
        dSv_player_c& localPlayer = g_dComIfG_gameInfo.info.getPlayer();
        const dSv_player_return_place_c localReturnPlace = localPlayer.getPlayerReturnPlace();
        dSv_player_field_last_stay_info_c localLastStay =
            localPlayer.getPlayerFieldLastStayInfo();
        const dSv_player_config_c localConfig = localPlayer.getConfig();
        dSv_save_c syncedSave = peerInfo.getSavedata();
        // The field map's discovered-region bits live in the last-stay record.
        // Keep this player's location and discovered regions while adding the
        // donor's regions, as the normal snapshot and live region sync do.
        const auto& donorLastStay = syncedSave.getPlayer().getPlayerFieldLastStayInfo();
        for (int region = 0; region < 8; ++region) {
            if (donorLastStay.isRegionBit(region)) localLastStay.onRegionBit(region);
        }
        syncedSave.getPlayer().getPlayerReturnPlace() = localReturnPlace;
        syncedSave.getPlayer().getPlayerFieldLastStayInfo() = localLastStay;
        syncedSave.getPlayer().getConfig() = localConfig;
        ToggleAutoSaveHook::trampoline(false);
        g_dComIfG_gameInfo.info.setSavedata(syncedSave);
        g_dComIfG_gameInfo.info.setMemory(syncedSave.getSave(currentStage));
        repair_lantern_item_state();
        dComIfGs_setItem(SLOT_18, dComIfGs_getItem(SLOT_18, false));
        pendingManualFlagsSave_.resize(sizeof(dSv_save_c));
        std::memcpy(pendingManualFlagsSave_.data(),
                    &g_dComIfG_gameInfo.info.getSavedata(), sizeof(dSv_save_c));
        manualReloadPending_ = true;
        return true;
    }

    ToggleAutoSaveHook::trampoline(false);
    const u8 vibration = dComIfGs_getOptVibration();
    g_dComIfG_gameInfo.info = peerInfo;
    g_dComIfG_gameInfo.info.getPlayer().getConfig().setVibration(vibration);
    repair_lantern_item_state();
    dComIfGs_setItem(SLOT_18, dComIfGs_getItem(SLOT_18, false));
    pendingManualInfo_.resize(sizeof(dSv_info_c));
    std::memcpy(pendingManualInfo_.data(), &g_dComIfG_gameInfo.info, sizeof(dSv_info_c));
    pendingManualVibration_ = vibration;
    manualTransitionActive_ = true;
    const s16 spawnPoint = packet.startPoint == -4 ? -1 : packet.startPoint;
    svc_log->info(mod_ctx,
        ("Manual sync warp starting stage=" + std::string(packet.stageName) +
         " room=" + std::to_string(packet.roomNo)).c_str());
    if (spawnPoint == -1) {
        dComIfGs_setRestartRoomParam(
            daPy_py_c::setParamData(packet.roomNo, 0, kManualSyncDefaultStartEvent, 0));
    }
    dComIfGp_setNextStage(packet.stageName, spawnPoint, packet.roomNo, packet.layer,
                          0.0f, 0, 1, 0, 0, 1, 3);
    return true;
}

void GameAdapter::update_pending_sync_replies() {
    if (pendingSyncReplies_.empty()) return;
    const bool baseSafe = stage_ready() && !opening_or_title_active() &&
                          fpcM_SearchByName(fpcNm_GAMEOVER_e) == nullptr;
    // The donor cannot know whether the requester timed out. Keep the request
    // until a safe snapshot is possible; the requester rejects late replies
    // using the original request ID.
    if (!baseSafe) return;
    for (auto it = pendingSyncReplies_.begin(); it != pendingSyncReplies_.end();) {
        if (it->waitTicks < std::numeric_limits<uint32_t>::max()) ++it->waitTicks;
        const bool timedOut = it->waitTicks >= kPendingSyncReplyTimeoutTicks;
        const bool ready = local_state_ready_for_cue(it->cueKey);
        if (!ready && !timedOut) {
            ++it;
            continue;
        }
        if (!ready) {
            std::ostringstream warning;
            warning << "Online queued sync request timed out waiting for cue="
                    << it->cueKey << "; replying with the current safe state";
            dusklight_online::log_info(warning.str());
        }
        if (!it->cueKey.empty()) {
            handledProgressionCues_.insert(it->peerId + ':' + it->cueKey);
        }
        std::ostringstream line;
        line << "Online replying to queued sync request peer=" << it->peerId
             << " mode=" << (it->flagsOnly ? "flags" : "warp")
             << " wait_ticks=" << it->waitTicks
             << " cue=" << (it->cueKey.empty() ? "manual" : it->cueKey);
        dusklight_online::log_info(line.str());
        send_snapshot_to(it->peerId, true, it->flagsOnly, it->requestId);
        it = pendingSyncReplies_.erase(it);
    }
}

void GameAdapter::tick_manual_transition() {
    if (manualReloadPending_ && engine_stage_ready() && !opening_or_title_active()) {
        manualReloadPending_ = false;
        manualTransitionActive_ = true;
        daPy_py_c::forceRestartRoom(0, 5, 0xC9);
        return;
    }
    if ((pendingManualInfo_.empty() && pendingManualFlagsSave_.empty()) ||
        !engine_stage_ready() ||
        opening_or_title_active()) return;
    RemoteApplicationGuard applying(applyingRemote_);
    if (!pendingManualInfo_.empty()) {
        if (pendingManualInfo_.size() != sizeof(dSv_info_c)) {
            pendingManualInfo_.clear();
            pendingManualVibration_.reset();
            manualTransitionActive_ = false;
            return;
        }
        std::memcpy(&g_dComIfG_gameInfo.info, pendingManualInfo_.data(), sizeof(dSv_info_c));
        pendingManualInfo_.clear();
        repair_lantern_item_state();
        if (pendingManualVibration_.has_value()) {
            dComIfGs_setOptVibration(*pendingManualVibration_);
            dComIfGp_setNowVibration(*pendingManualVibration_);
            pendingManualVibration_.reset();
        }
        dComIfGp_offOxygenShowFlag();
        dComIfGp_setMaxOxygen(600);
        dComIfGp_setOxygen(600);
    } else {
        if (pendingManualFlagsSave_.size() != sizeof(dSv_save_c)) {
            pendingManualFlagsSave_.clear();
            manualTransitionActive_ = false;
            return;
        }
        dSv_save_c save{};
        std::memcpy(&save, pendingManualFlagsSave_.data(), sizeof(save));
        g_dComIfG_gameInfo.info.setSavedata(save);
        pendingManualFlagsSave_.clear();
        repair_lantern_item_state();
    }
    dComIfGs_setItem(SLOT_18, dComIfGs_getItem(SLOT_18, false));
    manualTransitionActive_ = false;
    svc_log->info(mod_ctx, "Manual sync transition completed");
    pendingBombBagSaveWarp_.reset();
    localObservedState_ = nlohmann::json();
    observedRupees_ = dComIfGs_getRupee();
}

ApplyResult GameAdapter::apply_save_snapshot(const RoutedMessage& routed) {
    const nlohmann::json& message = routed.payload;
    if (message.value("manual_sync", false) && message.contains("full_state")) {
        const uint64_t requestId = manual_sync_request_id(message);
        if (!expecting_manual_sync_reply(routed.peerId, requestId)) {
            svc_log->warn(mod_ctx,
                ("Discarded unsolicited or expired manual sync reply id=" +
                 std::to_string(requestId) + " peer=" + routed.peerId).c_str());
            return ApplyResult::IgnoredByPolicy;
        }
        const bool flagsOnly = message.value("manual_sync_mode", "warp") == "flags";
        const bool titleWarp = !flagsOnly && manualSyncRequestedFromTitle_ &&
                               fpcM_SearchByName(fpcNm_TITLE_e) != nullptr;
        if (fpcM_SearchByName(fpcNm_GAMEOVER_e) != nullptr ||
            (!titleWarp && (!stage_ready() || opening_or_title_active()))) {
            return ApplyResult::Deferred;
        }
        // Validate BEFORE replacing any raw save data. Old manual snapshots
        // have no safe ownership model and are deliberately not accepted.
        const auto ooccooProgress = message.contains("ooccoo_state") ?
            ooccoo::decode(message["ooccoo_state"]) : std::nullopt;
        if (!ooccooProgress) return reject("manual sync requires Ooccoo protocol version 2");
        const bool applied = apply_manual_full_state(
            message.value("full_state", std::string()), flagsOnly, routed.peerId);
        if (applied) {
            replace_bottle_source_state(message);
            ooccooState_.restore(*ooccooProgress, 0,
                !flagsOnly && ooccoo::entitlements(*ooccooProgress) != 0);
            ooccooBoundToSave_ = true;
            ooccooCatchupPending_ = true;
            ooccooReplyPending_ = false;
        }
        if (manualSyncState_ == ManualSyncState::Waiting &&
            manualSyncPeerId_ == routed.peerId && manualSyncRequestId_ == requestId) {
            manualSyncState_ = applied ? ManualSyncState::Succeeded : ManualSyncState::Failed;
            manualSyncRequestedFromTitle_ = false;
        }
        svc_log->info(mod_ctx,
            ("Manual sync reply id=" + std::to_string(requestId) +
             " peer=" + routed.peerId +
             " mode=" + (flagsOnly ? "flags" : "warp") +
             " result=" + (applied ? "applied" : "rejected")).c_str());
        return applied ? ApplyResult::Applied : reject("manual sync state rejected");
    }

    // Normal catch-up hydrates raw save state. Recovery represents updates
    // received after this save, so it must use the usual live-event safety.
    const int localMaloPhaseBeforeSnapshot = malo_fundraising_phase();
    const bool snapshotOoccooAccepted = message.contains("ooccoo_state") &&
        accept_ooccoo_state(message["ooccoo_state"]);
    for (const auto& raw : message.value("event_flags", nlohmann::json::array())) {
        if (!raw.is_number_integer()) continue;
        const int value = raw.get<int>();
        if (value < 0 || value > 0xFFFF) continue;
        const uint16_t flag = static_cast<uint16_t>(value);
        if (!is_unsynced_event_bit(flag)) {
            if (replayingSaveRecovery_) {
                RoutedMessage event{routed.peerId,
                    {{"type", "event_bit"}, {"flag", flag}, {"set", true}},
                    {MessageDomain::Progression, false, true}, routed.ingress, routed.receivedAt};
                const auto result = apply_event_bit(event);
                if (result == ApplyResult::Retained || result == ApplyResult::Applied)
                    record_recovery_intent(routed, {"event_flags", flag}, true);
                continue;
            }
            if (flag == dSv_event_flag_c::M_035 && local_wagon_escort_unfinished()) {
                enqueue_unique_deferred_mutation(deferredStoryEvents_, {
                    {"type", "event_bit"}, {"flag", flag}, {"set", true},
                    {"_peer_id", routed.peerId},
                });
                record_recovery_intent(routed, {"event_flags", flag}, true);
                continue;
            }
            if (flag == 0x2B08 && !dComIfGs_isEventBit(flag)) {
                const char* currentStage = dComIfGp_getStartStageName();
                if (currentStage != nullptr && is_mirror_complete_reload_stage(currentStage))
                    mirrorReloadPending_ = true;
            }
            dComIfGs_onEventBit(flag);
        }
    }

    auto apply_stage_flags = [&](std::string_view field, int limit, auto apply) {
        for (const auto& entry : message.value(std::string(field), nlohmann::json::array())) {
            if (!entry.is_object()) continue;
            const int stage = entry.value("stage", -1);
            if (!valid_stage(stage)) continue;
            for (const auto& raw : entry.value("flags", nlohmann::json::array())) {
                if (!raw.is_number_integer()) continue;
                const int flag = raw.get<int>();
                if (flag >= 0 && flag < limit) apply(stage, flag);
            }
        }
    };
    apply_stage_flags("chests", 64, [](int stage, int flag) {
        const bool newlySet = !stage_bits(stage).isTbox(flag);
        stage_bits(stage).onTbox(flag);
        repair_remote_tbox_collectible(stage, flag, newlySet, false);
    });
    for (const auto& entry : message.value("switches", nlohmann::json::array())) {
        if (!entry.is_object()) continue;
        const int stage = entry.value("stage", -1);
        if (!valid_stage(stage)) continue;
        for (const auto& raw : entry.value("flags", nlohmann::json::array())) {
            if (!raw.is_number_integer()) continue;
            const int flag = raw.get<int>();
            const auto result = replayingSaveRecovery_ ?
                apply_switch_bit({{"type", "switch_bit"}, {"stage", stage},
                                  {"flag", flag}, {"set", true}}, routed.peerId) :
                apply_snapshot_switch_bit(stage, flag);
            if (result == ApplyResult::Retained ||
                (replayingSaveRecovery_ && result == ApplyResult::Applied))
                record_recovery_intent(routed, {"switches", stage, flag}, true);
        }
    }
    apply_stage_flags("items", dSv_info_c::DAN_ITEM, [this](int stage, int flag) {
        stage_bits(stage).onItem(flag);
        remember_memory_item(stage, flag);
        repair_remote_memory_item_collectible(stage, flag);
    });

    for (const auto& entry : message.value("dungeon_items", nlohmann::json::array())) {
        if (!entry.is_object()) continue;
        const int stage = entry.value("stage", -1);
        if (!valid_stage(stage)) continue;
        for (const auto& raw : entry.value("kinds", nlohmann::json::array())) {
            if (!raw.is_number_integer()) continue;
            switch (raw.get<int>()) {
            case 0: dComIfGs_onDungeonItemMap(stage); break;
            case 1: dComIfGs_onDungeonItemCompass(stage); break;
            case 2: dComIfGs_onDungeonItemBossKey(stage); break;
            case 3: dComIfGs_onStageBossEnemy(stage); break;
            case 4: dComIfGs_onStageLife(stage); break;
            case 5: dComIfGs_onStageBossDemo(stage); break;
            case 6: break; // Legacy raw Ooccoo bits must never poison another area.
            case 7: dComIfGs_onStageMiddleBoss(stage); break;
            default: break;
            }
        }
    }
    for (const auto& entry : message.value("key_counts", nlohmann::json::array())) {
        if (!entry.is_object()) continue;
        const int stage = entry.value("stage", -1), count = entry.value("count", -1);
        if (valid_stage(stage) && count >= 0 && count <= 99 &&
            !is_local_escort_key_count(stage)) {
            dComIfGs_setKeyNum(stage, static_cast<u8>(count));
        }
    }
    for (const auto& entry : message.value("light_drop_counts", nlohmann::json::array())) {
        if (!entry.is_object()) continue;
        const int area = entry.value("area", -1), count = entry.value("count", -1);
        if (area >= 0 && area <= 0xFF && count >= 0 && count <= 0xFF) {
            dComIfGs_setLightDropNum(static_cast<u8>(area), static_cast<u8>(count));
            if (area == 2 && count == 15) dComIfGs_onEventBit(dSv_event_flag_c::saveBitLabels[9]);
        }
    }
    for (const auto& raw : message.value("light_drop_get_flags", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < 3) {
            const int area = raw.get<int>();
            dComIfGs_onLightDropGetFlag(static_cast<u8>(area));
            dMeter2Info_setLightDropGetFlag(area, 0xFF);
        }
    }
    for (const auto& raw : message.value("key_items", nlohmann::json::array())) {
        if (!raw.is_number_integer()) continue;
        const int item = raw.get<int>();
        if (item >= 0 && item <= 0xFF && is_synced_key_item(item) &&
            !dComIfGs_isItemFirstBit(static_cast<u8>(item))) {
            if (!randomizer_active() && trade_item::item_phase(item) >= 0 &&
                observed_trade_phase() > trade_item::item_phase(item))
                record_older_trade_item(item);
            else
                execItemGet(static_cast<u8>(item), 0, nullptr);
        }
    }
    if (!randomizer_active() && message.contains("trade_item") &&
        message["trade_item"].is_object()) {
        const auto& trade = message["trade_item"];
        (void)apply_trade_state(trade.value("phase", -1), trade.value("item", -1));
    }
    repair_lantern_item_state();
    repair_current_stage_collectibles();
    for (const auto& entry : message.value("bomb_bag_slots", nlohmann::json::array())) {
        if (entry.is_object()) apply_bomb_bag_slot(entry.value("bag", -1),
            entry.value("item", -1), entry.value("count", -1));
    }
    for (const auto& raw : message.value("crystals", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < 8)
            dComIfGs_onCollectCrystal(static_cast<u8>(raw.get<int>()));
    }
    for (const auto& raw : message.value("mirrors", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < 8)
            dComIfGs_onCollectMirror(static_cast<u8>(raw.get<int>()));
    }
    for (const auto& raw : message.value("dark_clear_levels", nlohmann::json::array())) {
        if (!raw.is_number_integer()) continue;
        const int level = raw.get<int>();
        const auto result = apply_dark_clear(level);
        if (result == ApplyResult::Retained ||
            (replayingSaveRecovery_ && result == ApplyResult::Applied))
            record_recovery_intent(routed, {"dark_clear_levels", level}, true);
    }
    for (const auto& raw : message.value("transform_levels", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() == 3 &&
            dComIfGs_isEventBit(dSv_event_flag_c::M_071)) dComIfGs_onTransformLV(3);
    }
    for (const auto& raw : message.value("region_bits", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < 8)
            dComIfGs_onRegionBit(raw.get<int>());
    }
    auto apply_collect = [&](const char* field, int kind) {
        for (const auto& raw : message.value(field, nlohmann::json::array())) {
            if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < 8)
                g_dComIfG_gameInfo.info.getPlayer().getCollect().setCollect(
                    kind, static_cast<u8>(raw.get<int>()));
        }
    };
    apply_collect("collect_clothing", COLLECT_CLOTHING);
    apply_collect("collect_sword", COLLECT_SWORD);
    apply_collect("collect_shield", COLLECT_SHIELD);
    for (const auto& raw : message.value("letter_get_flags", nlohmann::json::array())) {
        if (raw.is_number_integer() && raw.get<int>() >= 0 && raw.get<int>() < LETTER_INFO_BIT)
            dComIfGs_onLetterGetFlag(raw.get<int>());
    }
    const int maxLife = message.value("max_life", 0);
    if (maxLife > dComIfGs_getMaxLife() && maxLife <= 100) dComIfGs_setMaxLife(static_cast<u8>(maxLife));
    if (!randomizer_active()) {
        const int bottles = message.value("bottle_slots", 0);
        const int localBottles = bottle_slot_count();
        int mergedBottles = std::max(localBottles, std::clamp(bottles, 0, 4));
        const bool hasBottleSourceState = message.contains("bottle_sources") &&
            message["bottle_sources"].is_array();
        const std::set<uint8_t> remoteBottleSources = hasBottleSourceState
            ? parse_bottle_sources(message["bottle_sources"])
            : std::set<uint8_t>{};
        const bool validRemoteSourceState = hasBottleSourceState &&
            static_cast<int>(remoteBottleSources.size()) <= bottles;
        if (validRemoteSourceState &&
            static_cast<int>(completedBottleSources_.size()) <= localBottles) {
            const bool remoteComplete = message.value("bottle_sources_complete", false) &&
                static_cast<int>(remoteBottleSources.size()) == bottles;
            const bool bothComplete = bottle_sources_exact(
                bottleSourcesComplete_, completedBottleSources_.size(), localBottles) &&
                remoteComplete;
            completedBottleSources_.insert(remoteBottleSources.begin(), remoteBottleSources.end());
            for (uint8_t source : remoteBottleSources) {
                remember_vanilla_bottle_source(source);
            }
            mergedBottles = merged_bottle_count(
                localBottles, bottles, completedBottleSources_.size());
            bottleSourcesComplete_ = bothComplete &&
                static_cast<int>(completedBottleSources_.size()) == mergedBottles;
        } else if (mergedBottles > localBottles) {
            // A legacy or incomplete snapshot can safely repair the absolute
            // count, but it cannot identify which source owns the new slot.
            bottleSourcesComplete_ = false;
        }
        for (int local = localBottles; local < mergedBottles; ++local) {
            dComIfGs_setEmptyBottle();
        }
    }
    const int rupees = message.value("rupees", -1);
    if (rupees >= 0 && rupees <= dComIfGs_getRupeeMax()) {
        dComIfGs_setRupee(static_cast<u16>(rupees));
        observedRupees_ = rupees;
    }
    const int poes = message.value("poe_count", -1);
    if (poes > dComIfGs_getPohSpiritNum() && poes <= MAX_POH_NUM)
        dComIfGs_setPohSpiritNum(static_cast<u8>(poes));
    const auto malo = message.value("malo_fundraising", nlohmann::json::object());
    if (malo.is_object()) {
        const int phase = malo.value("phase", -1), value = malo.value("value", -1);
        if (phase >= 0 && phase <= 2 && phase == malo_fundraising_phase() &&
            value >= 0 && value <= kMaxSyncedDonationTotal &&
            (phase > localMaloPhaseBeforeSnapshot || value > dMsgObject_getFundRaising())) {
            dMsgObject_setFundRaising(static_cast<u16>(value));
        }
    }
    const int offering = message.value("charlo_offering", -1);
    if (offering > dMsgObject_getOffering() && offering <= kMaxSyncedDonationTotal)
        dMsgObject_setOffering(static_cast<u16>(offering));
    for (const auto& entry : message.value("fish_records", nlohmann::json::array())) {
        if (!entry.is_object()) continue;
        const int index = entry.value("index", -1), count = entry.value("count", -1);
        const int size = entry.value("max_size", -1);
        if (index >= 0 && index < kSyncedFishSpeciesCount && count >= 0 &&
            count <= kMaxSyncedFishCount && size >= 0 && size <= 0xFF) {
            auto& fish = g_dComIfG_gameInfo.info.getPlayer().getFishingInfo();
            fish.mFishCount[index] = static_cast<u16>(std::max<int>(fish.getFishCount(index), count));
            fish.setMaxSize(index, static_cast<u8>(std::max<int>(fish.getMaxSize(index), size)));
        }
    }
    const int smell = message.value("collect_smell", -1);
    if (valid_collect_smell(smell) && collect_smell_priority(smell) >
        collect_smell_priority(raw_collect_smell())) {
        if (smell != dItemNo_NONE_e) dComIfGs_onItemFirstBit(static_cast<u8>(smell));
        dComIfGs_setCollectSmell(static_cast<u8>(smell));
    }
    if (snapshotOoccooAccepted) apply_shared_ooccoo_local_form();

    return ApplyResult::Applied;
}

void GameAdapter::poll_local_state(bool publish) {
    using nlohmann::json;
    json state = {
        {"keys", json::array()}, {"light", json::array()}, {"light_flags", json::array()},
        {"item_first", json::array()}, {"crystals", json::array()}, {"mirrors", json::array()},
        {"dark", json::array()}, {"transforms", json::array()}, {"regions", json::array()},
        {"clothing", json::array()}, {"swords", json::array()}, {"shields", json::array()},
        {"letters", json::array()}, {"fish_count", json::array()}, {"fish_size", json::array()},
        {"max_life", dComIfGs_getMaxLife()}, {"bottles", bottle_slot_count()},
        {"rupees", dComIfGs_getRupee()}, {"poes", dComIfGs_getPohSpiritNum()},
        {"malo_phase", malo_fundraising_phase()}, {"malo", dMsgObject_getFundRaising()},
        {"charlo", dMsgObject_getOffering()}, {"smell", raw_collect_smell()},
        {"bombs", nlohmann::json::array()},
    };
    const u8 rentalBag = dMeter2Info_getRentalBombBag();
    for (int bag = 0; bag < 3; ++bag) {
        state["bombs"].push_back({
            {"item", dComIfGs_getItem(SLOT_15 + bag, false)},
            {"count", dComIfGs_getBombNum(static_cast<u8>(bag))},
            {"rental", rentalBag != 0xFF && rentalBag == static_cast<u8>(bag)},
        });
    }
    for (int stage = 0; stage < dSv_save_c::STAGE_MAX; ++stage)
        state["keys"].push_back(stage_bits(stage).getKeyNum());
    for (int area = 0; area < 4; ++area)
        state["light"].push_back(dComIfGs_getLightDropNum(static_cast<u8>(area)));
    for (int area = 0; area < 3; ++area)
        state["light_flags"].push_back(bool(dComIfGs_isLightDropGetFlag(static_cast<u8>(area))));
    for (int item = 0; item < 256; ++item)
        state["item_first"].push_back(bool(dComIfGs_isItemFirstBit(static_cast<u8>(item))));
    for (int index = 0; index < 8; ++index) {
        state["crystals"].push_back(dComIfGs_isCollectCrystal(static_cast<u8>(index)));
        state["mirrors"].push_back(dComIfGs_isCollectMirror(static_cast<u8>(index)));
        state["dark"].push_back(bool(dComIfGs_isDarkClearLV(index)));
        state["transforms"].push_back(bool(dComIfGs_isTransformLV(index)));
        state["regions"].push_back(bool(dComIfGs_isRegionBit(index)));
        state["clothing"].push_back(bool(dComIfGs_isCollectClothing(static_cast<u8>(index))));
        state["swords"].push_back(bool(dComIfGs_isCollectSword(static_cast<u8>(index))));
        state["shields"].push_back(bool(dComIfGs_isCollectShield(static_cast<u8>(index))));
    }
    for (int index = 0; index < LETTER_INFO_BIT; ++index)
        state["letters"].push_back(bool(dComIfGs_isLetterGetFlag(index)));
    for (int index = 0; index < kSyncedFishSpeciesCount; ++index) {
        state["fish_count"].push_back(dComIfGs_getFishNum(static_cast<u8>(index)));
        state["fish_size"].push_back(dComIfGs_getFishSize(static_cast<u8>(index)));
    }
    if (!publish || !localObservedState_.is_object()) {
        localObservedState_ = std::move(state);
        return;
    }
    // Key mutations are published at their exact vanilla setter boundaries.
    // Polling every durable stage slot would misclassify off-stage engine
    // maintenance (for example the escort setup's stage-6 key write) as a
    // player progression event.
    const bool randomizerActive = randomizer_active();
    // Poe pickups are sent at the item grant. A sampled increase may instead
    // be remote progression received while stage readiness was false.
    for (int index = 0; index < kSyncedFishSpeciesCount; ++index)
        if (state["fish_size"][index] != localObservedState_["fish_size"][index])
            publish_local({{"type", "fish_record"}, {"index", index},
                {"count", state["fish_count"][index]}, {"max_size", state["fish_size"][index]}});
    if (!randomizerActive) {
        for (int bag = 0; bag < 3; ++bag) {
            const auto& current = state["bombs"][bag];
            const auto& previous = localObservedState_["bombs"][bag];
            const int item = current.value("item", -1);
            const int count = current.value("count", 0);
            if (should_publish_bomb_bag(randomizerActive, syncable_bomb_item(item),
                    current.value("rental", false), item, count,
                    previous.value("rental", false), previous.value("item", -1),
                    previous.value("count", 0))) {
                publish_local({{"type", "bomb_bag_slot"}, {"bag", bag},
                               {"item", item}, {"count", count}});
            }
        }
    }
    localObservedState_ = std::move(state);
}

ApplyResult GameAdapter::reject(std::string reason) {
    lastError_ = std::move(reason);
    return ApplyResult::Rejected;
}

}  // namespace dusklight_online::game
