#pragma once

#include <cstdint>

namespace dusklight_online::game {

// Apply the visual and actor side effects of a permanent collectible bit.
void repair_remote_tbox_collectible(int stage, int flag, bool newlySet, bool animate = true);
void update_remote_tbox_animations(bool active);
void repair_remote_memory_item_collectible(int stage, int flag);

// Apply actor-side state latched before a remotely received switch changed.
// This covers moving/set-piece actors which do not fully react to the save bit
// alone while their room is already loaded.
bool repair_remote_switch_actors(int stage, int flag);

// Read a loaded web's native destruction timer, or -1 for another actor.
int web_delete_timer(void* actor);

// Start the exact loaded web's native destruction sequence. The actor owns
// the animation, completion switch, collision release, and deletion.
bool apply_remote_web_timer(int actorName, int room, uint32_t params, int timer);

// Apply a web's authoritative completion switch and remove the exact actor.
bool repair_remote_web_actor(int actorName, int room, int flag, uint32_t params);

enum class RoomActorAction : uint8_t {
    Break = 1,
    PartialBreak = 2,
    DamageStage = 3,
    DigStart = 4,
    MoveStep = 5,
    RotateTo = 6,
    Slide = 7,
};

// State sampled around fpcM_Execute for multi-stage actors whose visible
// action begins before their completion switch changes.
int room_actor_action_state(void* actor);

// Read the compact argument for a newly started native room action. Movebox
// actions encode direction, push/pull, and duration; the Lakebed staircase
// encodes its target orientation.
int room_actor_action_argument(void* actor);

// Classify the action represented by an actor's native switch write. IceWall
// needs its live partial-break edge distinguished from full destruction.
RoomActorAction room_actor_switch_action(void* actor, bool wasSet);

// True only when this exact room actor owns the native durable stage switch.
// Live actions remain room-scoped, while these bits can use ordinary save-state
// synchronization before the destination room is loaded.
bool room_actor_persistent_switch(void* actor, int flag);

// Drive the exact loaded actor through a reviewed native, non-event action.
bool apply_remote_room_actor_action(int actorName, int room, uint32_t params,
                                    RoomActorAction action, int actionArgument = 0);

// A remotely initiated movebox still executes the actor's native walk mode,
// whose final frame touches Link's push/pull keep flag. This identifies only
// those remote walks so the caller can preserve the local player's prior flag.
bool remote_movebox_action_active(void* actor);

// Snowpeak ice blocks use a native camera event for local pushes. Remote
// slides temporarily park that actor event and restore it after movement.
bool remote_iceblock_action_active(void* actor);
void finish_remote_iceblock_action(void* actor);

// Apply the authoritative completion bit and repair an exact loaded actor if
// its action message was missed or the action began through another path.
bool apply_remote_room_actor_switch(int actorName, int room, int flag, uint32_t params,
                                    RoomActorAction fallbackAction);

// Actors can spawn after a snapshot or live bit is applied, so repeat the
// repair while playing.
void repair_current_stage_collectibles();

// Repair live small-key locks whose keyhole state is latched during creation.
void repair_remote_key_door_actor(void* process);

}  // namespace dusklight_online::game
