#include "dusklight_online/game/appearance.hpp"
#include "dusklight_online/game/remote_actor_bridge.hpp"
#include "f_op/f_op_actor_mng.h"

// Keep process-name comparisons consistent with the private ID used by the
// standalone profile bridge.
#define fopAcM_create(profile, actorParams, position, room, angle, scale, argument) \
    ::dusklight_online::game::create_remote_actor_process(                       \
        actorParams, position, room, angle, scale, argument)
#define fopAcM_delete(processId) \
    ::dusklight_online::game::delete_remote_actor_process(processId)
#include "remote_link_dummy_impl.inc"
#undef fopAcM_delete
#undef fopAcM_create

namespace dusklight_online::game::appearance {
std::string peer_for_actor(const void* actor) {
    if (!actor) return {};
    const auto id = fopAcM_GetID(const_cast<fopAc_ac_c*>(static_cast<const fopAc_ac_c*>(actor)));
    for (const auto& [peer, dummy] : dusk::multiplayer::sActorDummies) {
        if (dummy.actorId == id || dummy.preparedTransformActorId == id) return peer;
    }
    return {};
}
}
