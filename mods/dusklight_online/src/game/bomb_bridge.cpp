#include "dusklight_online/game/bomb_bridge.hpp"

#include "d/dolzel.h"

#include "dusklight_online/net/transport.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#include "d/actor/d_a_nbomb.h"
#include "f_op/f_op_actor_mng.h"
#include "mods/service.hpp"
#include "mods/svc/hook.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace dusklight_online::game {

DEFINE_HOOK(&daNbomb_c::execute, BombExecuteHook);

namespace {

net::Transport* sTransport = nullptr;
bool sSyncEnabled = false;
uint32_t sSequence = 0;
std::set<fpc_ProcID> sRemoteOwnedActorIds;
std::set<fpc_ProcID> sTerminalSentActorIds;

int detect_bomb_kind(const daNbomb_c* bomb) {
    switch (fopAcM_GetParam(bomb)) {
    case dBomb_c::PRM_WATER_BOMB_PLAYER:
        return 3;
    case dBomb_c::PRM_INSECT_BOMB_PLAYER:
        return 4;
    case dBomb_c::PRM_NORMAL_BOMB_PLAYER:
    default:
        return 2;
    }
}

void remember_remote_bomb_actor(int32_t actorId) {
    if (actorId == static_cast<int32_t>(fpcM_ERROR_PROCESS_ID_e)) return;
    sRemoteOwnedActorIds.insert(static_cast<fpc_ProcID>(actorId));
}

void bomb_execute_post(ModContext*, void* args, void*, void*) {
    if (!sSyncEnabled || sTransport == nullptr || !sTransport->status().welcomed) {
        return;
    }

    auto* bomb = mods::arg<daNbomb_c*>(args, 0);
    if (bomb == nullptr) return;
    const fpc_ProcID actorId = fopAcM_GetID(bomb);
    if (sRemoteOwnedActorIds.contains(actorId) || sTerminalSentActorIds.contains(actorId)) {
        return;
    }

    net::udp::RemoteObjectPacket packet{};
    if (const char* stage = dComIfGp_getStartStageName(); stage != nullptr) {
        std::strncpy(packet.stageName, stage, sizeof(packet.stageName) - 1);
    }
    packet.sequence = ++sSequence;
    packet.objectId = static_cast<int32_t>(actorId);
    packet.x = bomb->current.pos.x;
    packet.y = bomb->current.pos.y;
    packet.z = bomb->current.pos.z;
    packet.angleY = bomb->shape_angle.y;
    packet.exTime = static_cast<int16_t>(std::clamp<int>(bomb->getExTime(), -32768, 32767));
    packet.room = static_cast<int8_t>(fopAcM_GetRoomNo(bomb));
    packet.objectKind = dusk::multiplayer::REMOTE_OBJECT_BOMB;
    packet.kind = static_cast<uint8_t>(detect_bomb_kind(bomb));
    packet.flags = net::udp::ObjectActive;
    if (packet.exTime <= 0) {
        packet.flags = net::udp::ObjectExploding;
        sTerminalSentActorIds.insert(actorId);
    }
    (void)sTransport->send_remote_object(packet);
}

}  // namespace

ModResult install_bomb_hooks(net::Transport& transport, ModError* error) {
    sTransport = &transport;
    dusk::multiplayer::set_remote_bomb_actor_callback(&remember_remote_bomb_actor);
    const ModResult result = mods::hook::add_post<BombExecuteHook>(&bomb_execute_post);
    if (result != MOD_OK) {
        sTransport = nullptr;
        dusk::multiplayer::set_remote_bomb_actor_callback(nullptr);
        return mods::set_error(error, result, "Bomb execute hook is unavailable");
    }
    return MOD_OK;
}

void uninstall_bomb_hooks() {
    sSyncEnabled = false;
    mods::hook::uninstall<BombExecuteHook>();
    dusk::multiplayer::set_remote_bomb_actor_callback(nullptr);
    sTransport = nullptr;
    reset_bomb_sync_state();
}

void set_bomb_sync_enabled(bool enabled) {
    sSyncEnabled = enabled;
}

void reset_bomb_sync_state() {
    sSequence = 0;
    sRemoteOwnedActorIds.clear();
    sTerminalSentActorIds.clear();
    if (sTransport != nullptr) {
        dusk::multiplayer::set_remote_bomb_actor_callback(&remember_remote_bomb_actor);
    }
}

}  // namespace dusklight_online::game
