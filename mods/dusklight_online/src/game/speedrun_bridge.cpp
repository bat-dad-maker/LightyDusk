#include "dusklight_online/game/speedrun_bridge.hpp"
#include "dusklight_online/logging.hpp"

#include "dusk/game_mode.hpp"
#include "dusk/speedrun.h"
#include "d/d_com_inf_game.h"
#include "d/d_save.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_scene_mng.h"
#include "f_op/f_op_overlap_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "JSystem/JUtility/JUTGamePad.h"
#include "m_Do/m_Do_MemCard.h"
#include "m_Do/m_Do_MemCardRWmng.h"
#include "m_Do/m_Do_Reset.h"
#include "mods/service.hpp"
#include "mods/svc/hook.h"

#include <array>
#include <cstring>
#include <type_traits>

namespace dusklight_online::game::speedrun {
namespace {

static_assert(sizeof(dSv_save_c) <= SAVEDATA_SIZE);
static_assert(std::is_trivially_copyable_v<dSv_save_c>);

dusk::gamemode::GameModeManager* manager = nullptr;
dusk::speedrun::SpeedrunInfo* timer = nullptr;
void (*nativeStart)() = nullptr;
void (*nativeReset)() = nullptr;
void (*nativeNewSave)(uint32_t) = nullptr;
void (*nativeCloseAllDocuments)() = nullptr;

#if defined(_WIN32)
constexpr const char* kManagerSymbol =
    "?g_GameModeManager@gamemode@dusk@@3VGameModeManager@12@A";
constexpr const char* kTimerSymbol =
    "?g_speedrunInfo@speedrun@dusk@@3USpeedrunInfo@12@A";
#else
constexpr const char* kManagerSymbol = "dusk::gamemode::g_GameModeManager";
constexpr const char* kTimerSymbol = "dusk::speedrun::g_speedrunInfo";
#endif

bool resolve_symbol(const char* name, HookSymbolFlags wanted, void** output) {
    HookSymbolFlags flags{};
    if (svc_hook->resolve(mod_ctx, name, output, &flags) == MOD_OK &&
        *output != nullptr && (flags & wanted) != 0) return true;
    log_info(std::string("Speedrun controls missing native symbol: ") + name);
    return false;
}

scene_class* current_start_scene() {
    constexpr std::array names{
        fpcNm_PLAY_SCENE_e, fpcNm_OPENING_SCENE_e, fpcNm_NAME_SCENE_e,
        fpcNm_NAMEEX_SCENE_e, fpcNm_MENU_SCENE_e, fpcNm_LOGO_SCENE_e,
    };
    for (const auto name : names) {
        if (auto* scene = fpcM_SearchByName(name))
            return reinterpret_cast<scene_class*>(scene);
    }
    return nullptr;
}

} // namespace

bool initialize() {
    void* address = nullptr;
    if (!resolve_symbol(kManagerSymbol, HOOK_SYMBOL_DATA, &address)) return false;
    manager = static_cast<dusk::gamemode::GameModeManager*>(address);
    address = nullptr;
    if (!resolve_symbol(kTimerSymbol, HOOK_SYMBOL_DATA, &address)) return false;
    timer = static_cast<dusk::speedrun::SpeedrunInfo*>(address);
    address = nullptr;
    if (!resolve_symbol("dusk::speedrun::start", HOOK_SYMBOL_CODE, &address)) return false;
    nativeStart = reinterpret_cast<void (*)()>(address);
    address = nullptr;
    if (!resolve_symbol("dusk::speedrun::reset", HOOK_SYMBOL_CODE, &address)) return false;
    nativeReset = reinterpret_cast<void (*)()>(address);
    address = nullptr;
    if (!resolve_symbol("dusk::mods::svc::save_slot_new", HOOK_SYMBOL_CODE, &address))
        return false;
    nativeNewSave = reinterpret_cast<void (*)(uint32_t)>(address);
    address = nullptr;
    if (resolve_symbol("dusk::ui::close_all_documents", HOOK_SYMBOL_CODE, &address))
        nativeCloseAllDocuments = reinterpret_cast<void (*)()>(address);
    return true;
}

bool mode_active() {
    return manager != nullptr && manager->isCurrentGameMode("vanilla_speedrun");
}

bool can_start_here() {
    return !mDoRst::isReset() && !fopOvlpM_IsPeek() &&
           current_start_scene() != nullptr;
}

bool reset_run() {
    if (!mode_active() || timer == nullptr || nativeReset == nullptr) return false;
    timer->reset();
    nativeReset();
    JUTGamePad::C3ButtonReset::sResetSwitchPushing = true;
    return true;
}

bool start_run() {
    if (!mode_active() || !can_start_here() || timer == nullptr ||
        nativeStart == nullptr || nativeNewSave == nullptr)
        return false;
    auto* scene = current_start_scene();
    if (scene == nullptr || fopScnM_ChangeReq(scene, fpcNm_PLAY_SCENE_e, 0, 5) == 0)
        return false;

    // Match the native new-save transition in dScnName_c::changeGameScene.
    dComIfGs_init();
    dComIfGs_setNoFile(0);
    dComIfGs_setDataNum(0);
    nativeNewSave(0);
    // File select normally refreshes this live flag after initializing the
    // new save. This direct start skips that step; match its rumble-on default.
    dComIfGs_setOptVibration(1);
    dComIfGp_setNowVibration(1);
    // dComIfGs_init supplies the localized default names and fresh state.
    dComIfGs_setSaveTotalTime(dComIfGs_getTotalTime());
    dComIfGs_setSaveStartTime(OSGetTime());
    dComIfGs_gameStart();
    dComIfGp_offEnableNextStage();
    dComIfGp_setNextStage("F_SP108", 21, 1, 13);
    dKy_clear_game_init();
    dComIfGs_resetDan();
    dComIfGs_setRestartRoomParam(0);
    timer->reset();
    nativeReset();
    nativeStart();
    return true;
}

bool close_menus_after_start() {
    if (nativeCloseAllDocuments == nullptr) return false;
    nativeCloseAllDocuments();
    return true;
}

void SaveProbe::begin() {
    clear();
    bytes_.resize(SAVEFILE_SIZE);
    if (!mode_active() ||
        std::strcmp(mDoMemCd_GetFileName(), "gczelda2-speedrun") != 0) {
        error_ = "Speedrun mode's save file is unavailable.";
        return;
    }
    phase_ = Phase::WaitingForCard;
    result_ = Result::Pending;
}

SaveProbe::Result SaveProbe::tick() {
    if (phase_ == Phase::Done || phase_ == Phase::Idle) return result_;
    if (!mode_active() ||
        std::strcmp(mDoMemCd_GetFileName(), "gczelda2-speedrun") != 0) {
        error_ = "Speedrun mode or save file changed during save check.";
        phase_ = Phase::Done;
        return result_ = Result::Error;
    }
    if (phase_ == Phase::WaitingForCard) {
        if (!mDoMemCd_isCardCommNone()) return Result::Pending;
        const auto status = mDoMemCd_getStatus(0);
        if (status == 14) return Result::Pending;
        if (status == 1) {
            // File select normally creates an empty card file before showing
            // the slots. Do not start if that initialization has not happened.
            error_ = "Open the Speedrun save slots once to create the save file.";
            phase_ = Phase::Done;
            return result_ = Result::Error;
        }
        if (status != 2) {
            error_ = "Speedrun save file is unavailable.";
            phase_ = Phase::Done;
            return result_ = Result::Error;
        }
        mDoMemCd_Load();
        phase_ = Phase::Loading;
        return Result::Pending;
    }
    const auto loaded = mDoMemCd_LoadSync(bytes_.data(), bytes_.size(), 0);
    if (loaded == 0) return Result::Pending;
    phase_ = Phase::Done;
    if (loaded != 1) {
        error_ = "Could not read Speedrun save slots.";
        return result_ = Result::Error;
    }
    for (int index = 0; index < SAVEDATA_NUM; ++index) {
        auto* slot = bytes_.data() + index * SAVEDATA_SIZE;
        if (!mDoMemCdRWm_TestCheckSumGameData(slot)) {
            error_ = "A Speedrun save slot has an invalid checksum.";
            return result_ = Result::Error;
        }
        // Slot stride is 0xA94, which does not preserve 8-byte alignment on
        // every slot. Copy before accessing the native save object on arm64.
        dSv_save_c save;
        std::memcpy(&save, slot, sizeof(save));
        if (*save.getPlayer().getPlayerInfo().getPlayerName() != '\0') {
            error_ = "All three Speedrun save slots must be empty.";
            return result_ = Result::Occupied;
        }
    }
    return result_ = Result::Clear;
}

void SaveProbe::clear() {
    phase_ = Phase::Idle;
    result_ = Result::Error;
    error_.clear();
}

} // namespace dusklight_online::game::speedrun
