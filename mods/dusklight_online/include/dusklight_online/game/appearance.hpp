#pragma once
#include "dusklight_online/game/player_color.hpp"
#include <mods/api.h>
class J3DModel;
namespace dusklight_online::game::appearance {
// Outside a lobby, leave texture selection to the game and cosmetic mods.
void set_lobby_active(bool active);
void set_local(Color color, Color outfit, Color zora);
Color local_outfit_color();
Color local_zora_color();
Color local_color();
void set_peer(std::string_view peer, Color color, Color outfit, Color zora);
Color peer_outfit_color(std::string_view peer);
Color peer_zora_color(std::string_view peer);
Color peer_color(std::string_view peer);
void forget_peer(std::string_view peer);
void reset_peers();
// Implemented alongside the mod-owned remote actor registry.
std::string peer_for_actor(const void* actor);
void apply(const void* owner, Color outfit, Color zora, J3DModel* body, J3DModel* head,
           J3DModel* bridge = nullptr);
void release(const void* owner);
ModResult initialize(ModError* error);
void shutdown();
}
