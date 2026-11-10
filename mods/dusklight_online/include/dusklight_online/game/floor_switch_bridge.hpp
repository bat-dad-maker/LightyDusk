#pragma once

#include <mods/api.h>
#include <cstdint>
#include <string_view>
#include <nlohmann/json_fwd.hpp>

class dSv_info_c;

namespace dusklight_online::game {
class GameAdapter;
enum class ApplyResult : uint8_t;

ModResult install_floor_switch_hooks(GameAdapter& adapter, ModError* error);
void uninstall_floor_switch_hooks();
// Reuse the mod's already-working fpcM_Execute hook (the same observation
// boundary used by webs, ice blocks, Lakebed stairs, etc). Per-actor REL
// Execute hooks are intentionally avoided.
void floor_switch_process_pre(void* process);
void floor_switch_process_post(void* process);
// Run after actor simulation, before inbound packets. Publishes sender-local
// contacts on changes, plus short heartbeats for joining peers / lease safety.
void flush_floor_switch_state();
ApplyResult receive_floor_switch_state(std::string_view peer, const nlohmann::json& message);
void forget_floor_switch_peer(std::string_view peer);
void reset_floor_switch_state(bool newSession = false);

bool is_floor_switch_actor(int actor);
// Prevent completion writes caused solely by received pressure from echoing.
bool is_remote_floor_switch_execution(const void* actor);
bool is_floor_switch_momentary_output(int actor, uint32_t params, int flag);
bool loaded_floor_switch_owns_flag(int stage, int flag);
// Strip loaded momentary outputs from an outgoing manual save COPY only.
void sanitize_floor_switch_snapshot(dSv_info_c& copy);
// Existing durable switch sync remains the completion fallback for latching
// plates. Repair their native animation if a short contact burst was missed.
void repair_floor_switch_completion(int stage, int flag);
} // namespace dusklight_online::game
