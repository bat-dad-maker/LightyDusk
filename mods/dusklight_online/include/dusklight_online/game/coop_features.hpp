#pragma once

// Dusklight Co-op gameplay features (game-side glue).
//
//  * Shared enemy HP: when you damage an enemy, the same enemy in your
//    partner's game loses the same HP, so both of you wear it down together.
//  * Two-player bosses: boss damage is divided by the number of players in
//    the stage, so a two-player fight has double the boss's health.
//  * Boss doors together: while in a room, a boss door only opens when a
//    partner is standing at it as well.
//
// The rules themselves are in coop_rules.hpp and unit tested.

#include "dusklight_online/game/coop_rules.hpp"
#include "dusklight_online/game/protocol_router.hpp"

#include <mods/api.h>

#include <string>

namespace dusklight_online::net {
class Transport;
}

namespace dusklight_online::game::coop {

ModResult install_hooks();
void uninstall_hooks();

void attach(net::Transport* transport);
void set_local_options(const Options& options);
[[nodiscard]] Options current_options();
[[nodiscard]] bool host_controls_options();

// Called from the existing fpcM_Execute pre/post hooks.
void process_execute_pre(void* process);
void process_execute_post(void* process);

// Called once per game update.
void tick();

[[nodiscard]] bool handles(const std::string& type);
ApplyResult consume(const RoutedMessage& message);
void peer_left(const std::string& peerId);
void reset();

}  // namespace dusklight_online::game::coop
