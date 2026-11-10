#pragma once

#include <mods/api.h>

namespace dusklight_online::net {
class Transport;
}

namespace dusklight_online::game {

ModResult install_bomb_hooks(net::Transport& transport, ModError* error);
void uninstall_bomb_hooks();
void set_bomb_sync_enabled(bool enabled);
void reset_bomb_sync_state();

}  // namespace dusklight_online::game
