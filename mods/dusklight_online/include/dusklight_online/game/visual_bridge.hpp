#pragma once

#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace dusklight_online::game {

void set_host_mirror_mode(bool mirrored);

struct ProgressionPromptView {
    bool active = false;
    bool waiting = false;
    std::string title;
    std::string body;
    float ageSeconds = 0.0f;
    float remainingRatio = 0.0f;
    float holdRatio = 0.0f;
};

struct SpeedrunPromptView {
    bool active = false;
    bool countdown = false;
    bool localReady = false;
    uint32_t readyCount = 0;
    uint32_t playerCount = 0;
    float holdRatio = 0.0f;
    float countdownSeconds = 0.0f;
};

struct PlayerLocationView {
    std::string stage;
    int room = -1;
    struct FieldMapMarker {
        std::string stage;
        int region = -1;  // Field-map region numbers are one-based.
        float x = 0.0f;
        float z = 0.0f;
        int angleY = 0;
    };
    std::optional<FieldMapMarker> fieldMapMarker;
};

ModResult install_visual_hooks(ModError* error);
void uninstall_visual_hooks();
void update_visual_overlays(
    bool connected, bool chatAvailable, bool gameplayReady, bool nameLabelsEnabled, bool remoteModelEnabled,
    bool playerListEnabled, std::string_view room, std::string_view localStatus,
    std::string_view localName,
    const std::map<std::string, dusk::multiplayer::PeerPoseSnapshot>& poses,
    const std::map<std::string, std::string>& names,
    const std::map<std::string, PlayerLocationView>& locations,
    const std::map<std::string, uint32_t>& latencies,
    const ProgressionPromptView& progressionPrompt);
void push_online_notification(std::string text, float durationSeconds = 5.0f,
                              bool warning = false);
void set_speedrun_prompt(const SpeedrunPromptView& prompt);
void push_online_player_notification(std::string playerName, std::string itemName,
                                     std::string checkName, uint32_t color,
                                     float durationSeconds = 5.0f);
void push_chat_message(std::string playerName, std::string text, uint32_t color);
[[nodiscard]] std::optional<std::string> take_chat_submission();
void configure_voice_mute_hotkey(int scancode, bool enabled);
void begin_voice_mute_hotkey_capture();
void cancel_voice_mute_hotkey_capture();
[[nodiscard]] bool voice_mute_hotkey_capture_active();
[[nodiscard]] std::optional<int> take_voice_mute_hotkey_binding();
[[nodiscard]] bool take_voice_mute_hotkey_toggle();
[[nodiscard]] std::string voice_mute_hotkey_name(int scancode);
void set_voice_mute_indicator(bool visible);
void reset_visual_overlays();

}  // namespace dusklight_online::game
