#pragma once

#include "SSystem/SComponent/c_xyz.h"
#include "SSystem/SComponent/c_sxyz.h"
#include "f_pc/f_pc_manager.h"

#include <mods/api.h>

namespace dusklight_online::game {

ModResult install_remote_actor_profile(ModError* error);
void destroy_remote_actor_processes_for_unload();
void uninstall_remote_actor_profile();
// Track engine handles, not callback objects or reusable emitter addresses.
void register_remote_depth_particle(u32 emitterId);

// Remote Link uses a private process ID. Profile substitution is scoped to
// creation requests recorded by this bridge rather than indexing mainline's
// fixed profile table.
fpc_ProcID create_remote_actor_process(u32 actorParams, const cXyz* position, int room,
                                       const csXyz* angle, const cXyz* scale, s8 argument);
int delete_remote_actor_process(fpc_ProcID processId);

}  // namespace dusklight_online::game
