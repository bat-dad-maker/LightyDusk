#pragma once

#include "dusklight_online/game/remote_visibility.hpp"
#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "f_op/f_op_overlap_mng.h"

namespace dusklight_online::game {

// TP stores these two Ganondorf clash motions in boss archives rather than
// Link's global animation archive. Other event motions remain unsupported.
inline bool ganon_clash_animation_archive(int procId, bool isWolf, int arcNo) {
    return (isWolf && procId == daAlink_c::PROC_WOLF_GANON_CATCH && arcNo == 8) ||
           (!isWolf && procId == daAlink_c::PROC_SWORD_PUSH && arcNo == 7);
}

inline bool local_link_in_load_exit(daAlink_c* link) {
    if (link == nullptr || !dComIfGp_isEnableNextStage()) return false;
    // checkSceneChange sets this flag and ORIGINAL 26/17 only after a
    // successful ordinary exit. A warp or cinematic stage request is not an
    // exit-walk exception. Doors retain their own event command instead.
    const auto mode = link->mDemo.getDemoMode();
    return link->eventInfo.checkCommandDoor() ||
           link->mProcID == daAlink_c::PROC_DOOR_OPEN ||
           (link->mDemo.getDemoType() == daPy_demo_c::DEMO_TYPE_ORIGINAL_e &&
            link->checkNoResetFlg0(daPy_py_c::FLG0_UNK_4000) &&
            (mode == daPy_demo_c::DEMO_UNK_26_e ||
             mode == daPy_demo_c::DEMO_UNK_17_e));
}

inline bool local_transition_hides_remote_link() {
    auto* link = daAlink_getAlinkActorClass();
    if (link == nullptr) return true;
    // IsDoingReq spans both visible fades and the request's final cleanup;
    // it is not a teardown signal. IsPeek marks the actual swap barrier.
    return remote_link_transition_hidden(dComIfGp_isEnableNextStage(),
                                         fopOvlpM_IsPeek(),
                                         local_link_in_load_exit(link));
}

inline bool local_scene_hides_remote_link() {
    // Remote actors carry NOPAUSE and can keep rendering and consuming live
    // poses while the viewer's own Link is in an event or transforming. The
    // overlap swap is still unsafe because this scene's actors are being torn
    // down. Interaction has a separate event-time guard.
    return local_transition_hides_remote_link();
}

} // namespace dusklight_online::game
