#pragma once

#include "dusklight_online/net/transport.hpp"
#include "dusklight_online/game/speedrun_bridge.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <mods/api.h>
#include <mods/svc/config.h>
#include <mods/svc/ui.h>

namespace dusklight_online {

namespace game {
class GameAdapter;
class ProtocolRouter;
class VoiceChat;
}

class OnlineApp {
public:
    OnlineApp();
    ~OnlineApp();

    OnlineApp(const OnlineApp&) = delete;
    OnlineApp& operator=(const OnlineApp&) = delete;

    ModResult initialize(ModError* error);
    void consume_progression_prompt_input();
    void update();
    void shutdown();

private:
    struct Config {
        ConfigVarHandle playerName = 0;
        ConfigVarHandle playerColor = 0;
        ConfigVarHandle outfitColor = 0;
        ConfigVarHandle zoraColor = 0;
        ConfigVarHandle matchOutfitColor = 0;
        ConfigVarHandle directRoom = 0;
        ConfigVarHandle bindHost = 0;
        ConfigVarHandle publicHost = 0;
        ConfigVarHandle port = 0;
        ConfigVarHandle directInvite = 0;
        ConfigVarHandle relayCode = 0;
        ConfigVarHandle relayRoom = 0;
        ConfigVarHandle lobbyCode = 0;
        ConfigVarHandle relayManualHost = 0;
        ConfigVarHandle relayLocal = 0;
        ConfigVarHandle dummyModel = 0;
        ConfigVarHandle nameLabels = 0;
        ConfigVarHandle syncFlags = 0;
        ConfigVarHandle syncPrompts = 0;
        ConfigVarHandle saveRecovery = 0;
        ConfigVarHandle displayMidna = 0;
        ConfigVarHandle remoteCollision = 0;
        ConfigVarHandle pvp = 0;
        ConfigVarHandle sharedHp = 0;
        ConfigVarHandle bossDoorTogether = 0;
        ConfigVarHandle playerList = 0;
        ConfigVarHandle hideActiveMods = 0;
        ConfigVarHandle voiceEnabled = 0;
        ConfigVarHandle voiceProximity = 0;
        ConfigVarHandle voiceProximityRange = 0;
        ConfigVarHandle voiceMicMuted = 0;
        ConfigVarHandle voiceMuteHotkey = 0;
        ConfigVarHandle voiceInput = 0;
        ConfigVarHandle micVolume = 0;
        ConfigVarHandle playerVolume = 0;
    } config_;

    net::Transport transport_;
    std::unique_ptr<game::GameAdapter> game_;
    std::unique_ptr<game::ProtocolRouter> router_;
    std::unique_ptr<game::VoiceChat> voice_;
    std::filesystem::path hostConfigPath_;
    std::optional<std::filesystem::file_time_type> lastHostConfigWrite_;
    std::chrono::steady_clock::time_point lastHostConfigPoll_{};
    float masterVolumeGain_ = 1.0f;
    std::string lastVoiceError_;
    UiWindowHandle window_ = 0;
    UiWindowHandle settingsWindow_ = 0;
    UiWindowHandle playerOptionsWindow_ = 0;
    UiWindowHandle speedrunWindow_ = 0;
    UiWindowHandle voiceWindow_ = 0;
    UiElementHandle voiceMuteHotkeyText_ = 0;
    std::string renderedVoiceMuteHotkeyText_;
    UiWindowHandle syncWindow_ = 0;
    UiWindowHandle lobbyWindow_ = 0;
    UiMenuTabHandle menuTab_ = 0;
    UiStyleHandle overlayStyle_ = 0;
    UiStyleHandle hideActiveModsStyle_ = 0;
    bool hideActiveModsStyleUnavailable_ = false;
    UiElementHandle panelStatus_ = 0;
    UiElementHandle windowStatus_ = 0;
    UiElementHandle windowPlayersEmpty_ = 0;
    bool windowPlayersEmptyVisible_ = true;
    std::array<UiElementHandle, 3> manualHostControls_{};
    enum class ConnectionRole : uint8_t {
        Host,
        Join,
    } connectionRole_ = ConnectionRole::Host;
    std::string panelRenderedStatus_;
    std::string windowRenderedStatus_;
    std::string directCodeDisplay_;
    std::string relayCodeDisplay_;
    bool reopenWindowPending_ = false;
    bool reopenSyncWindowPending_ = false;
    std::string statusMessage_ = "Not connected";
    std::string requestedDisconnectStatus_;
    std::string activeCode_;
    bool livePublishInitialized_ = false;
    bool relayHostIntent_ = false;
    bool lastWantPuppet_ = true;
    bool lastWantMidna_ = false;
    bool manualSyncWasWaiting_ = false;
    uint32_t manualSyncCooldownTicks_ = 0;
    UiElementHandle manualSyncFlagsButton_ = 0;
    UiElementHandle manualSyncWarpButton_ = 0;
    bool relayOwnerStateKnown_ = false;
    bool wasRelayOwner_ = false;
    std::string pendingLobbyFailurePrefix_;
    bool pendingLobbyFailureNotified_ = false;
    std::string connectedLobbyName_;
    game::speedrun::SaveProbe speedrunProbe_;
    std::set<std::string> speedrunWaitingPeers_;
    std::set<std::string> speedrunParticipants_;
    std::set<std::string> speedrunReadyPeers_;
    enum class SpeedrunPhase : uint8_t { None, CheckingSaves, WaitingReady, Countdown };
    SpeedrunPhase speedrunPhase_ = SpeedrunPhase::None;
    uint64_t speedrunSequence_ = 0;
    uint64_t speedrunRequestId_ = 0;
    uint64_t speedrunPeerRequestId_ = 0;
    std::chrono::steady_clock::time_point speedrunDeadline_{};
    std::chrono::steady_clock::time_point speedrunHoldStart_{};
    std::chrono::steady_clock::time_point speedrunCountdownAt_{};
    std::chrono::steady_clock::time_point speedrunResetAt_{};
    bool speedrunLocalReady_ = false;
    bool speedrunPeerReady_ = false;
    bool speedrunLocalPlayerReady_ = false;
    bool speedrunDownHeld_ = false;
    uint32_t speedrunReadyCount_ = 0;
    uint32_t speedrunReadyTotal_ = 0;
    bool speedrunChecksSent_ = false;
    bool speedrunAvailable_ = false;
    std::vector<std::string> manualPeerIds_;
    std::vector<std::string> manualPeerLabels_;
    std::vector<std::string> voiceInputLabels_;
    std::vector<const char*> voiceInputOptions_;
    std::vector<UiElementHandle> manualPeerButtonElements_;
    struct ManualPeerButtonContext {
        OnlineApp* app = nullptr;
        int64_t index = 0;
    };
    std::vector<ManualPeerButtonContext> manualPeerButtonContexts_;
    int64_t selectedManualPeer_ = 0;
    struct InlineKickRow {
        OnlineApp* app = nullptr;
        UiElementHandle row = 0;
        UiElementHandle identity = 0;
        UiElementHandle button = 0;
        std::string peerId;
        std::string peerName;
        std::string renderedIdentity;
        bool rowVisible = true;
        bool buttonVisible = true;
        bool kickPending = false;
    };
    std::array<InlineKickRow, 7> inlineKickRows_{};

    ModResult register_config(ModError* error);
    void refresh_host_settings();
    ModResult register_ui(ModError* error);
    std::string string_value(ConfigVarHandle handle) const;
    bool bool_value(ConfigVarHandle handle, bool fallback = false) const;
    int64_t int_value(ConfigVarHandle handle, int64_t fallback = 0) const;
    net::RoomSettings configured_settings() const;
    net::RoomSettings displayed_settings() const;
    std::string status_text() const;
    std::string dashboard_rml() const;
    void open_window();
    void open_settings_window();
    void open_player_options_window();
    void open_speedrun_window();
    void tick_speedrun();
    void update_speedrun_prompt();
    void begin_speedrun_ready();
    void begin_speedrun_countdown();
    void cancel_speedrun_start(std::string_view reason);
    void update_speedrun_mods_visibility();
    void clear_speedrun_request();
    void handle_speedrun_message(const net::Event& event);
    void commit_speedrun_start();
    void open_voice_window();
    void open_sync_window();
    void open_lobby_window(ConnectionRole role);
    void host_direct();
    void join_direct();
    void host_relay();
    void join_relay();
    void disconnect();
    void publish_live_options();
    void refresh_manual_peer_choices();
    void refresh_inline_player_rows();
    void request_manual_sync(bool flagsOnly);
    void set_manual_sync_pending_visual(bool pending);
    void begin_lobby_attempt(std::string failurePrefix);
    void notify_lobby_attempt_failure(std::string_view detail);

public:
    // C service callbacks must be addressable by the descriptor-building
    // helpers; they immediately recover the owning OnlineApp from user_data.
    static ModResult build_panel(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult update_panel(ModContext*, void*, ModError*);
    static ModResult build_session_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                       UiElementHandle, void*, ModError*);
    static ModResult build_player_options_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                              UiElementHandle, void*, ModError*);
    static ModResult build_speedrun_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                       UiElementHandle, void*, ModError*);
    static ModResult build_voice_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                    UiElementHandle, void*, ModError*);
    static void player_options_window_closed(ModContext*, UiWindowHandle, void*);
    static void speedrun_window_closed(ModContext*, UiWindowHandle, void*);
    static void voice_window_closed(ModContext*, UiWindowHandle, void*);
    static void player_options_pressed(ModContext*, void*);
    static void speedrun_pressed(ModContext*, void*);
    static void start_speedrun_pressed(ModContext*, void*);
    static void reset_speedrun_pressed(ModContext*, void*);
    static bool speedrun_unavailable(ModContext*, void*);
    static bool speedrun_action_unavailable(ModContext*, void*);
    static bool start_speedrun_unavailable(ModContext*, void*);
    static void voice_pressed(ModContext*, void*);
    static void mute_hotkey_pressed(ModContext*, void*);
    static void voice_input_get(ModContext*, void*, UiControlValue*);
    static void voice_input_set(ModContext*, void*, const UiControlValue*);
    static ModResult build_settings_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                        UiElementHandle, void*, ModError*);
    static ModResult build_sync_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                    UiElementHandle, void*, ModError*);
    static ModResult build_lobby_tab(ModContext*, UiWindowHandle, UiElementHandle,
                                     UiElementHandle, void*, ModError*);
    static ModResult update_lobby_window(ModContext*, void*, ModError*);
    static ModResult build_host_direct_settings(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult build_host_relay_settings(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult build_join_direct_settings(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult build_join_relay_settings(ModContext*, UiElementHandle, void*, ModError*);
    static ModResult update_window(ModContext*, void*, ModError*);
    static void window_closed(ModContext*, UiWindowHandle, void*);
    static void settings_window_closed(ModContext*, UiWindowHandle, void*);
    static void sync_window_closed(ModContext*, UiWindowHandle, void*);
    static void lobby_window_closed(ModContext*, UiWindowHandle, void*);
    static void reset_player_options(ModContext*, void*);
    static bool player_colour_locked(ModContext*, void*);
    static bool voice_proximity_range_locked(ModContext*, void*);
    static bool voice_proximity_setting_locked(ModContext*, void*);
    void match_player_colour();
    static void open_pressed(ModContext*, void*);
    static void settings_pressed(ModContext*, void*);
    static void sync_menu_pressed(ModContext*, void*);
    static void host_lobby_pressed(ModContext*, void*);
    static void join_lobby_pressed(ModContext*, void*);
    static void menu_selected(ModContext*, void*);
    static void disconnect_pressed(ModContext*, void*);
    static void stop_hosting_pressed(ModContext*, void*);
    static void host_direct_pressed(ModContext*, void*);
    static void join_direct_pressed(ModContext*, void*);
    static void host_relay_pressed(ModContext*, void*);
    static void join_relay_pressed(ModContext*, void*);
    static void direct_code_get(ModContext*, void*, UiControlValue*);
    static void direct_code_set(ModContext*, void*, const UiControlValue*);
    static void relay_code_get(ModContext*, void*, UiControlValue*);
    static void relay_code_set(ModContext*, void*, const UiControlValue*);
    static void copy_direct_code_pressed(ModContext*, void*);
    static void paste_direct_code_pressed(ModContext*, void*);
    static void copy_relay_code_pressed(ModContext*, void*);
    static void paste_relay_code_pressed(ModContext*, void*);
    static void manual_peer_pressed(ModContext*, void*);
    static bool manual_peer_selected(ModContext*, void*);
    static void inline_kick_pressed(ModContext*, void*);
    static void manual_sync_warp_pressed(ModContext*, void*);
    static void manual_sync_flags_pressed(ModContext*, void*);
    static void refresh_peers_pressed(ModContext*, void*);
    static void refresh_sync_peers_pressed(ModContext*, void*);
    static bool manual_sync_unavailable(ModContext*, void*);
    static bool sync_menu_unavailable(ModContext*, void*);
    static bool inline_kick_unavailable(ModContext*, void*);
    static bool session_active(ModContext*, void*);
    static bool host_inactive(ModContext*, void*);
    static bool joiner_inactive(ModContext*, void*);
    static bool direct_host_inactive(ModContext*, void*);
    static bool direct_join_inactive(ModContext*, void*);
    static bool relay_host_inactive(ModContext*, void*);
    static bool relay_join_inactive(ModContext*, void*);
    static bool room_setting_locked(ModContext*, void*);
    static bool remote_collision_setting_locked(ModContext*, void*);
    static bool pvp_setting_locked(ModContext*, void*);
    static void dummy_model_get(ModContext*, void*, UiControlValue*);
    static void dummy_model_set(ModContext*, void*, const UiControlValue*);
    static void sync_flags_get(ModContext*, void*, UiControlValue*);
    static void sync_flags_set(ModContext*, void*, const UiControlValue*);
    static void save_recovery_get(ModContext*, void*, UiControlValue*);
    static void save_recovery_set(ModContext*, void*, const UiControlValue*);
    static void remote_collision_get(ModContext*, void*, UiControlValue*);
    static void remote_collision_set(ModContext*, void*, const UiControlValue*);
    static void pvp_get(ModContext*, void*, UiControlValue*);
    static void pvp_set(ModContext*, void*, const UiControlValue*);
    static void voice_proximity_get(ModContext*, void*, UiControlValue*);
    static void voice_proximity_set(ModContext*, void*, const UiControlValue*);
    static void voice_proximity_range_get(ModContext*, void*, UiControlValue*);
    static void voice_proximity_range_set(ModContext*, void*, const UiControlValue*);
};

}  // namespace dusklight_online
