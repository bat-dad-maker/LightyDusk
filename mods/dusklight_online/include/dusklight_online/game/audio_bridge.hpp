#pragma once

#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>

#include <vector>

namespace dusklight_online::game {

ModResult install_audio_hooks(ModError* error);
void uninstall_audio_hooks();
void set_randomizer_audio_filter(bool active);
std::vector<dusk::multiplayer::RemoteAudioEvent> drain_local_audio_events();
std::vector<dusk::multiplayer::RemoteAudioEvent> drain_local_active_audio_events();
void clear_local_audio_events();

}  // namespace dusklight_online::game
