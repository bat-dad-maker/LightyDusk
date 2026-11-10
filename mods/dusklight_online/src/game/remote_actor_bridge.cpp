#include "dusk/multiplayer/multiplayer.hpp"

#include "d/dolzel.h"
#include "dusklight_online/game/remote_actor_bridge.hpp"
#include "dusk/frame_interpolation.h"
#include "dusk/logging.h"
#include "f_pc/f_pc_base.h"
#include "f_pc/f_pc_create_iter.h"
#include "f_pc/f_pc_create_req.h"
#include "f_pc/f_pc_delete_tag.h"
#include "f_pc/f_pc_deletor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_profile.h"
#include "f_pc/f_pc_name.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "JSystem/JParticle/JPABaseShape.h"
#include "JSystem/JParticle/JPAEmitter.h"
#include "d/d_com_inf_game.h"
#include "dusklight_online/game/particle_depth.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <vector>

extern const actor_process_profile_definition g_profile_REMOTE_LINK;
int fpcDt_deleteMethod(base_process_class* process);

namespace dusklight_online::game {

DEFINE_HOOK(&fpcPf_Get, RemoteProfileLookupHook);
DEFINE_HOOK(&fpcBs_Create, RemoteProcessCreateHook);
DEFINE_HOOK(&fpcBs_Delete, RemoteProcessDeleteHook);
DEFINE_HOOK(&JPABaseShape::setGX, RemoteParticleDepthHook);
DEFINE_HOOK_SYMBOL("dusk::interp::begin_frame",
                   void(float), EngineInterpBeginFrameHook);
DEFINE_HOOK_SYMBOL("dusk::interp::begin_sim_tick",
                   void(), EngineInterpBeginSimTickHook);
DEFINE_HOOK_SYMBOL("dusk::interp::begin_presentation",
                   void(float), EngineInterpPresentationHook);
DEFINE_HOOK_SYMBOL("dusk::interp::lookup_replacement",
                   bool(const void*, float (*)[4]), EngineInterpLookupHook);
DEFINE_HOOK_SYMBOL("dusk::interp::lookup_concat_replacement",
                   bool(const void*, const void*, float (*)[4]),
                   EngineInterpLookupConcatHook);
DEFINE_HOOK_SYMBOL("dusk::interp::is_enabled",
                   bool(), EngineInterpIsEnabledSymbol);
DEFINE_HOOK_SYMBOL("dusk::game_clock::is_sim_frame",
                   bool(), EngineInterpIsSimFrameSymbol);
DEFINE_HOOK_SYMBOL("dusk::interp::get_interpolation_step",
                   float(), EngineInterpStepSymbol);
DEFINE_HOOK_SYMBOL("dusk::interp::presentation_sync_active",
                   bool(), EngineInterpPresentationSyncSymbol);
// Upstream also provides an overload with shared ownership. The unqualified
// manifest name is ambiguous; select the existing two-argument entry point.
#if defined(_MSC_VER)
#define ONLINE_INTERP_CALLBACK_SYMBOL "?add_interpolation_callback@interp@dusk@@YAXP6AXPEAX@Z0@Z"
#else
#define ONLINE_INTERP_CALLBACK_SYMBOL "_ZN4dusk6interp26add_interpolation_callbackEPFvPvES1_"
#endif
DEFINE_HOOK_SYMBOL(ONLINE_INTERP_CALLBACK_SYMBOL,
                   void(void (*)(void*), void*),
                   EngineInterpAddCallbackSymbol);
#undef ONLINE_INTERP_CALLBACK_SYMBOL

namespace {

constexpr s16 kRemoteLinkProfileId = static_cast<s16>(0x7FFE);
std::set<fpc_ProcID> sPendingRemoteCreates;
std::set<fpc_ProcID> sRemoteProcessIds;
std::map<fpc_ProcID, base_process_class*> sRemoteProcesses;
bool sResolvingRemoteProfile = false;
ParticleDepthOwners sDepthParticles;

void remote_particle_depth_post(ModContext*, void* args, void*, void*) {
    const auto* shape = mods::arg<const JPABaseShape*>(args, 0);
    const auto* work = mods::arg<JPAEmitterWorkData*>(args, 1);
    if (shape == nullptr || work == nullptr || work->mpEmtr == nullptr ||
        shape->getZEnable() || work->mpEmtr->getParticleCallBackPtr() != nullptr) return;
    if (sDepthParticles.contains(work->mpEmtr, [](uint32_t id) {
            return dComIfGp_particle_getEmitter(id);
        })) {
        GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_DISABLE);
    }
}

HookAction engine_interp_begin_frame_pre(ModContext*, void* args, void*, void*) {
    dusk::frame_interp::observe_engine_frame(dusk::frame_interp::is_enabled(),
                                             dusk::frame_interp::is_sim_frame(),
                                             mods::arg<float>(args, 0));
    return HOOK_CONTINUE;
}

HookAction engine_interp_begin_sim_tick_pre(ModContext*, void*, void*, void*) {
    dusk::frame_interp::begin_engine_sim_tick();
    return HOOK_CONTINUE;
}

void engine_interp_presentation_post(ModContext*, void*, void*, void*) {
    dusk::frame_interp::run_presentation_callbacks();
}

HookAction engine_interp_presentation_pre(ModContext*, void*, void*, void*) {
    dusk::frame_interp::prepare_presentation_callbacks();
    return HOOK_CONTINUE;
}

HookAction engine_interp_lookup_pre(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr) return HOOK_CONTINUE;
    const void* key = mods::arg<const void*>(args, 0);
    auto* out = mods::arg<float (*)[4]>(args, 1);
    if (key == nullptr || out == nullptr ||
        !dusk::frame_interp::lookup_local_replacement(key, out)) {
        return HOOK_CONTINUE;
    }
    *static_cast<bool*>(retval) = true;
    return HOOK_SKIP_ORIGINAL;
}

HookAction engine_interp_lookup_concat_pre(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr) return HOOK_CONTINUE;
    const void* lhs = mods::arg<const void*>(args, 0);
    const void* rhs = mods::arg<const void*>(args, 1);
    auto* out = mods::arg<float (*)[4]>(args, 2);
    if (lhs == nullptr || rhs == nullptr || out == nullptr ||
        !dusk::frame_interp::lookup_local_concat_replacement(lhs, rhs, out)) {
        return HOOK_CONTINUE;
    }
    *static_cast<bool*>(retval) = true;
    return HOOK_SKIP_ORIGINAL;
}

void* find_remote_create_request(void* requestPtr, void* processIdPtr) {
    auto* request = static_cast<create_request*>(requestPtr);
    auto* processId = static_cast<fpc_ProcID*>(processIdPtr);
    return request != nullptr && processId != nullptr && request->id == *processId
               ? request
               : nullptr;
}

bool finish_remote_process_delete(base_process_class* process) {
    if (process == nullptr) return true;
    if (process->state.init_state != 3 && fpcDt_Delete(process) == 0) return false;

    // fpcDtTg_Do normally waits one frame before invoking the delete method.
    // A mod image cannot leave its actor methods queued across DLL unload, so
    // bypass only that timer while the image is still resident.
    for (int attempt = 0; attempt < 4; ++attempt) {
        process->delete_tag.timer = 0;
        if (fpcDtTg_Do(&process->delete_tag,
                       reinterpret_cast<delete_tag_func>(&fpcDt_deleteMethod)) != 0) {
            return true;
        }
    }
    return false;
}

HookAction remote_profile_lookup_pre(ModContext*, void* args, void* retval, void*) {
    if (!sResolvingRemoteProfile ||
        mods::arg<s16>(args, 0) != kRemoteLinkProfileId || retval == nullptr) {
        return HOOK_CONTINUE;
    }
    *static_cast<const process_profile_definition**>(retval) =
        reinterpret_cast<const process_profile_definition*>(&g_profile_REMOTE_LINK);
    return HOOK_SKIP_ORIGINAL;
}

HookAction remote_process_create_pre(ModContext*, void* args, void*, void*) {
    const s16 profile = mods::arg<s16>(args, 0);
    const fpc_ProcID processId = mods::arg<fpc_ProcID>(args, 1);
    sResolvingRemoteProfile =
        profile == kRemoteLinkProfileId && sPendingRemoteCreates.contains(processId);
    return HOOK_CONTINUE;
}

void remote_process_create_post(ModContext*, void* args, void* retval, void*) {
    const fpc_ProcID processId = mods::arg<fpc_ProcID>(args, 1);
    if (sResolvingRemoteProfile) {
        sPendingRemoteCreates.erase(processId);
        if (retval != nullptr) {
            auto* process = *static_cast<base_process_class**>(retval);
            if (process != nullptr) sRemoteProcesses[processId] = process;
        }
    }
    sResolvingRemoteProfile = false;
}

void remote_process_delete_post(ModContext*, void* args, void* retval, void*) {
    if (retval == nullptr || *static_cast<int*>(retval) != 1) return;

    // fpcBs_Delete has freed the process by the time a post-hook runs. Compare
    // the address only; never dereference it here. A failed delete remains
    // tracked so shutdown can finish it while this DLL is still resident.
    auto* process = mods::arg<base_process_class*>(args, 0);
    dusk::frame_interp::remove_interpolation_callbacks_for(process);
    for (auto it = sRemoteProcesses.begin(); it != sRemoteProcesses.end(); ++it) {
        if (it->second == process) {
            const fpc_ProcID processId = it->first;
            sRemoteProcesses.erase(it);
            sRemoteProcessIds.erase(processId);
            sPendingRemoteCreates.erase(processId);
            return;
        }
    }
}

}  // namespace

fpc_ProcID create_remote_actor_process(u32 actorParams, const cXyz* position, int room,
                                       const csXyz* angle, const cXyz* scale, s8 argument) {
    const fpc_ProcID processId = fopAcM_create(
        kRemoteLinkProfileId, actorParams, position, room, angle, scale, argument, 0, 0xFF);
    if (processId != fpcM_ERROR_PROCESS_ID_e) {
        sPendingRemoteCreates.insert(processId);
        sRemoteProcessIds.insert(processId);
    }
    return processId;
}

int delete_remote_actor_process(fpc_ProcID processId) {
    const auto known = sRemoteProcesses.find(processId);
    base_process_class* process = known != sRemoteProcesses.end()
                                      ? known->second
                                      : reinterpret_cast<base_process_class*>(
                                            fopAcM_SearchByID(processId));
    if (process != nullptr) {
        return fopAcM_delete(reinterpret_cast<fopAc_ac_c*>(process));
    }

    // fopAcM_delete(id) only searches live actor queues. Area transitions can
    // remove a peer during Remote Link's asynchronous create, in which case
    // the stock helper reports success but leaves the request to spawn an
    // orphan in the next scene. Cancel the exact recorded request instead.
    auto* request = static_cast<create_request*>(
        fpcCtIt_Judge(&find_remote_create_request, &processId));
    if (request != nullptr) {
        process = request->process;
        if (fpcCtRq_Cancel(request) == FALSE) return 0;
        if (process != nullptr) return 1;
    }

    sPendingRemoteCreates.erase(processId);
    sRemoteProcessIds.erase(processId);
    sRemoteProcesses.erase(processId);
    return 1;
}

void destroy_remote_actor_processes_for_unload() {
    // Successful forced deletion runs the fpcBs_Delete post-hook and removes
    // IDs from the live set, so iterate over a stable snapshot.
    const std::vector<fpc_ProcID> processIds(sRemoteProcessIds.begin(),
                                             sRemoteProcessIds.end());
    for (const fpc_ProcID processId : processIds) {
        const auto known = sRemoteProcesses.find(processId);
        base_process_class* process = known != sRemoteProcesses.end()
                                          ? known->second
                                          : reinterpret_cast<base_process_class*>(
                                                fopAcM_SearchByID(processId));
        if (process != nullptr) {
            if (!finish_remote_process_delete(process)) {
                DuskLog.warn("RemoteLink: synchronous unload delete failed for process {}",
                             processId);
            }
            continue;
        }

        auto* request = static_cast<create_request*>(
            fpcCtIt_Judge(&find_remote_create_request,
                          const_cast<fpc_ProcID*>(&processId)));
        if (request == nullptr) continue;

        process = request->process;
        const bool cancelled = fpcCtRq_Cancel(request) != FALSE;
        if (!cancelled || !finish_remote_process_delete(process)) {
            DuskLog.warn("RemoteLink: synchronous unload create cancellation failed for process {}",
                         processId);
        }
    }
    sRemoteProcessIds.clear();
    sRemoteProcesses.clear();
    sPendingRemoteCreates.clear();
    sResolvingRemoteProfile = false;
}

ModResult install_remote_actor_profile(ModError* error) {
    // Use the official drawing hook. No C++ virtual callback is handed to the
    // engine, whose callback ABI may differ from the SDK used to build a mod.
    if (mods::hook::add_post<RemoteParticleDepthHook>(&remote_particle_depth_post) != MOD_OK) {
        uninstall_remote_actor_profile();
        return mods::set_error(error, MOD_UNAVAILABLE, "Remote particle depth hook is unavailable");
    }
    if (mods::hook::add_pre<EngineInterpBeginFrameHook>(
            &engine_interp_begin_frame_pre) != MOD_OK ||
        mods::hook::add_pre<EngineInterpBeginSimTickHook>(
            &engine_interp_begin_sim_tick_pre) != MOD_OK ||
        mods::hook::add_pre<EngineInterpPresentationHook>(
            &engine_interp_presentation_pre) != MOD_OK ||
        mods::hook::add_post<EngineInterpPresentationHook>(
            &engine_interp_presentation_post) != MOD_OK ||
        mods::hook::add_pre<EngineInterpLookupHook>(
            &engine_interp_lookup_pre) != MOD_OK ||
        mods::hook::add_pre<EngineInterpLookupConcatHook>(
            &engine_interp_lookup_concat_pre) != MOD_OK ||
        mods::hook::add_pre<RemoteProcessCreateHook>(&remote_process_create_pre) != MOD_OK ||
        mods::hook::add_post<RemoteProcessCreateHook>(&remote_process_create_post) != MOD_OK ||
        mods::hook::add_post<RemoteProcessDeleteHook>(&remote_process_delete_post) != MOD_OK ||
        mods::hook::add_pre<RemoteProfileLookupHook>(&remote_profile_lookup_pre) != MOD_OK) {
        uninstall_remote_actor_profile();
        return mods::set_error(error, MOD_UNAVAILABLE,
                               "Remote Link actor-profile lookup hook is unavailable");
    }
    return MOD_OK;
}

void register_remote_depth_particle(u32 emitterId) {
    sDepthParticles.add(emitterId, [](uint32_t id) {
        return dComIfGp_particle_getEmitter(id);
    });
}

void uninstall_remote_actor_profile() {
    mods::hook::uninstall<RemoteParticleDepthHook>();
    sDepthParticles.clear();
    mods::hook::uninstall<RemoteProfileLookupHook>();
    mods::hook::uninstall<RemoteProcessDeleteHook>();
    mods::hook::uninstall<RemoteProcessCreateHook>();
    mods::hook::uninstall<EngineInterpLookupConcatHook>();
    mods::hook::uninstall<EngineInterpLookupHook>();
    mods::hook::uninstall<EngineInterpPresentationHook>();
    mods::hook::uninstall<EngineInterpBeginSimTickHook>();
    mods::hook::uninstall<EngineInterpBeginFrameHook>();
    sPendingRemoteCreates.clear();
    sRemoteProcessIds.clear();
    sRemoteProcesses.clear();
    sResolvingRemoteProfile = false;
}

}  // namespace dusklight_online::game

namespace dusk::multiplayer {
namespace {

bool sPvpEnabled = false;
RemotePvpHitCallback sPvpHit = nullptr;
RemoteBombActorCallback sBombCreated = nullptr;
bool sDisplayMidna = false;
bool sSyncWorld = false;
bool sRemoteCollision = false;
bool sSemanticRenderingEnabled = false;
std::map<std::string, std::map<int32_t, RemoteBombObjectSnapshot>> sRemoteBombs;

}  // namespace

void configure_remote_actor_bridge(bool pvpEnabled, RemotePvpHitCallback pvpHit,
                                   RemoteBombActorCallback bombCreated) {
    sPvpEnabled = pvpEnabled;
    sPvpHit = pvpHit;
    sBombCreated = bombCreated;
}

void set_remote_pvp_hit_callback(RemotePvpHitCallback callback) {
    sPvpHit = callback;
}

void set_remote_bomb_actor_callback(RemoteBombActorCallback callback) {
    sBombCreated = callback;
}

void set_remote_actor_options(bool displayMidna, bool syncWorld,
                              bool remoteCollision, bool pvpEnabled,
                              bool semanticRenderingEnabled) {
    sDisplayMidna = displayMidna;
    sSyncWorld = syncWorld;
    sRemoteCollision = remoteCollision;
    sPvpEnabled = pvpEnabled;
    sSemanticRenderingEnabled = semanticRenderingEnabled;
    if (!sSyncWorld) sRemoteBombs.clear();
}

void set_remote_bomb_object(const RemoteBombObjectSnapshot& object) {
    if (!sSyncWorld || object.peerId.empty() || object.objectKind != REMOTE_OBJECT_BOMB) return;
    auto& peerObjects = sRemoteBombs[object.peerId];
    const auto existing = peerObjects.find(object.objectId);
    if (existing != peerObjects.end() && object.sequence <= existing->second.sequence) {
        return;
    }
    peerObjects[object.objectId] = object;
}

void erase_remote_actor_peer(const std::string& peerId) {
    sRemoteBombs.erase(peerId);
}

void reset_remote_actor_bridge() {
    sPvpEnabled = false;
    sDisplayMidna = false;
    sSyncWorld = false;
    sRemoteCollision = false;
    sSemanticRenderingEnabled = false;
    sRemoteBombs.clear();
    dusk::frame_interp::reset_callbacks();
}

bool pvp_enabled() {
    return sPvpEnabled && sPvpHit != nullptr;
}

bool display_remote_midna_enabled() {
    return sDisplayMidna;
}

bool remote_collision_enabled() {
    return sRemoteCollision;
}

bool semantic_rendering_enabled() {
    return sSemanticRenderingEnabled;
}

bool get_remote_bomb_object_for_peer(const std::string& peerId,
                                     RemoteBombObjectSnapshot* out) {
    if (!sSyncWorld || out == nullptr) return false;
    const auto peer = sRemoteBombs.find(peerId);
    if (peer == sRemoteBombs.end()) return false;

    const char* localStageName = dComIfGp_getStartStageName();
    const std::string localStage = localStageName != nullptr ? localStageName : "";
    bool found = false;
    RemoteBombObjectSnapshot best;
    for (auto it = peer->second.begin(); it != peer->second.end();) {
        RemoteBombObjectSnapshot& object = it->second;
        if (++object.ageTicks > 90 || (!object.active && object.ageTicks > 10)) {
            it = peer->second.erase(it);
            continue;
        }
        ++it;
        if (!object.valid || object.objectKind != REMOTE_OBJECT_BOMB) continue;
        if (!object.stage.empty() && !localStage.empty() && object.stage != localStage) {
            continue;
        }
        if (object.room >= 0 && !dComIfGp_roomControl_checkRoomDisp(object.room)) {
            continue;
        }
        if (!found || object.sequence > best.sequence) {
            best = object;
            found = true;
        }
    }
    if (!found) return false;
    *out = best;
    return true;
}

void report_remote_link_pvp_target_hit(fopAc_ac_c* remoteLinkActor,
                                       fopAc_ac_c* attackActor,
                                       dCcD_GObjInf* attackInfo) {
    if (sPvpHit != nullptr) sPvpHit(remoteLinkActor, attackActor, attackInfo);
}

void register_remote_bomb_actor_id(int32_t actorId) {
    if (sBombCreated != nullptr) sBombCreated(actorId);
}

}  // namespace dusk::multiplayer

namespace dusk::frame_interp {

namespace {

struct CallbackEntry {
    InterpolationCallBack callback = nullptr;
    void* userWork = nullptr;
    bool active = true;
};

// A deque keeps callback-entry addresses stable while more Remote Links
// register during the same simulation tick. The host retains these pointers
// until its next begin_sim_tick clears its own callback queue.
std::deque<CallbackEntry> sCallbacks;
std::map<const void*, std::array<float, 12>> sReplacements;
float sEngineStep = 0.0f;
bool sEngineEnabled = false;
bool sEngineSimFrame = true;
bool sRunningPresentationCallbacks = false;
bool sAcceptingOverrides = false;

template <typename Function, typename Symbol>
Function resolved_engine_function() {
    return reinterpret_cast<Function>(Symbol::resolved_target());
}

bool using_engine_callback_dispatch() {
    return dusklight_online::game::EngineInterpAddCallbackSymbol::resolved_target() != nullptr;
}

bool engine_presentation_sync_active() {
    using Function = bool (*)();
    const Function function =
        resolved_engine_function<
            Function, dusklight_online::game::EngineInterpPresentationSyncSymbol>();
    return function != nullptr && function();
}

void dispatch_interpolation_callback(void* rawEntry) {
    // Upstream dispatches only during presentation. Keep the mod-local ABI.
    constexpr bool isSimFrame = false;
    auto* entry = static_cast<CallbackEntry*>(rawEntry);
    if (entry == nullptr || !entry->active || entry->callback == nullptr ||
        entry->userWork == nullptr) {
        return;
    }

    const bool previousAcceptingOverrides = sAcceptingOverrides;
    sAcceptingOverrides = !isSimFrame && !engine_presentation_sync_active();
    entry->callback(isSimFrame, entry->userWork);
    sAcceptingOverrides = previousAcceptingOverrides;
}

}  // namespace

bool is_enabled() {
    using Function = bool (*)();
    const Function function =
        resolved_engine_function<
            Function, dusklight_online::game::EngineInterpIsEnabledSymbol>();
    if (function != nullptr) return function();
    return sEngineEnabled;
}

bool is_sim_frame() {
    using Function = bool (*)();
    const Function function =
        resolved_engine_function<
            Function, dusklight_online::game::EngineInterpIsSimFrameSymbol>();
    if (function != nullptr) return function();
    return sEngineSimFrame && !sRunningPresentationCallbacks;
}

float get_interpolation_step() {
    using Function = float (*)();
    const Function function =
        resolved_engine_function<
            Function, dusklight_online::game::EngineInterpStepSymbol>();
    if (function != nullptr) return function();
    return sEngineStep;
}

bool lookup_local_replacement(const void* key, Mtx out) {
    const auto found = sReplacements.find(key);
    if (found == sReplacements.end()) return false;
    std::memcpy(out, found->second.data(), sizeof(float) * found->second.size());
    return true;
}

bool lookup_local_concat_replacement(const void* lhs, const void* rhs, Mtx out) {
    if (lhs == nullptr || rhs == nullptr || out == nullptr ||
        engine_presentation_sync_active()) {
        return false;
    }

    Mtx lhsScratch;
    Mtx rhsScratch;
    const auto* lhsMatrix = reinterpret_cast<const Mtx*>(lhs);
    const auto* rhsMatrix = reinterpret_cast<const Mtx*>(rhs);

    const auto lhsLocal = sReplacements.find(lhs);
    const auto rhsLocal = sReplacements.find(rhs);
    const bool hasLocalLhs = lhsLocal != sReplacements.end();
    const bool hasLocalRhs = rhsLocal != sReplacements.end();
    if (!hasLocalLhs && !hasLocalRhs) return false;

    if (hasLocalLhs) {
        std::memcpy(lhsScratch, lhsLocal->second.data(), sizeof(lhsScratch));
        lhsMatrix = &lhsScratch;
    }
    if (hasLocalRhs) {
        std::memcpy(rhsScratch, rhsLocal->second.data(), sizeof(rhsScratch));
        rhsMatrix = &rhsScratch;
    }

    // Resolve both operands before concatenating. When only one operand has a
    // local override, retain the host replacement for the other operand.
    const auto hostLookup = dusklight_online::game::EngineInterpLookupHook::g_orig;
    if (!hasLocalLhs && hostLookup != nullptr && hostLookup(lhs, lhsScratch)) {
        lhsMatrix = &lhsScratch;
    }
    if (!hasLocalRhs && hostLookup != nullptr && hostLookup(rhs, rhsScratch)) {
        rhsMatrix = &rhsScratch;
    }

    MTXConcat(*lhsMatrix, *rhsMatrix, out);
    return true;
}

bool lookup_replacement(const void* key, Mtx out) {
    if (key == nullptr || out == nullptr) return false;
    if (lookup_local_replacement(key, out)) return true;

    // Layer the mod-owned override map over the host map. Calling g_orig here
    // reaches the host lookup without recursively entering the pre-hook.
    const auto hostLookup = dusklight_online::game::EngineInterpLookupHook::g_orig;
    return hostLookup != nullptr && hostLookup(key, out);
}

bool lookup_concat_replacement(const void* lhs, const void* rhs, Mtx out) {
    if (lookup_local_concat_replacement(lhs, rhs, out)) return true;

    const auto hostLookup = dusklight_online::game::EngineInterpLookupConcatHook::g_orig;
    return hostLookup != nullptr && hostLookup(lhs, rhs, out);
}

void add_interpolation_callback(InterpolationCallBack callback, void* userWork) {
    if (!is_enabled() || !is_sim_frame() || callback == nullptr || userWork == nullptr) {
        return;
    }

    sCallbacks.push_back({callback, userWork, true});
    CallbackEntry* const rawEntry = &sCallbacks.back();

    using Function = void (*)(void (*)(void*), void*);
    const Function hostAdd =
        resolved_engine_function<
            Function, dusklight_online::game::EngineInterpAddCallbackSymbol>();
    if (hostAdd != nullptr) {
        // Register with Dusklight's per-simulation callback list so callback
        // timing, camera validity, and presentation nesting remain engine-owned.
        hostAdd(&dispatch_interpolation_callback, rawEntry);
    }
}

void remove_interpolation_callbacks_for(void* userWork) {
    for (CallbackEntry& entry : sCallbacks) {
        if (entry.userWork == userWork) {
            entry.active = false;
            entry.userWork = nullptr;
        }
    }
}

void prepare_presentation_callbacks() {
    sReplacements.clear();
}

void run_presentation_callbacks() {
    // When the host callback API is present, begin_presentation has
    // already dispatched these callbacks. The post hook is a fallback for a
    // host that exposes the timing hooks but not the callback API.
    if (using_engine_callback_dispatch() || !sEngineEnabled || sEngineSimFrame ||
        sCallbacks.empty()) {
        return;
    }
    sRunningPresentationCallbacks = true;
    for (CallbackEntry& entry : sCallbacks) {
        dispatch_interpolation_callback(&entry);
    }
    sRunningPresentationCallbacks = false;
}

void observe_engine_frame(bool enabled, bool isSimFrame, float step) {
    sEngineEnabled = enabled;
    sEngineSimFrame = isSimFrame;
    sEngineStep = std::clamp(step, 0.0f, 1.0f);
    if (!enabled) {
        sCallbacks.clear();
        sReplacements.clear();
    }
}

void begin_engine_sim_tick() {
    sEngineSimFrame = true;
    sRunningPresentationCallbacks = false;
    sAcceptingOverrides = false;
    sCallbacks.clear();
    sReplacements.clear();
}

void reset_callbacks() {
    sRunningPresentationCallbacks = false;
    sAcceptingOverrides = false;
    sCallbacks.clear();
    sReplacements.clear();
    sEngineStep = 0.0f;
    sEngineEnabled = false;
    sEngineSimFrame = true;
}

void override_replacement(const void* key, Mtx matrix) {
    if (!sAcceptingOverrides || key == nullptr || matrix == nullptr) return;
    auto& replacement = sReplacements[key];
    std::memcpy(replacement.data(), matrix, sizeof(float) * replacement.size());
}

}  // namespace dusk::frame_interp
