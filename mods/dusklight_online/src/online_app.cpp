#include "dusklight_online/game/appearance.hpp"
#include "dusklight_online/online_app.hpp"
#include "dusklight_online/game/coop_features.hpp"
#include "dusklight_online/logging.hpp"

#include "dusk/multiplayer/invite_code.hpp"
#include "dusklight_online/game/game_adapter.hpp"
#include "dusklight_online/game/local_pose.hpp"
#include "dusklight_online/game/protocol_router.hpp"
#include "dusklight_online/game/visual_bridge.hpp"
#include "dusklight_online/game/voice_chat.hpp"
#include "dusklight_online/net/sdk_room_channel.hpp"

#include <mods/service.hpp>
#include <mods/svc/config.h>
#include <mods/svc/host.h>
#include <mods/svc/ui.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include <SDL3/SDL_scancode.h>
#include "m_Do/m_Do_controller_pad.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace dusklight_online {
namespace {

constexpr uint32_t kManualSyncCooldownTicks = 5 * 30;
#if defined(DUSKLIGHT_ONLINE_VOICE)
constexpr bool kVoiceRuntimeAvailable = true;
#else
// A saved setting from another platform must not enable voice on this build.
constexpr bool kVoiceRuntimeAvailable = false;
#endif
constexpr const char* kCloudRoomServiceUrl =
    "https://dusklight-online-rooms.matthewdavidrichardanderson.workers.dev";

float master_volume_to_linear(float percent) {
    return percent <= 0.0f ? 0.0f :
        std::pow(10.0f, (percent / 100.0f - 1.0f) * 2.0f);
}

const char* connection_label(const net::Status& status) {
    if (status.mode == net::Mode::CloudRoom) return "NAT (Cloudflare room)";
    if (status.mode != net::Mode::Relay) return "Direct";
    if (status.natPeerCount && status.relayPeerCount) return "NAT/Relay (mixed)";
    if (status.natPeerCount) return "NAT";
    if (status.relayPeerCount) return "Relay";
    return "NAT/Relay";
}

std::string connected_health(const net::Status& status, size_t otherPlayers) {
    if (status.mode != net::Mode::CloudRoom)
        return status.mode == net::Mode::DirectHost || status.welcomed ?
            "Connected" : "Joining lobby";
    if (!status.welcomed) return "Joining lobby";
    if (otherPlayers == 0) return "Room ready — waiting for players";
    if (status.natPeerCount >= otherPlayers) return "Connected";
    if (status.natPeerCount == 0)
        return otherPlayers == 1 ? "Connecting to player" : "Connecting to players";
    return "Connecting to players (" + std::to_string(status.natPeerCount) +
        "/" + std::to_string(otherPlayers) + " ready)";
}

ModResult add_config(const char* name, ConfigVarType type, const char* defaultString,
                     int64_t defaultInt, bool defaultBool, ConfigVarHandle& output,
                     ModError* error) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = type;
    desc.default_string = defaultString;
    desc.default_int = defaultInt;
    desc.default_bool = defaultBool;
    if (svc_config->register_var(mod_ctx, &desc, &output) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online configuration");
    }
    return MOD_OK;
}

bool never_modified(ModContext*, void*) { return false; }

UiElementHandle add_bound_control(UiElementHandle pane, UiControlKind kind, const char* label,
                                  ConfigVarHandle variable, int64_t minimum = 0,
                                  int64_t maximum = 0, int64_t step = 1,
                                  int32_t maxLength = 0, UiPredicateFn isDisabled = nullptr,
                                  void* userData = nullptr, const char* styleClass = nullptr,
                                  const char* layoutClass = nullptr,
                                  const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = kind;
    control.label = label;
    control.help_rml = helpRml;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = variable;
    control.min = minimum;
    control.max = maximum;
    control.step = step;
    control.max_length = maxLength;
    control.is_disabled = isDisabled;
    control.is_modified = &never_modified;
    control.user_data = userData;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK &&
        element != 0 && styleClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, styleClass, true);
    }
    if (element != 0 && layoutClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, layoutClass, true);
    }
    return element;
}

void add_session_toggle(UiElementHandle pane, const char* label,
                        UiControlGetFn get, UiControlSetFn set,
                        UiPredicateFn isDisabled, OnlineApp* app,
                        const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = label;
    control.help_rml = helpRml;
    control.get = get;
    control.set = set;
    control.is_disabled = isDisabled;
    control.is_modified = &never_modified;
    control.user_data = app;
    svc_ui->pane_add_control(mod_ctx, pane, &control, nullptr);
}

UiElementHandle add_form_string(UiElementHandle pane, const char* label,
                                ConfigVarHandle variable, int32_t maxLength,
                                const char* layoutClass = nullptr,
                                const char* helpRml = nullptr) {
    return add_bound_control(pane, UI_CONTROL_STRING, label, variable, 0, 0, 1, maxLength,
                             nullptr, nullptr, "online-form-field", layoutClass, helpRml);
}

UiElementHandle add_button(UiElementHandle pane, const char* label, UiPressedFn callback,
                           void* userData, UiPredicateFn isDisabled = nullptr,
                           UiPredicateFn isModified = nullptr,
                           const char* styleClass = nullptr,
                           UiPredicateFn isSelected = nullptr,
                           const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_BUTTON;
    control.label = label;
    control.help_rml = helpRml;
    control.on_pressed = callback;
    control.user_data = userData;
    control.is_disabled = isDisabled;
    control.is_modified = isModified;
    control.is_selected = isSelected;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK &&
        element != 0 && styleClass != nullptr) {
        svc_ui->elem_set_class(mod_ctx, element, styleClass, true);
    }
    return element;
}

UiElementHandle add_code_control(UiElementHandle pane, const char* label, UiControlGetFn get,
                                 UiControlSetFn set, OnlineApp* app,
                                 const char* helpRml = nullptr) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_STRING;
    control.label = label;
    control.help_rml = helpRml;
    control.get = get;
    control.set = set;
    control.is_modified = &never_modified;
    control.user_data = app;
    control.max_length = 2048;
    UiElementHandle element = 0;
    if (svc_ui->pane_add_control(mod_ctx, pane, &control, &element) == MOD_OK && element != 0) {
        svc_ui->elem_set_class(mod_ctx, element, "online-code", true);
        svc_ui->elem_set_class(mod_ctx, element, "online-form-field", true);
        svc_ui->elem_set_class(mod_ctx, element, "online-code-row", true);
    }
    return element;
}

std::string rml_escape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string trim_clipboard_text(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string friendly_status_text(std::string text) {
    std::replace(text.begin(), text.end(), '_', ' ');
    if (!text.empty()) text.front() = static_cast<char>(std::toupper(
        static_cast<unsigned char>(text.front())));
    return text;
}

std::string friendly_error_sentence(std::string_view error) {
    if (error.empty()) return {};
    if (error == "lobby_not_found") return "No room with that Room name exists.";
    if (error == "lobby_exists") return "A room with that Room name already exists. Pick another Room name.";
    if (error == "bad_lobby_code") return "The room code is incorrect.";
    if (error == "lobby_full") return "The room is full.";
    if (error == "room_recovering") return "A lobby connection is recovering. Please try again shortly.";
    if (error == "protocol_version") return "The relay protocol is incompatible.";
    if (error == "invalid_lobby_code_proof") return "Lobby code verification failed.";
    if (error == "lobby_too_long") return "The Room name is too long.";
    if (error == "username_too_long") return "The player name is too long.";
    if (error == "remote closed") return "The connection was closed.";
    std::string sentence = friendly_status_text(std::string(error));
    if (!sentence.empty() && sentence.back() != '.' && sentence.back() != '!' &&
        sentence.back() != '?') {
        sentence.push_back('.');
    }
    return sentence;
}

std::string failure_message(std::string_view prefix, std::string_view detail) {
    std::string message(prefix);
    if (!message.empty() && message.back() != '.') message.push_back('.');
    const std::string reason = friendly_error_sentence(detail);
    if (!reason.empty()) {
        if (!message.empty()) message.push_back(' ');
        message += reason;
    }
    return message;
}

bool write_clipboard(std::string_view text) {
    return !text.empty() && mods::ui::set_clipboard_text(std::string(text)) == MOD_OK;
}

std::string read_clipboard() {
    std::string text;
    if (mods::ui::get_clipboard_text(text) != MOD_OK) return {};
    return trim_clipboard_text(std::move(text));
}

void push_clipboard_toast(const char* message, bool warning = false) {
    const std::string body = warning ?
        "<row><span>" + rml_escape(message) +
            "</span><icon class=\"warning\"></icon></row>" :
        rml_escape(message);
    UiToastDesc toast = UI_TOAST_DESC_INIT;
    toast.type = warning ? "online-warning" : "online";
    toast.body_rml = body.c_str();
    toast.duration_ms = 2500;
    svc_ui->push_toast(mod_ctx, &toast);
}

constexpr const char* kOnlineOverlayRcss = R"RCSS(
toast[mod-id="io.github.batdadmaker.dusklight_coop"] {
    top: 58dp;
    padding: 7dp 10dp;
    gap: 3dp;
    border-radius: 10dp;
}
toast.online heading,
toast.online-warning heading {
    display: none;
}
toast.online-warning {
    border: 1dp #C2A42D;
}
toast.online-warning message row {
    align-items: center;
    gap: 10dp;
}
toast.online-warning message row > span {
    flex: 1 1 auto;
}
)RCSS";

constexpr const char* kOnlineWindowRcss = R"RCSS(
window content pane:last-of-type > div {
    line-height: 1.2;
}
window content pane.online-form-pane {
    flex-flow: row wrap;
    align-content: flex-start;
}
window content pane.online-form-pane > div {
    flex: 0 0 100%;
}
window content pane.online-lobby-choice-pane {
    flex-flow: column;
    align-items: stretch;
}
window content pane.online-lobby-choice-pane > select-button.group-button {
    align-self: stretch;
}
.online-state {
    display: block;
    padding: 14dp 16dp;
    margin-bottom: 6dp;
    border: 1dp rgba(146, 135, 91, 55%);
    border-radius: 8dp;
    background-color: rgba(224, 219, 200, 5%);
}
.online-state-title {
    display: block;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
    font-size: 22dp;
    color: #e0dbc8;
}
.online-state-detail {
    display: block;
    margin-top: 4dp;
    font-size: 16dp;
    color: rgba(224, 219, 200, 70%);
}
.online-detail-row {
    display: flex;
    margin-bottom: 3dp;
}
.online-detail-label {
    display: block;
    flex: 0 0 116dp;
    width: 116dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
    color: rgba(224, 219, 200, 55%);
}
.online-detail-value {
    display: block;
    flex: 1 1 auto;
    min-width: 0;
}
.online-player-row,
.online-player-empty {
    display: flex;
    align-items: center;
    padding: 12dp 14dp;
    margin-bottom: 8dp;
    border: 1dp rgba(146, 135, 91, 55%);
    border-radius: 8dp;
    background-color: rgba(224, 219, 200, 5%);
}
.online-player-row {
    justify-content: space-between;
}
.online-player-identity {
    display: flex;
    align-items: center;
    flex: 1 1 auto;
    min-width: 0;
}
.online-player-indicator {
    display: block;
    flex: 0 0 8dp;
    width: 8dp;
    height: 8dp;
    margin-right: 11dp;
    border-radius: 4dp;
    background-color: #d4b83f;
    box-shadow: rgba(212, 184, 63, 35%) 0 0 5dp 1dp;
}
.online-player-name {
    display: block;
    flex: 1 1 0;
    min-width: 0;
    font-size: 20dp;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
}
.online-player-ping {
    display: block;
    flex: 0 0 76dp;
    width: 76dp;
    margin-left: 6dp;
    text-align: right;
    font-family: "Fira Sans Condensed";
    font-size: 18dp;
    color: rgba(224, 219, 200, 72%);
    white-space: nowrap;
}
.online-player-empty {
    display: block;
    color: rgba(224, 219, 200, 72%);
}
.online-player-empty-title,
.online-player-empty-detail {
    display: block;
}
.online-player-empty-detail {
    margin-top: 3dp;
    font-size: 16dp;
    color: rgba(224, 219, 200, 48%);
}
button.online-inline-kick {
    flex: 0 0 auto;
    min-width: 70dp;
    height: 34dp;
    margin-left: 14dp;
    padding: 0dp 13dp;
    border-radius: 6dp;
    line-height: 34dp;
    text-align: center;
    font-family: "Fira Sans Condensed";
    font-size: 16dp;
    font-weight: bold;
    color: #ff716a;
    background-color: rgba(226, 83, 77, 14%);
    box-shadow: rgba(226, 83, 77, 80%) 0 0 0 1dp;
}
button.online-inline-kick:not(:disabled):focus-visible,
button.online-inline-kick:not(:disabled):active {
    color: #fff0ed;
    background-color: rgba(226, 83, 77, 36%);
}
button.online-inline-kick:disabled {
    opacity: 0.4;
    cursor: default;
}
button.online-peer-selected {
    color: #e0dbc8;
    background-color: rgba(194, 164, 45, 24%);
    box-shadow: #d4b83f 0 0 0 1dp;
}
button.online-sync-pending {
    opacity: 0.35;
    cursor: default;
}
select-button.online-code {
    min-width: 0;
}
select-button.online-form-field {
    display: block;
    position: relative;
    flex: 1 1 100%;
    height: 57dp;
    min-width: 0;
    padding: 0dp;
    border-radius: 10dp;
    background-color: rgba(17, 16, 10, 32%);
}
select-button.online-form-field.online-half-field {
    flex: 1 1 100%;
}
select-button.online-code-row {
    flex: 1 1 100%;
}
select-button.online-form-field key {
    display: block;
    position: absolute;
    top: 7dp;
    left: 12dp;
    right: 12dp;
    height: 17dp;
    margin: 0dp;
    font-size: 15dp;
    line-height: 17dp;
    opacity: 0.72;
}
select-button.online-form-field value {
    display: block;
    position: absolute;
    top: 29dp;
    left: 12dp;
    right: 12dp;
    width: auto;
    height: 22dp;
    min-width: 0;
    text-align: left;
    font-size: 20dp;
    line-height: 22dp;
}
select-button.online-form-field input {
    position: absolute;
    top: 28dp;
    left: 12dp;
    right: 12dp;
    width: auto;
    height: 24dp;
    min-width: 0;
    text-align: left;
    font-size: 20dp;
    line-height: 22dp;
}
select-button.online-code value {
    display: block;
    right: auto;
    width: 92%;
    min-width: 0;
    max-width: 92%;
    overflow: hidden;
    white-space: nowrap;
    text-overflow: ellipsis;
}
select-button.online-code input {
    min-width: 0;
}
button.online-copy-action {
    display: block;
    flex: 1 1 100%;
    height: 32dp;
    min-height: 32dp;
    align-self: center;
    padding: 0dp 12dp;
    line-height: 32dp;
    text-align: center;
    font-size: 17dp;
    font-weight: normal;
    color: rgba(204, 184, 119, 92%);
    box-shadow: rgba(146, 135, 91, 25%) 0 0 0 1dp;
}
button.online-primary-action {
    color: #d4b83f;
    box-shadow: rgba(194, 164, 45, 65%) 0 0 0 1dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
}
button.online-danger-action {
    color: #e2534d;
    box-shadow: rgba(226, 83, 77, 65%) 0 0 0 1dp;
    font-family: "Fira Sans Condensed";
    font-weight: bold;
}
select-button.online-wide-control {
    flex: 1 1 100%;
}
window content pane.online-form-pane > button.online-primary-action,
window content pane.online-form-pane > button.online-danger-action {
    flex: 1 1 100%;
}
button.online-primary-action:disabled,
button.online-danger-action:disabled,
button.online-copy-action:disabled {
    display: none;
}
window content pane.online-form-pane > .online-manual-only-hidden {
    display: none;
}
window content pane.online-form-pane > button.online-primary-action:not(:disabled):focus-visible,
window content pane.online-form-pane > button.online-primary-action:not(:disabled):active,
window content pane.online-form-pane > button.online-danger-action:not(:disabled):focus-visible,
window content pane.online-form-pane > button.online-danger-action:not(:disabled):active {
    opacity: 0.9;
    background-color: rgba(17, 16, 10, 20%);
}
window content pane.online-form-pane > button:not(:disabled):selected,
window content pane.online-form-pane > select-button:not(:disabled):selected,
window content pane.online-session-pane > button:not(:disabled):selected {
    opacity: 1;
    background-color: rgba(17, 16, 10, 0%);
}
)RCSS";

const char* mode_text(net::Mode mode) {
    switch (mode) {
    case net::Mode::DirectHost: return "direct host";
    case net::Mode::DirectJoin: return "direct join";
    case net::Mode::Relay: return "relay";
    case net::Mode::CloudRoom: return "cloud room";
    default: return "disabled";
    }
}

const char* state_text(net::State state) {
    switch (state) {
    case net::State::Listening: return "listening";
    case net::State::Connecting: return "connecting";
    case net::State::Connected: return "connected";
    default: return "disconnected";
    }
}

bool room_settings_locked(const net::Status& status, bool relayHostIntent) {
    if (!status.enabled) return false;
    if (status.mode == net::Mode::DirectJoin) return true;
    if (net::is_room_mode(status.mode)) return !relayHostIntent && !status.isOwner;
    return false;
}


}  // namespace

OnlineApp::OnlineApp() = default;
OnlineApp::~OnlineApp() = default;

ModResult OnlineApp::initialize(ModError* error) {
    const char* dataDir = nullptr;
    if (svc_host->header.minor_version >= 2 &&
        svc_host->data_dir(mod_ctx, &dataDir) == MOD_OK && dataDir != nullptr) {
        hostConfigPath_ = std::filesystem::u8path(dataDir).parent_path().parent_path() /
                          "config.json";
        masterVolumeGain_ = master_volume_to_linear(60.0f);
    }
    if (register_config(error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (register_ui(error) != MOD_OK) {
        return MOD_ERROR;
    }
    speedrunAvailable_ = game::speedrun::initialize();
    if (!speedrunAvailable_) log_info("Speedrun controls unavailable: native symbols not found");
    game_ = std::make_unique<game::GameAdapter>(transport_);
    if (game_->initialize_hooks(error) != MOD_OK) {
        game_.reset();
        return MOD_ERROR;
    }
    router_ = std::make_unique<game::ProtocolRouter>(*game_);
    voice_ = std::make_unique<game::VoiceChat>();
    return MOD_OK;
}

void OnlineApp::refresh_host_settings() {
    if (hostConfigPath_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (lastHostConfigPoll_ != std::chrono::steady_clock::time_point{} &&
        now - lastHostConfigPoll_ < std::chrono::milliseconds(250)) return;
    lastHostConfigPoll_ = now;

    std::error_code error;
    const auto written = std::filesystem::last_write_time(hostConfigPath_, error);
    if (error || (lastHostConfigWrite_ && *lastHostConfigWrite_ == written)) return;

    std::ifstream file(hostConfigPath_);
    if (!file) return;
    const auto config = nlohmann::json::parse(file, nullptr, false);
    if (!config.is_object()) return;
    const auto mirror = config.find("game.enableMirrorMode");
    game::set_host_mirror_mode(mirror != config.end() && mirror->is_boolean() &&
                               mirror->get<bool>());
    float percent = 60.0f;  // Dusklight's default when the key is absent.
    const auto value = config.find("audio.masterVolume");
    if (value != config.end()) {
        if (!value->is_number()) return;
        percent = static_cast<float>(value->get<double>());
    }
    percent = std::clamp(percent, 0.0f, 100.0f);
    // Match Dusklight's MasterVolumeToLinear curve for the separate voice stream.
    masterVolumeGain_ = master_volume_to_linear(percent);
    lastHostConfigWrite_ = written;
}

void OnlineApp::consume_progression_prompt_input() {
    if (game_ != nullptr)
        game_->set_progression_prompts_enabled(bool_value(config_.syncPrompts));
    speedrunDownHeld_ = false;
    if (speedrunPhase_ == SpeedrunPhase::WaitingReady && !speedrunLocalPlayerReady_) {
        interface_of_controller_pad& pad = mDoCPd_c::getCpadInfo(PAD_1);
        speedrunDownHeld_ = (pad.mButtonFlags & PAD_BUTTON_DOWN) != 0;
        pad.mButtonFlags &= ~PAD_BUTTON_DOWN;
        pad.mPressedButtonFlags &= ~PAD_BUTTON_DOWN;
        return;
    }
    if (game_ != nullptr && speedrunPhase_ == SpeedrunPhase::None)
        game_->consume_progression_prompt_input();
}

void OnlineApp::match_player_colour() {
    if (!bool_value(config_.matchOutfitColor, true)) return;
    const auto outfit = string_value(config_.outfitColor);
    if (string_value(config_.playerColor) != outfit)
        svc_config->set_string(mod_ctx, config_.playerColor, outfit.c_str());
}

void OnlineApp::update() {
    if (game_ != nullptr)
        game_->set_progression_prompts_enabled(bool_value(config_.syncPrompts));
    if (const auto binding = game::take_voice_mute_hotkey_binding()) {
        svc_config->set_int(mod_ctx, config_.voiceMuteHotkey, *binding);
    }
    if (game::take_voice_mute_hotkey_toggle() && bool_value(config_.voiceEnabled)) {
        svc_config->set_bool(mod_ctx, config_.voiceMicMuted,
                             !bool_value(config_.voiceMicMuted));
    }
    using TimingClock = std::chrono::steady_clock;
    struct PoseTiming {
        TimingClock::time_point start = TimingClock::now(), previous = start;
        uint64_t updates = 0, poses = 0, skipped = 0, errors = 0;
        double maxUpdateGap = 0;
        std::map<std::string, uint32_t> sequences;
    };
    static PoseTiming timing;
    const auto timingNow = TimingClock::now();
    const auto timingStatus = transport_.status();
    if (!timingStatus.welcomed) {
        timing = {};
    } else {
        timing.maxUpdateGap = (std::max)(timing.maxUpdateGap,
            std::chrono::duration<double, std::milli>(timingNow - timing.previous).count());
        timing.previous = timingNow;
        ++timing.updates;
        if (timingNow - timing.start >= std::chrono::seconds(1)) {
            bool foreground = true;
#ifdef _WIN32
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            foreground = foregroundPid == GetCurrentProcessId();
#endif
            std::ostringstream line;
            line << "UDP_TRANSPORT_TIMING mode=" << static_cast<int>(timingStatus.mode)
                 << " interval_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(timingNow-timing.start).count()
                 << " updates=" << timing.updates << " max_update_gap_ms=" << timing.maxUpdateGap
                 << " poses=" << timing.poses << " skipped_sequences=" << timing.skipped
                 << " errors=" << timing.errors << " foreground=" << foreground
                 << " tx_sequence=" << transport_.last_visual_send_stats().sequence;
            log_info(line.str());
            timing.start = timingNow;
            timing.updates = timing.poses = timing.skipped = timing.errors = 0;
            timing.maxUpdateGap = 0;
        }
    }
    match_player_colour();
    if (game_) game_->set_player_color(game::appearance::parse_color(
        string_value(config_.playerColor)).value_or(game::appearance::default_color),
        game::appearance::parse_color(string_value(config_.outfitColor))
            .value_or(game::appearance::default_color),
        game::appearance::parse_color(string_value(config_.zoraColor))
            .value_or(game::appearance::default_color));
    if (reopenWindowPending_ && window_ == 0) {
        reopenWindowPending_ = false;
        open_window();
    }
    if (reopenSyncWindowPending_ && syncWindow_ == 0) {
        reopenSyncWindowPending_ = false;
        open_sync_window();
    }
    if (auto chat = game::take_chat_submission()) {
        const net::Status status = transport_.status();
        const bool chatAvailable = status.enabled &&
            (status.mode == net::Mode::DirectHost ?
                (status.state == net::State::Listening || status.state == net::State::Connected) :
                status.welcomed);
        if (!chatAvailable) {
            game::push_online_notification("Chat is unavailable while disconnected.", 4.0f, true);
        } else if (transport_.send({{"type", "chat"}, {"text", *chat}})) {
            game::push_chat_message(status.name, std::move(*chat), game::appearance::local_color());
        } else {
            game::push_online_notification("Could not send chat message.", 4.0f, true);
        }
    }
    transport_.tick();
    const net::Status captureStatus = transport_.status();
    refresh_host_settings();
    if (voice_ != nullptr) {
        const bool voiceEnabled = kVoiceRuntimeAvailable && captureStatus.welcomed &&
                                  captureStatus.voiceSettingsReady &&
                                  bool_value(config_.voiceEnabled);
        voice_->configure(voiceEnabled, bool_value(config_.voiceMicMuted),
                          string_value(config_.voiceInput),
                          static_cast<int>(int_value(config_.micVolume, 100)),
                          static_cast<int>(int_value(config_.playerVolume, 100)),
                          captureStatus.mode == net::Mode::DirectHost ||
                              captureStatus.mode == net::Mode::DirectJoin,
                          masterVolumeGain_);
        const std::string& voiceError = voice_->error();
        if (voiceEnabled && !voiceError.empty() && voiceError != lastVoiceError_)
            game::push_online_notification(voiceError, 4.0f, true);
        lastVoiceError_ = voiceEnabled ? voiceError : std::string();
    }
    const bool captureSyncFlags = captureStatus.enabled ? captureStatus.settings.syncFlags :
                                                          bool_value(config_.syncFlags, true);
    if (game_ != nullptr) {
        game_->set_save_recovery_policy(
            captureStatus.enabled && captureStatus.saveRecoverySettingsReady,
            captureStatus.settings.saveRecovery);
        game_->restore_saved_progress_if_ready();
        game_->capture_local_mutations_before_remote(captureSyncFlags);
    }
    bool protocolFatal = false;
    while (transport_.has_events()) {
        net::Event event = transport_.pop_event();
        if (event.kind == net::EventKind::UdpMessage) {
            ++timing.poses;
            auto& previous = timing.sequences[event.peerId + ":" + std::to_string(static_cast<int>(event.udpType))];
            if (previous && event.udpSequence > previous && event.udpSequence - previous > 1)
                timing.skipped += event.udpSequence - previous - 1;
            previous = event.udpSequence;
        } else if (event.kind == net::EventKind::Error) {
            if (++timing.errors == 1) log_info("UDP_TRANSPORT_TIMING first_error=" + event.detail);
        } else if (event.kind == net::EventKind::PeerLeft) {
            // Peer IDs are session-local; bound the diagnostic history on churn.
            timing.sequences.clear();
            if (voice_ != nullptr) voice_->peer_left(event.peerId);
            if (speedrunRequestId_ != 0 && speedrunParticipants_.contains(event.peerId))
                cancel_speedrun_start("Speedrun start cancelled: a player left.");
        } else if (event.kind == net::EventKind::PeerJoined && speedrunRequestId_ != 0) {
            cancel_speedrun_start("Speedrun start cancelled: player list changed.");
        }
        if (event.kind == net::EventKind::Message &&
            event.message.value("type", std::string()) == "owner_changed" &&
            net::is_room_mode(event.ingress.mode)) {
            if (speedrunPhase_ != SpeedrunPhase::None) {
                clear_speedrun_request();
                game::push_online_notification("Speedrun start cancelled: host changed.",
                                               4.0f, true);
            }
            const std::string ownerId = event.message.value("owner_client_id", std::string());
            const bool isOwner = !event.ingress.clientId.empty() &&
                                 ownerId == event.ingress.clientId;
            if (relayOwnerStateKnown_ && !wasRelayOwner_ && isOwner) {
                game::push_online_notification("You are now the lobby host.");
            }
            relayOwnerStateKnown_ = true;
            wasRelayOwner_ = isOwner;
        }
        switch (event.kind) {
        case net::EventKind::UdpVoice:
            if (kVoiceRuntimeAvailable && voice_ != nullptr &&
                transport_.status().voiceSettingsReady &&
                bool_value(config_.voiceEnabled)) {
                const net::RoomSettings settings = transport_.status().settings;
                const bool proximity = settings.voiceProximity;
                const int rangePercent = settings.voiceProximityRange;
                const float gain = proximity && game_ != nullptr ?
                    game_->voice_gain(event.voicePosition, rangePercent) :
                    (proximity ? 0.0f : 1.0f);
                const float pan = proximity && game_ != nullptr ?
                    game_->voice_pan(event.voicePosition) : 0.5f;
                voice_->receive(event.peerId, event.udpSequence, event.voice,
                                gain, pan);
            }
            break;
        case net::EventKind::Diagnostic:
            log_info("MP_CLOUD_DIAG peer=" + (event.peerId.empty() ? "-" : event.peerId) +
                     " " + event.detail);
            break;
        case net::EventKind::RouteChanged:
            log_info("MP_PEER_ROUTE peer=" + event.peerId + " realtime=" + event.detail + " reliable=" + event.detail);
            break;
        case net::EventKind::Connected: {
            const bool reconnected = !connectedLobbyName_.empty();
            connectedLobbyName_ = transport_.status().room;
            statusMessage_ = event.ingress.mode == net::Mode::CloudRoom ?
                "Joined Cloudflare room " + connectedLobbyName_ :
                "Connected to " + connectedLobbyName_;
            game::push_online_notification(
                std::string(reconnected ? "Reconnected to lobby " :
                            relayHostIntent_ ? "Hosting lobby " : "Joined lobby ") +
                connectedLobbyName_ + ".");
            pendingLobbyFailurePrefix_.clear();
            pendingLobbyFailureNotified_ = false;
            relayOwnerStateKnown_ = net::is_room_mode(event.ingress.mode);
            wasRelayOwner_ = relayOwnerStateKnown_ &&
                event.message.value("owner_client_id", std::string()) ==
                    event.message.value("client_id", event.ingress.clientId);
            livePublishInitialized_ = false;
            break;
        }
        case net::EventKind::Disconnected:
            clear_speedrun_request();
            speedrunResetAt_ = {};
            if (voice_ != nullptr) voice_->stop();
            if (event.ingress.mode == net::Mode::CloudRoom)
                log_info("MP_CLOUD_DISCONNECT reason=" + event.detail);
            if (transport_.status().reconnecting) {
                statusMessage_ = "Connection lost — reconnecting to lobby " + connectedLobbyName_;
                game::push_online_notification(statusMessage_ + ".", 5.0f, true);
            } else if (!connectedLobbyName_.empty()) {
                if (event.detail == "user requested") {
                    game::push_online_notification("Left lobby " + connectedLobbyName_ + ".");
                } else {
                    game::push_online_notification(
                        failure_message("Disconnected from lobby " + connectedLobbyName_,
                                        event.detail),
                        5.0f, true);
                }
                connectedLobbyName_.clear();
            } else if (event.detail != "user requested" && !pendingLobbyFailurePrefix_.empty()) {
                notify_lobby_attempt_failure(event.detail);
            }
            if (event.detail == "user requested" && !requestedDisconnectStatus_.empty()) {
                statusMessage_ = requestedDisconnectStatus_;
            } else if (!transport_.status().reconnecting) {
                statusMessage_ = event.detail.empty() ? "Disconnected" :
                    "Disconnected: " + event.detail;
            }
            requestedDisconnectStatus_.clear();
            pendingLobbyFailurePrefix_.clear();
            pendingLobbyFailureNotified_ = false;
            if (!transport_.status().reconnecting) {
                relayHostIntent_ = false;
                activeCode_.clear();
            }
            relayOwnerStateKnown_ = false;
            wasRelayOwner_ = false;
            manualSyncCooldownTicks_ = 0;
            set_manual_sync_pending_visual(false);
            if (router_ != nullptr) {
                router_->clear();
            }
            if (game_ != nullptr) {
                game_->reset_session();
            }
            break;
        case net::EventKind::Error:
            if (event.ingress.mode == net::Mode::CloudRoom)
                log_info("MP_CLOUD_ERROR reason=" + event.detail);
            if (!pendingLobbyFailurePrefix_.empty()) {
                statusMessage_ = "Online error: " + event.detail;
                notify_lobby_attempt_failure(event.detail);
                // A failed attempt is terminal, including malformed handshake replies.
                requestedDisconnectStatus_ = statusMessage_;
                disconnect();
            } else if (!event.detail.empty()) {
                statusMessage_ = "Online error: " + event.detail;
                game::push_online_notification(
                    failure_message("Online error", event.detail), 5.0f, true);
            }
            break;
        default:
            if (event.kind == net::EventKind::Message) {
                const std::string type = event.message.value("type", std::string());
                if (type == "speedrun_check" || type == "speedrun_ready" ||
                    type == "speedrun_prompt" || type == "speedrun_player_ready" ||
                    type == "speedrun_ready_count" || type == "speedrun_countdown" ||
                    type == "speedrun_cancel" || type == "speedrun_start" ||
                    type == "speedrun_reset") {
                    handle_speedrun_message(event);
                    break;
                }
            }
            if (router_ != nullptr) {
                const net::Status routeStatus = transport_.status();
                const bool routeSyncFlags = routeStatus.enabled ? routeStatus.settings.syncFlags :
                    bool_value(config_.syncFlags, true);
                const game::ApplyResult result = router_->route(event, routeSyncFlags);
                if ((result == game::ApplyResult::Rejected ||
                     result == game::ApplyResult::Unsupported) &&
                    !router_->last_error().empty()) {
                    statusMessage_ = "Online protocol error: " + router_->last_error();
                }
                if (router_->fatal_error()) {
                    statusMessage_ = "Online protocol overload: " + router_->last_error();
                    transport_.disconnect();
                    router_->clear();
                    if (game_ != nullptr) game_->reset_session();
                    protocolFatal = true;
                }
            }
            break;
        }
        if (protocolFatal) break;
    }
    tick_speedrun();
    update_speedrun_prompt();
    update_speedrun_mods_visibility();
    if (kVoiceRuntimeAvailable && voice_ != nullptr && transport_.status().welcomed &&
        transport_.status().voiceSettingsReady &&
        bool_value(config_.voiceEnabled)) {
        const auto position = game_ != nullptr ? game_->voice_position() : std::nullopt;
        const bool proximity = transport_.status().settings.voiceProximity;
        const net::udp::VoicePosition voicePosition =
            position.value_or(net::udp::VoicePosition{});
        for (const auto& frame : voice_->capture()) {
            if (!position && proximity) continue;
            // Global voice can transmit from menus, where Link has no position.
            (void)transport_.send_voice(frame.sequence, voicePosition, frame.bytes);
        }
    }
    const net::Status currentStatus = transport_.status();
    // A direct host owns a lobby before any guests arrive. Joiners must
    // receive welcome; a connected socket alone is not lobby membership.
    const bool lobbyActive = currentStatus.enabled &&
        (currentStatus.mode == net::Mode::DirectHost
            ? (currentStatus.state == net::State::Listening ||
               currentStatus.state == net::State::Connected)
            : currentStatus.welcomed);
    game::appearance::set_lobby_active(lobbyActive);
    game::set_voice_mute_indicator(lobbyActive && kVoiceRuntimeAvailable &&
                                   voice_ != nullptr &&
                                   bool_value(config_.voiceEnabled) &&
                                   bool_value(config_.voiceMicMuted));
    if (protocolFatal) return;
    const bool syncFlagsEnabled = currentStatus.enabled ? currentStatus.settings.syncFlags :
        bool_value(config_.syncFlags, true);
    if (router_ != nullptr) {
        router_->flush(syncFlagsEnabled);
    }
    if (game_ != nullptr) {
        const bool remoteModelEnabled = currentStatus.enabled ? currentStatus.settings.dummyModel :
            bool_value(config_.dummyModel, true);
        // World synchronization remains protocol-compatible but is intentionally
        // dormant until it is complete enough to expose as a supported feature.
        constexpr bool syncWorldEnabled = false;
        const bool remoteCollisionEnabled = currentStatus.enabled ?
            net::effective_remote_collision(currentStatus.settings) :
            bool_value(config_.dummyModel, true) &&
                bool_value(config_.remoteCollision, true);
        const bool pvpEnabled = currentStatus.enabled ?
            net::effective_pvp(currentStatus.settings) :
            remoteCollisionEnabled && bool_value(config_.pvp, true);
        // Semantic recreation is the normal representation. Matrix streaming is
        // retained only as an explicit developer diagnostic and is never a room
        // setting that another peer can enable or disable.
        const bool matrixStreamingEnabled = game::matrix_streaming_enabled();
        game::coop::set_local_options(game::coop::Options{
            bool_value(config_.sharedHp, true), bool_value(config_.bossDoorTogether, true)});
        game_->update(syncFlagsEnabled, syncWorldEnabled, remoteModelEnabled,
                      bool_value(config_.nameLabels, true), false,
                      matrixStreamingEnabled,
                      remoteCollisionEnabled, pvpEnabled,
                      bool_value(config_.playerList, false));
        const bool waiting = game_->manual_sync_waiting();
        if (manualSyncWasWaiting_ && !waiting && game_->manual_sync_failed()) {
            if (game_->manual_sync_timed_out()) {
                game::push_online_notification(
                    "Sync request failed. Ensure both players are not in a cutscene.",
                    5.0f, true);
            } else {
                game::push_online_notification("Sync request failed.", 5.0f, true);
            }
        }
        manualSyncWasWaiting_ = waiting;
    }
    if (manualSyncCooldownTicks_ > 0 && --manualSyncCooldownTicks_ == 0) {
        set_manual_sync_pending_visual(false);
    }
    const int muteHotkey = static_cast<int>(std::clamp(
        int_value(config_.voiceMuteHotkey, SDL_SCANCODE_M),
        int64_t{SDL_SCANCODE_UNKNOWN}, int64_t{SDL_SCANCODE_COUNT - 1}));
    game::configure_voice_mute_hotkey(muteHotkey, kVoiceRuntimeAvailable &&
        voice_ != nullptr && bool_value(config_.voiceEnabled));
    if (voiceMuteHotkeyText_ != 0) {
        const std::string text = game::voice_mute_hotkey_capture_active() ?
            "Press a key to bind (Esc cancels; Backspace clears)." :
            "Mute hotkey: " + game::voice_mute_hotkey_name(muteHotkey);
        if (text != renderedVoiceMuteHotkeyText_) {
            if (svc_ui->elem_set_text(mod_ctx, voiceMuteHotkeyText_, text.c_str()) == MOD_OK)
                renderedVoiceMuteHotkeyText_ = text;
            else voiceMuteHotkeyText_ = 0;
        }
    }
    publish_live_options();
}

void OnlineApp::shutdown() {
    game::cancel_voice_mute_hotkey_capture();
    if (voice_ != nullptr) voice_->stop();
    transport_.disconnect();
    game::appearance::set_lobby_active(false);
    if (router_ != nullptr) {
        router_->clear();
    }
    if (game_ != nullptr) {
        game_->reset_session();
        game_->shutdown_hooks();
    }
    if (playerOptionsWindow_ != 0) {
        svc_ui->window_close(mod_ctx, playerOptionsWindow_);
        playerOptionsWindow_ = 0;
    }
    if (speedrunWindow_ != 0) {
        svc_ui->window_close(mod_ctx, speedrunWindow_);
        speedrunWindow_ = 0;
    }
    clear_speedrun_request();
    speedrunResetAt_ = {};
    if (voiceWindow_ != 0) {
        svc_ui->window_close(mod_ctx, voiceWindow_);
        voiceWindow_ = 0;
    }
    voiceMuteHotkeyText_ = 0;
    renderedVoiceMuteHotkeyText_.clear();
    if (settingsWindow_ != 0) {
        svc_ui->window_close(mod_ctx, settingsWindow_);
        settingsWindow_ = 0;
    }
    if (syncWindow_ != 0) {
        svc_ui->window_close(mod_ctx, syncWindow_);
        syncWindow_ = 0;
    }
    if (lobbyWindow_ != 0) {
        svc_ui->window_close(mod_ctx, lobbyWindow_);
        lobbyWindow_ = 0;
    }
    if (window_ != 0) {
        svc_ui->window_close(mod_ctx, window_);
        window_ = 0;
    }
    if (menuTab_ != 0) {
        svc_ui->unregister_menu_tab(mod_ctx, menuTab_);
        menuTab_ = 0;
    }
    if (overlayStyle_ != 0) {
        svc_ui->unregister_styles(mod_ctx, overlayStyle_);
        overlayStyle_ = 0;
    }
    if (hideActiveModsStyle_ != 0) {
        svc_ui->unregister_styles(mod_ctx, hideActiveModsStyle_);
        hideActiveModsStyle_ = 0;
    }
    panelStatus_ = 0;
    windowStatus_ = 0;
    windowPlayersEmpty_ = 0;
    for (auto& slot : inlineKickRows_) slot = {};
    router_.reset();
    game_.reset();
    voice_.reset();
    livePublishInitialized_ = false;
    manualSyncWasWaiting_ = false;
    manualSyncCooldownTicks_ = 0;
    manualSyncFlagsButton_ = 0;
    manualSyncWarpButton_ = 0;
    relayOwnerStateKnown_ = false;
    wasRelayOwner_ = false;
    pendingLobbyFailurePrefix_.clear();
    pendingLobbyFailureNotified_ = false;
    connectedLobbyName_.clear();
}

ModResult OnlineApp::register_config(ModError* error) {
    struct StringVar { const char* name; const char* value; ConfigVarHandle* handle; };
    const std::array strings = {
        StringVar{"player-name", "Player", &config_.playerName},
        StringVar{"player-color", "", &config_.playerColor},
        StringVar{"outfit-color", "inherit", &config_.outfitColor},
        StringVar{"zora-color", "inherit", &config_.zoraColor},
        StringVar{"direct-room", "Lobby", &config_.directRoom},
        StringVar{"bind-host", "0.0.0.0", &config_.bindHost},
        StringVar{"public-host", "127.0.0.1", &config_.publicHost},
        StringVar{"direct-invite", "", &config_.directInvite},
        StringVar{"relay-code", "", &config_.relayCode},
        StringVar{"relay-room", "Lobby", &config_.relayRoom},
        StringVar{"relay-password", "", &config_.lobbyCode},
        StringVar{"voice-input", "", &config_.voiceInput},
    };
    for (const auto& variable : strings) {
        if (add_config(variable.name, CONFIG_VAR_STRING, variable.value, 0, false,
                       *variable.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }
    // One-time migration preserves the old combined colour. Empty is an
    // explicit original-outfit choice and must never inherit again.
    if (string_value(config_.outfitColor) == "inherit") {
        const auto previous = game::appearance::parse_color(string_value(config_.playerColor))
                                  .value_or(game::appearance::default_color);
        const auto value = game::appearance::color_string(previous);
        if (svc_config->set_string(mod_ctx, config_.outfitColor, value.c_str()) != MOD_OK)
            return MOD_ERROR;
    }
    // The migration sentinel is not a UI default. Re-register before any
    // controls bind; the service preserves the effective value on unregister.
    if (svc_config->unregister_var(mod_ctx, config_.outfitColor) != MOD_OK)
        return MOD_ERROR;
    config_.outfitColor = 0;
    if (add_config("outfit-color", CONFIG_VAR_STRING, "", 0, false,
                   config_.outfitColor, error) != MOD_OK)
        return MOD_ERROR;
    // Existing users had one outfit colour for both tunic and Zora Armour.
    // Preserve that appearance once, then let the two choices diverge.
    if (string_value(config_.zoraColor) == "inherit") {
        const auto value = string_value(config_.outfitColor);
        if (svc_config->set_string(mod_ctx, config_.zoraColor, value.c_str()) != MOD_OK)
            return MOD_ERROR;
    }
    if (svc_config->unregister_var(mod_ctx, config_.zoraColor) != MOD_OK)
        return MOD_ERROR;
    config_.zoraColor = 0;
    if (add_config("zora-color", CONFIG_VAR_STRING, "", 0, false,
                   config_.zoraColor, error) != MOD_OK)
        return MOD_ERROR;
    if (add_config("port", CONFIG_VAR_INT, nullptr, 34197, false, config_.port, error) != MOD_OK) {
        return MOD_ERROR;
    }
    if (add_config("voice-mic-volume", CONFIG_VAR_INT, nullptr, 100, false,
                   config_.micVolume, error) != MOD_OK ||
        add_config("voice-player-volume", CONFIG_VAR_INT, nullptr, 100, false,
                   config_.playerVolume, error) != MOD_OK ||
        add_config("voice-proximity-range", CONFIG_VAR_INT, nullptr, 50, false,
                   config_.voiceProximityRange, error) != MOD_OK ||
        add_config("voice-mute-hotkey", CONFIG_VAR_INT, nullptr, SDL_SCANCODE_M, false,
                   config_.voiceMuteHotkey, error) != MOD_OK) return MOD_ERROR;
    struct BoolVar { const char* name; bool value; ConfigVarHandle* handle; };
    const std::array booleans = {
        BoolVar{"match-outfit-color", true, &config_.matchOutfitColor},
        BoolVar{"relay-local", false, &config_.relayLocal},
        BoolVar{"relay-manual-host", false, &config_.relayManualHost},
        BoolVar{"remote-model", true, &config_.dummyModel},
        BoolVar{"name-labels", true, &config_.nameLabels},
        BoolVar{"sync-flags", true, &config_.syncFlags},
        BoolVar{"sync-prompts", false, &config_.syncPrompts},
        BoolVar{"save-recovery", true, &config_.saveRecovery},
        BoolVar{"display-midna", false, &config_.displayMidna},
        BoolVar{"remote-collision", true, &config_.remoteCollision},
        BoolVar{"pvp", true, &config_.pvp},
        BoolVar{"shared-hp", true, &config_.sharedHp},
        BoolVar{"boss-door-together", true, &config_.bossDoorTogether},
        BoolVar{"player-list-overlay", false, &config_.playerList},
        BoolVar{"speedrun-hide-active-mods", false, &config_.hideActiveMods},
        BoolVar{"voice-enabled", false, &config_.voiceEnabled},
        BoolVar{"voice-proximity", true, &config_.voiceProximity},
        BoolVar{"voice-mic-muted", false, &config_.voiceMicMuted},
    };
    for (const auto& variable : booleans) {
        if (add_config(variable.name, CONFIG_VAR_BOOL, nullptr, 0, variable.value,
                       *variable.handle, error) != MOD_OK) {
            return MOD_ERROR;
        }
    }
    return MOD_OK;
}

ModResult OnlineApp::register_ui(ModError* error) {
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = &OnlineApp::build_panel;
    panel.update = &OnlineApp::update_panel;
    panel.user_data = this;
    if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online UI panel");
    }
    UiMenuTabDesc menu = UI_MENU_TAB_DESC_INIT;
    menu.label = "Multiplayer";
    menu.on_selected = &OnlineApp::menu_selected;
    menu.user_data = this;
    if (svc_ui->register_menu_tab(mod_ctx, &menu, &menuTab_) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online menu tab");
    }
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_OVERLAY, kOnlineOverlayRcss,
                                &overlayStyle_) != MOD_OK) {
        return mods::set_error(error, MOD_ERROR, "failed to register Online overlay styles");
    }
    return MOD_OK;
}

std::string OnlineApp::string_value(ConfigVarHandle handle) const {
    size_t length = 0;
    if (svc_config->get_string(mod_ctx, handle, nullptr, 0, &length) != MOD_OK) {
        return {};
    }
    std::vector<char> bytes(length + 1, '\0');
    if (svc_config->get_string(mod_ctx, handle, bytes.data(), bytes.size(), nullptr) != MOD_OK) {
        return {};
    }
    return std::string(bytes.data(), length);
}

bool OnlineApp::bool_value(ConfigVarHandle handle, bool fallback) const {
    bool value = fallback;
    svc_config->get_bool(mod_ctx, handle, &value);
    return value;
}

int64_t OnlineApp::int_value(ConfigVarHandle handle, int64_t fallback) const {
    int64_t value = fallback;
    svc_config->get_int(mod_ctx, handle, &value);
    return value;
}

net::RoomSettings OnlineApp::configured_settings() const {
    net::RoomSettings settings;
    settings.dummyModel = bool_value(config_.dummyModel, true);
    settings.syncFlags = bool_value(config_.syncFlags, true);
    settings.saveRecovery = bool_value(config_.saveRecovery, true);
    settings.syncWorld = false;
    settings.remoteCollision = bool_value(config_.remoteCollision, true);
    settings.pvp = bool_value(config_.pvp, true) && settings.remoteCollision;
    settings.voiceProximity = bool_value(config_.voiceProximity, true);
    settings.voiceProximityRange = static_cast<int>(std::clamp(
        int_value(config_.voiceProximityRange, 50), int64_t{0}, int64_t{200}));
    return settings;
}

net::RoomSettings OnlineApp::displayed_settings() const {
    const net::Status status = transport_.status();
    return status.enabled ? status.settings : configured_settings();
}

std::string OnlineApp::status_text() const {
    const net::Status status = transport_.status();
    std::string connection = "—";
    std::string session = "—";
    std::string health = "Disconnected";
    if (status.enabled) {
        connection = connection_label(status);
        const size_t playerCount = transport_.peers().size() + 1u;
        session = (status.room.empty() ? std::string("Unnamed lobby") : status.room) +
            " — " + std::to_string(playerCount) + (playerCount == 1 ? " player" : " players");
        if (status.reconnecting) {
            health = "Reconnecting";
        } else if (!status.error.empty()) {
            health = "Error — " + friendly_status_text(status.error);
        } else {
            switch (status.state) {
            case net::State::Connected:
                health = connected_health(status, playerCount - 1u);
                break;
            case net::State::Connecting: health = "Connecting"; break;
            case net::State::Listening: health = "Ready — waiting for players"; break;
            default: health = "Disconnected"; break;
            }
        }
    } else if (!status.error.empty()) {
        health = "Error — " + friendly_status_text(status.error);
    } else if (!statusMessage_.empty() && statusMessage_ != "Not connected") {
        health = friendly_status_text(statusMessage_);
    }

    const auto summary_row = [](std::string_view label, std::string_view value) {
        return "<div style=\"display: flex; margin-bottom: 3dp;\">"
            "<span style=\"display: block; flex: 0 0 116dp; width: 116dp; "
            "font-family: 'Fira Sans Condensed'; font-weight: bold; "
            "color: rgba(224, 219, 200, 55%);\">" + rml_escape(label) +
            "</span><span style=\"display: block; flex: 1 1 auto; min-width: 0;\">" +
            rml_escape(value) + "</span></div>";
    };
    return summary_row("Status", health) +
        summary_row("Lobby", session) +
        summary_row("Connection", connection);
}

std::string OnlineApp::dashboard_rml() const {
    const net::Status status = transport_.status();
    const auto detail_row = [](std::string_view label, std::string_view value) {
        return "<div class=\"online-detail-row\"><span class=\"online-detail-label\">" +
            rml_escape(label) + "</span><span class=\"online-detail-value\">" +
            rml_escape(value) + "</span></div>";
    };

    std::string connection = "—";
    std::string session = "—";
    std::string health = "Disconnected";
    if (status.enabled) {
        connection = connection_label(status);
        const size_t playerCount = transport_.peers().size() + 1u;
        session = (status.room.empty() ? std::string("Unnamed lobby") : status.room) +
            " — " + std::to_string(playerCount) + (playerCount == 1 ? " player" : " players");
        if (status.reconnecting) {
            health = "Reconnecting";
        } else if (!status.error.empty()) {
            health = "Error — " + friendly_status_text(status.error);
        } else {
            switch (status.state) {
            case net::State::Connected:
                health = connected_health(status, playerCount - 1u);
                break;
            case net::State::Connecting: health = "Connecting"; break;
            case net::State::Listening: health = "Ready — waiting for players"; break;
            default: health = "Disconnected"; break;
            }
        }
    } else if (!status.error.empty()) {
        health = "Error — " + friendly_status_text(status.error);
    } else if (!statusMessage_.empty() && statusMessage_ != "Not connected") {
        health = friendly_status_text(statusMessage_);
    }

    std::string rml;
    rml += detail_row("Status", health);
    rml += detail_row("Lobby", session);
    rml += detail_row("Connection", connection);
    return rml;
}

void OnlineApp::open_window() {
    if (window_ != 0) {
        return;
    }
    refresh_manual_peer_choices();
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Session";
    tab.build = &OnlineApp::build_session_tab;
    tab.update = &OnlineApp::update_window;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &window_);
}

void OnlineApp::open_settings_window() {
    if (settingsWindow_ != 0) return;
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Session options";
    tab.build = &OnlineApp::build_settings_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::settings_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &settingsWindow_);
}

void OnlineApp::open_player_options_window() {
    if (playerOptionsWindow_ != 0) return;
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Cosmetic options";
    tab.build = &OnlineApp::build_player_options_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::player_options_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &playerOptionsWindow_);
}

void OnlineApp::open_speedrun_window() {
    if (speedrunWindow_ != 0 || !speedrunAvailable_ || !game::speedrun::mode_active()) return;
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Speedrun options";
    tab.build = &OnlineApp::build_speedrun_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::speedrun_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &speedrunWindow_);
}

void OnlineApp::open_voice_window() {
    if (voiceWindow_ != 0) return;
    voiceInputLabels_ = {"Default microphone"};
    if (voice_ != nullptr) {
        for (auto& name : voice_->input_devices()) voiceInputLabels_.push_back(std::move(name));
    }
    voiceInputOptions_.clear();
    for (const auto& label : voiceInputLabels_) voiceInputOptions_.push_back(label.c_str());
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Voice chat";
    tab.build = &OnlineApp::build_voice_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::voice_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &voiceWindow_);
}

void OnlineApp::open_sync_window() {
    if (syncWindow_ != 0) return;
    refresh_manual_peer_choices();
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = "Manual sync";
    tab.build = &OnlineApp::build_sync_tab;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::sync_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &syncWindow_);
}

void OnlineApp::open_lobby_window(ConnectionRole role) {
    if (lobbyWindow_ != 0) return;
    connectionRole_ = role;
    static UiTabDesc tab;
    tab = UI_TAB_DESC_INIT;
    tab.title = role == ConnectionRole::Host ? "Host lobby" : "Join lobby";
    tab.build = &OnlineApp::build_lobby_tab;
    tab.update = &OnlineApp::update_lobby_window;
    tab.user_data = this;
    UiWindowDesc desc = UI_WINDOW_DESC_INIT;
    desc.tabs = &tab;
    desc.tab_count = 1;
    desc.rcss = kOnlineWindowRcss;
    desc.on_closed = &OnlineApp::lobby_window_closed;
    desc.user_data = this;
    svc_ui->window_push(mod_ctx, &desc, &lobbyWindow_);
}

void OnlineApp::refresh_manual_peer_choices() {
    const std::string selectedId = selectedManualPeer_ >= 0 &&
        selectedManualPeer_ < static_cast<int64_t>(manualPeerIds_.size()) ?
            manualPeerIds_[selectedManualPeer_] : std::string();
    manualPeerIds_.clear();
    manualPeerLabels_.clear();
    manualPeerButtonContexts_.clear();
    for (const auto& [id, name] : transport_.peers()) {
        manualPeerIds_.push_back(id);
        manualPeerLabels_.push_back(name.empty() ? "Player" : name);
    }
    manualPeerButtonContexts_.reserve(manualPeerIds_.size());
    for (int64_t i = 0; i < static_cast<int64_t>(manualPeerIds_.size()); ++i) {
        manualPeerButtonContexts_.push_back({this, i});
    }
    selectedManualPeer_ = 0;
    if (!selectedId.empty()) {
        const auto selected = std::find(manualPeerIds_.begin(), manualPeerIds_.end(), selectedId);
        if (selected != manualPeerIds_.end()) {
            selectedManualPeer_ = std::distance(manualPeerIds_.begin(), selected);
        }
    }
}

void OnlineApp::refresh_inline_player_rows() {
    const auto& peers = transport_.peers();
    const net::Status status = transport_.status();
    const bool owner = status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner));

    const bool emptyVisible = peers.empty();
    if (windowPlayersEmpty_ != 0 && emptyVisible != windowPlayersEmptyVisible_) {
        if (svc_ui->elem_set_visible(mod_ctx, windowPlayersEmpty_, emptyVisible) == MOD_OK) {
            windowPlayersEmptyVisible_ = emptyVisible;
        } else {
            windowPlayersEmpty_ = 0;
        }
    }

    auto peer = peers.begin();
    for (auto& slot : inlineKickRows_) {
        const bool hasPeer = peer != peers.end();
        if (hasPeer) {
            const std::string peerName = peer->second.empty() ? "Player" : peer->second;
            if (slot.peerId != peer->first) {
                slot.peerId = peer->first;
                slot.kickPending = false;
            }
            slot.peerName = peerName;
            const auto latency = game_ != nullptr ?
                game_->peer_latency_ms(peer->first) : std::nullopt;
            const std::string ping = latency ?
                std::to_string(*latency) + " ms" : "--";
            const std::string identity =
                "<span class=\"online-player-indicator\"></span>"
                "<span class=\"online-player-name\">" + rml_escape(peerName) + "</span>"
                "<span class=\"online-player-ping\">" + ping + "</span>";
            if (slot.identity != 0 && identity != slot.renderedIdentity) {
                if (svc_ui->elem_set_rml(mod_ctx, slot.identity, identity.c_str()) != MOD_OK) {
                    slot.identity = 0;
                }
                slot.renderedIdentity = identity;
            }
            ++peer;
        } else {
            slot.peerId.clear();
            slot.peerName.clear();
            slot.renderedIdentity.clear();
            slot.kickPending = false;
        }

        if (slot.row != 0 && hasPeer != slot.rowVisible) {
            if (svc_ui->elem_set_visible(mod_ctx, slot.row, hasPeer) == MOD_OK) {
                slot.rowVisible = hasPeer;
            } else {
                slot.row = 0;
                slot.identity = 0;
                slot.button = 0;
            }
        }
        const bool buttonVisible = hasPeer && owner;
        if (slot.button != 0 && buttonVisible != slot.buttonVisible) {
            if (svc_ui->elem_set_visible(mod_ctx, slot.button, buttonVisible) == MOD_OK) {
                slot.buttonVisible = buttonVisible;
            } else {
                slot.button = 0;
            }
        }
    }
}

void OnlineApp::set_manual_sync_pending_visual(bool pending) {
    const UiElementHandle buttons[] = {manualSyncFlagsButton_, manualSyncWarpButton_};
    for (const UiElementHandle button : buttons) {
        if (button != 0 &&
            svc_ui->elem_set_class(mod_ctx, button, "online-sync-pending", pending) != MOD_OK) {
            if (button == manualSyncFlagsButton_) manualSyncFlagsButton_ = 0;
            if (button == manualSyncWarpButton_) manualSyncWarpButton_ = 0;
        }
    }
}

void OnlineApp::begin_lobby_attempt(std::string failurePrefix) {
    connectedLobbyName_.clear();
    requestedDisconnectStatus_.clear();
    pendingLobbyFailurePrefix_ = std::move(failurePrefix);
    pendingLobbyFailureNotified_ = false;
}

void OnlineApp::notify_lobby_attempt_failure(std::string_view detail) {
    if (pendingLobbyFailurePrefix_.empty() || pendingLobbyFailureNotified_) return;
    game::push_online_notification(
        failure_message(pendingLobbyFailurePrefix_, detail), 5.0f, true);
    pendingLobbyFailureNotified_ = true;
}

void OnlineApp::request_manual_sync(bool flagsOnly) {
    if (manualSyncCooldownTicks_ > 0) return;
    if (game_ == nullptr || selectedManualPeer_ < 0 ||
        selectedManualPeer_ >= static_cast<int64_t>(manualPeerIds_.size())) {
        statusMessage_ = "Manual sync failed: reopen Online and choose a connected peer";
        game::push_online_notification(
            "Sync request failed. Choose a connected player.", 5.0f, true);
        return;
    }
    std::string error;
    if (!game_->request_manual_sync(manualPeerIds_[selectedManualPeer_], flagsOnly, &error)) {
        statusMessage_ = "Manual sync failed: " +
            (error.empty() ? std::string("request could not be sent") : error);
        game::push_online_notification(
            failure_message("Sync request failed",
                            error.empty() ? "The request could not be sent." : error),
            5.0f, true);
    } else {
        manualSyncWasWaiting_ = true;
        manualSyncCooldownTicks_ = kManualSyncCooldownTicks;
        set_manual_sync_pending_visual(true);
        const std::string& peerName = manualPeerLabels_[selectedManualPeer_];
        game::push_online_notification(
            (flagsOnly ? "Flag sync to " : "Sync to ") + peerName + " requested.");
        statusMessage_ = flagsOnly ? "Requested flags sync" : "Requested sync and warp";
    }
}

void OnlineApp::host_direct() {
    const int64_t port = int_value(config_.port, 34197);
    if (port < 1 || port > 65535) {
        statusMessage_ = "Direct host failed: port must be 1-65535";
        game::push_online_notification(
            "Could not host the direct lobby. The port must be between 1 and 65535.",
            5.0f, true);
        return;
    }
    net::DirectHostConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.directRoom);
    config.bindHost = string_value(config_.bindHost);
    config.publicHost = string_value(config_.publicHost);
    config.port = static_cast<uint16_t>(port);
    config.settings = configured_settings();
    config.wantPuppet = config.settings.dummyModel;
    config.wantMidna = false;
    config.sessionId = dusk::multiplayer::make_session_token(9);
    config.sessionKey = dusk::multiplayer::make_session_token(16);
    dusk::multiplayer::InviteCodePayload invite;
    invite.transport = "direct";
    invite.host = config.publicHost;
    invite.port = config.port;
    invite.room = config.room;
    invite.sessionId = config.sessionId;
    invite.sessionKey = config.sessionKey;
    activeCode_ = dusk::multiplayer::create_invite_code(invite);
    std::string error;
    if (!transport_.start_direct_host(config, &error)) {
        statusMessage_ = "Direct host failed: " + error;
        game::push_online_notification(
            failure_message("Could not host the direct lobby", error), 5.0f, true);
        activeCode_.clear();
    } else {
        svc_config->set_string(mod_ctx, config_.directInvite, activeCode_.c_str());
        statusMessage_ = "Hosting direct lobby";
        begin_lobby_attempt({});
        connectedLobbyName_ = transport_.status().room;
        game::push_online_notification("Hosting lobby " + connectedLobbyName_ + ".");
    }
}

void OnlineApp::join_direct() {
    const std::string code = string_value(config_.directInvite);
    std::string error;
    const auto invite = dusk::multiplayer::decode_invite_code(code, &error);
    if (!invite || invite->transport != "direct") {
        statusMessage_ = "Direct join failed: " +
                         (error.empty() ? std::string("not a direct invite") : error);
        game::push_online_notification(
            failure_message("Could not join the direct lobby",
                            error.empty() ? "The invite code is not for a direct lobby." : error),
            5.0f, true);
        return;
    }
    net::DirectJoinConfig config;
    config.name = string_value(config_.playerName);
    config.room = invite->room;
    config.host = invite->host;
    config.port = static_cast<uint16_t>(invite->port);
    config.sessionId = invite->sessionId;
    config.sessionKey = invite->sessionKey;
    config.settings = configured_settings();
    config.wantPuppet = bool_value(config_.dummyModel, true);
    config.wantMidna = false;
    if (!transport_.start_direct_join(config, &error)) {
        statusMessage_ = "Direct join failed: " + error;
        game::push_online_notification(
            failure_message("Could not join the direct lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not join the direct lobby");
        activeCode_ = code;
        statusMessage_ = "Joining direct lobby";
    }
}

void OnlineApp::host_relay() {
    relayHostIntent_ = false;
    if (!bool_value(config_.relayManualHost)) {
        net::CloudRoomConfig cloud;
        cloud.name = string_value(config_.playerName);
        cloud.room = string_value(config_.relayRoom);
        cloud.lobbyCode = string_value(config_.lobbyCode);
        cloud.serverUrl = kCloudRoomServiceUrl;
        cloud.createRoom = true;
        cloud.settings = configured_settings();
        cloud.wantPuppet = cloud.settings.dummyModel;
        std::string error;
        if (!transport_.start_cloud_room(cloud, net::make_sdk_room_channel(), &error)) {
            statusMessage_ = "Cloud room host failed: " + error;
            game::push_online_notification(
                failure_message("Could not create the NAT lobby", error), 5.0f, true);
        } else {
            begin_lobby_attempt("Could not create the NAT lobby");
            relayHostIntent_ = true;
            activeCode_.clear();
            statusMessage_ = "Creating NAT lobby via Cloudflare room service";
        }
        return;
    }
    net::RelayConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.relayRoom);
    config.lobbyCode = string_value(config_.lobbyCode);
    config.createRoom = true;
    config.settings = configured_settings();
    config.wantPuppet = config.settings.dummyModel;
    const std::string code = string_value(config_.relayCode);
    std::string error;
    const auto endpoint = dusk::multiplayer::decode_invite_code(code, &error);
    if (!endpoint || endpoint->transport != "relay") {
        statusMessage_ = "NAT/Relay host failed: " +
                         (error.empty() ? std::string("not a relay code") : error);
        game::push_online_notification(
            failure_message("Could not create the NAT/Relay lobby",
                            error.empty() ? "The invite code is not for a relay." : error),
            5.0f, true);
        return;
    }
    const bool useLocalRelay = bool_value(config_.relayLocal);
    config.host = useLocalRelay ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "NAT/Relay host failed: " + error;
        game::push_online_notification(
            failure_message("Could not create the NAT/Relay lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not create the NAT/Relay lobby");
        relayHostIntent_ = true;
        activeCode_ = code;
        statusMessage_ = useLocalRelay
            ? "Creating NAT/Relay lobby using the relay on this PC"
            : "Creating NAT/Relay lobby using the server in the relay code";
    }
}

void OnlineApp::join_relay() {
    relayHostIntent_ = false;
    if (!bool_value(config_.relayManualHost)) {
        net::CloudRoomConfig cloud;
        cloud.name = string_value(config_.playerName);
        cloud.room = string_value(config_.relayRoom);
        cloud.lobbyCode = string_value(config_.lobbyCode);
        cloud.serverUrl = kCloudRoomServiceUrl;
        cloud.createRoom = false;
        cloud.settings = configured_settings();
        cloud.wantPuppet = bool_value(config_.dummyModel, true);
        std::string error;
        if (!transport_.start_cloud_room(cloud, net::make_sdk_room_channel(), &error)) {
            statusMessage_ = "Cloud room join failed: " + error;
            game::push_online_notification(
                failure_message("Could not join the NAT lobby", error), 5.0f, true);
        } else {
            begin_lobby_attempt("Could not join the NAT lobby");
            activeCode_.clear();
            statusMessage_ = "Joining NAT lobby via Cloudflare room service";
        }
        return;
    }
    const std::string code = string_value(config_.relayCode);
    std::string error;
    const auto endpoint = dusk::multiplayer::decode_invite_code(code, &error);
    if (!endpoint || endpoint->transport != "relay") {
        statusMessage_ = "NAT/Relay join failed: " +
                         (error.empty() ? std::string("not a relay code") : error);
        game::push_online_notification(
            failure_message("Could not join the NAT/Relay lobby",
                            error.empty() ? "The invite code is not for a relay." : error),
            5.0f, true);
        return;
    }
    net::RelayConfig config;
    config.name = string_value(config_.playerName);
    config.room = string_value(config_.relayRoom);
    config.lobbyCode = string_value(config_.lobbyCode);
    const bool useLocalRelay = bool_value(config_.relayLocal);
    config.host = useLocalRelay ? "127.0.0.1" : endpoint->host;
    config.port = static_cast<uint16_t>(endpoint->port);
    config.sessionId = endpoint->sessionId;
    config.sessionKey = endpoint->sessionKey;
    config.createRoom = false;
    config.settings = configured_settings();
    config.wantPuppet = bool_value(config_.dummyModel, true);
    if (!transport_.start_relay(config, &error)) {
        statusMessage_ = "NAT/Relay join failed: " + error;
        game::push_online_notification(
            failure_message("Could not join the NAT/Relay lobby", error), 5.0f, true);
    } else {
        begin_lobby_attempt("Could not join the NAT/Relay lobby");
        relayHostIntent_ = false;
        activeCode_ = code;
        statusMessage_ = useLocalRelay
            ? "Joining NAT/Relay lobby using the relay on this PC"
            : "Joining NAT/Relay lobby using the server in the relay code";
    }
}

void OnlineApp::disconnect() {
    if (requestedDisconnectStatus_.empty()) requestedDisconnectStatus_ = "Disconnected";
    transport_.disconnect();
    clear_speedrun_request();
    speedrunResetAt_ = {};
    game::appearance::set_lobby_active(false);
    if (router_ != nullptr) {
        router_->clear();
    }
    if (game_ != nullptr) {
        game_->reset_session();
    }
    statusMessage_ = requestedDisconnectStatus_;
    activeCode_.clear();
    relayHostIntent_ = false;
    livePublishInitialized_ = false;
}

void OnlineApp::publish_live_options() {
    const net::Status status = transport_.status();
    if (!status.welcomed) {
        livePublishInitialized_ = false;
        return;
    }

    // The active room settings are authoritative after a session starts.
    // Persistent preferences must never overwrite a host's values on welcome
    // or owner migration, and the puppet preference must follow the active
    // room model rather than a joiner's stale local preference.
    const bool wantsPuppet = status.settings.dummyModel;
    const bool wantsMidna = false;

    if (!livePublishInitialized_) {
        lastWantPuppet_ = !wantsPuppet; // force one preference publication
        lastWantMidna_ = !wantsMidna;
        livePublishInitialized_ = true;
    }

    if (wantsPuppet != lastWantPuppet_ || wantsMidna != lastWantMidna_) {
        if (transport_.publish_visual_preferences(wantsPuppet, wantsMidna)) {
            lastWantPuppet_ = wantsPuppet;
            lastWantMidna_ = wantsMidna;
        }
    }
}

ModResult OnlineApp::build_panel(ModContext*, UiElementHandle panel, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.panelStatus_ = 0;
    svc_ui->pane_add_section(mod_ctx, panel, "Status");
    const std::string status = app.status_text();
    svc_ui->pane_add_rml(mod_ctx, panel, status.c_str(), &app.panelStatus_);
    app.panelRenderedStatus_ = status;
    add_button(panel, "Open Multiplayer", &OnlineApp::open_pressed, &app);
    return MOD_OK;
}

ModResult OnlineApp::update_panel(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (app.panelStatus_ != 0) {
        const std::string status = app.status_text();
        if (status != app.panelRenderedStatus_ &&
            svc_ui->elem_set_rml(mod_ctx, app.panelStatus_, status.c_str()) != MOD_OK) {
            // Panel contents are rebuilt on host-tab changes; never poll a
            // generation-checked element after its owning content is gone.
            app.panelStatus_ = 0;
        }
        app.panelRenderedStatus_ = status;
    }
    return MOD_OK;
}

ModResult OnlineApp::build_session_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                       UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.windowStatus_ = 0;
    app.windowPlayersEmpty_ = 0;
    app.windowPlayersEmptyVisible_ = true;
    for (auto& slot : app.inlineKickRows_) slot = {};
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);

    svc_ui->pane_add_section(mod_ctx, left, "PLAY");
    add_button(left, "Create room", &OnlineApp::host_lobby_pressed, &app, nullptr, nullptr, nullptr, nullptr,
               "<p>Start a room. Pick a Room name and Room code, then give both to your partner.</p>");
    add_button(left, "Join room", &OnlineApp::join_lobby_pressed, &app, nullptr, nullptr, nullptr, nullptr,
               "<p>Join your partner's room with their Room name and Room code.</p>");
    add_button(left, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::host_inactive, nullptr, "online-danger-action");
    add_button(left, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::joiner_inactive, nullptr, "online-danger-action");

    svc_ui->pane_add_section(mod_ctx, left, "Co-op");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Shared HP", app.config_.sharedHp,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>On: enemies share one health pool between you and your partner. Bosses get double health with two players. The room host's setting is used for everyone.</p>");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Boss doors need both players", app.config_.bossDoorTogether,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>On: a boss door only opens when your partner is at it too. The room host's setting is used for everyone.</p>");

    svc_ui->pane_add_section(mod_ctx, left, "Session");
    add_button(left, "Session options", &OnlineApp::settings_pressed, &app);
    add_button(left, "Manual sync", &OnlineApp::sync_menu_pressed, &app,
               &OnlineApp::sync_menu_unavailable);
    svc_ui->pane_add_section(mod_ctx, left, "Misc");
    add_button(left, "Voice chat", &OnlineApp::voice_pressed, &app);
    add_button(left, "Cosmetic options", &OnlineApp::player_options_pressed, &app);
    add_button(left, "Speedrun options", &OnlineApp::speedrun_pressed, &app,
               &OnlineApp::speedrun_unavailable);

    svc_ui->elem_set_class(mod_ctx, right, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, right, "Status");
    const std::string status = app.dashboard_rml();
    svc_ui->pane_add_rml(mod_ctx, right, status.c_str(), &app.windowStatus_);
    app.windowRenderedStatus_ = status;
    svc_ui->pane_add_section(mod_ctx, right, "Connected players");
    svc_ui->pane_add_rml(
        mod_ctx, right,
        "<span class=\"online-player-empty-title\">No other players are connected.</span>"
        "<span class=\"online-player-empty-detail\">Players will appear here after joining.</span>",
        &app.windowPlayersEmpty_);
    if (app.windowPlayersEmpty_ != 0) {
        svc_ui->elem_set_class(mod_ctx, app.windowPlayersEmpty_, "online-player-empty", true);
    }
    for (auto& slot : app.inlineKickRows_) {
        slot.app = &app;
        UiRowDesc row = UI_ROW_DESC_INIT;
        row.align = UI_ROW_ALIGN_SPACE_BETWEEN;
        if (svc_ui->pane_add_row(mod_ctx, right, &row, &slot.row) != MOD_OK || slot.row == 0) {
            slot.row = 0;
            continue;
        }
        svc_ui->elem_set_class(mod_ctx, slot.row, "online-player-row", true);
        svc_ui->pane_add_rml(mod_ctx, slot.row, "", &slot.identity);
        if (slot.identity != 0) {
            svc_ui->elem_set_class(mod_ctx, slot.identity, "online-player-identity", true);
        }
        slot.button = add_button(
            slot.row, "KICK", &OnlineApp::inline_kick_pressed, &slot,
            &OnlineApp::inline_kick_unavailable, nullptr, "online-inline-kick", nullptr,
            "<p>Remove this player from the lobby.</p>");
    }
    app.refresh_inline_player_rows();
    return MOD_OK;
}

ModResult OnlineApp::build_player_options_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                              UiElementHandle, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Cosmetic options");
    app.match_player_colour();
    static constexpr const char* presets[] = {
        "508040", "3E8AC4", "BC5350", "9A72BD", "D99A45", "D27DA7", "E6DEC6", "555B65"
    };
    UiControlDesc colour = UI_CONTROL_DESC_INIT;
    colour.kind = UI_CONTROL_COLOR;
    colour.label = "Tunic colour";
    colour.binding = UI_BINDING_CONFIG_VAR;
    colour.config_var = app.config_.outfitColor;
    colour.color_presets = presets;
    colour.color_preset_count = std::size(presets);
    colour.help_rml = "Choose Link's tunic colour.";
    svc_ui->pane_add_control(mod_ctx, left, &colour, nullptr);
    colour.label = "Player colour";
    colour.config_var = app.config_.playerColor;
    colour.is_disabled = &OnlineApp::player_colour_locked;
    colour.user_data = &app;
    colour.help_rml = "Choose your nametag colour and the minimap colour other players see.";
    svc_ui->pane_add_control(mod_ctx, left, &colour, nullptr);
    UiControlDesc match = UI_CONTROL_DESC_INIT;
    match.kind = UI_CONTROL_TOGGLE;
    match.label = "Match tunic colour";
    match.binding = UI_BINDING_CONFIG_VAR;
    match.config_var = app.config_.matchOutfitColor;
    match.help_rml = "Use your tunic colour for your player colour.";
    svc_ui->pane_add_control(mod_ctx, left, &match, nullptr);
    colour.label = "Zora Armour colour";
    colour.config_var = app.config_.zoraColor;
    colour.is_disabled = nullptr;
    colour.user_data = nullptr;
    colour.help_rml = "Choose Link's Zora Armour colour separately from the tunic.";
    svc_ui->pane_add_control(mod_ctx, left, &colour, nullptr);
    add_button(left, "Reset to defaults", &OnlineApp::reset_player_options, &app);
    return MOD_OK;
}

ModResult OnlineApp::build_speedrun_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                       UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Speedrun options");
    add_button(left, "Start Speedrun", &OnlineApp::start_speedrun_pressed, &app,
               &OnlineApp::start_speedrun_unavailable);
    add_button(left, "Reset Speedrun", &OnlineApp::reset_speedrun_pressed, &app,
               &OnlineApp::speedrun_action_unavailable);
    add_bound_control(left, UI_CONTROL_TOGGLE, "Hide Active Mods",
                      app.config_.hideActiveMods);
    svc_ui->elem_set_class(mod_ctx, right, "online-session-pane", true);
    svc_ui->pane_add_rml(mod_ctx, right,
        "<p>Start requires all three Speedrun save slots to be empty for every "
        "player. Reset ends the timer and soft resets everyone.</p>",
        nullptr);
    return MOD_OK;
}

ModResult OnlineApp::build_voice_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                    UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Voice chat");
#if defined(DUSKLIGHT_ONLINE_VOICE)
    add_bound_control(left, UI_CONTROL_TOGGLE, "Voice chat", app.config_.voiceEnabled);
    add_session_toggle(left, "Proximity mode", &OnlineApp::voice_proximity_get,
                       &OnlineApp::voice_proximity_set,
                       &OnlineApp::voice_proximity_setting_locked, &app,
                       "<p>The lobby host chooses whether voices follow Links and fade with distance. Voice chat remains optional for each player.</p>");
    UiControlDesc range = UI_CONTROL_DESC_INIT;
    range.kind = UI_CONTROL_NUMBER;
    range.label = "Proximity range";
    range.get = &OnlineApp::voice_proximity_range_get;
    range.set = &OnlineApp::voice_proximity_range_set;
    range.min = 0;
    range.max = 200;
    range.step = 5;
    range.suffix = "%";
    range.is_disabled = &OnlineApp::voice_proximity_range_locked;
    range.is_modified = &never_modified;
    range.user_data = &app;
    range.help_rml = "<p>The lobby host sets how far away players can hear each other.</p>";
    svc_ui->pane_add_control(mod_ctx, left, &range, nullptr);
    add_button(left, "Set mute hotkey", &OnlineApp::mute_hotkey_pressed, &app,
               nullptr, nullptr, nullptr, nullptr,
               "<p>Choose a key that toggles your microphone while playing.</p>");
    const std::string hotkeyText = "Mute hotkey: " + game::voice_mute_hotkey_name(
        static_cast<int>(app.int_value(app.config_.voiceMuteHotkey, SDL_SCANCODE_M)));
    svc_ui->pane_add_text(mod_ctx, left, hotkeyText.c_str(), &app.voiceMuteHotkeyText_);
    app.renderedVoiceMuteHotkeyText_ = hotkeyText;
    UiControlDesc input = UI_CONTROL_DESC_INIT;
    input.kind = UI_CONTROL_DROPDOWN;
    input.label = "Microphone input";
    input.options = app.voiceInputOptions_.data();
    input.option_count = app.voiceInputOptions_.size();
    input.get = &OnlineApp::voice_input_get;
    input.set = &OnlineApp::voice_input_set;
    input.user_data = &app;
    svc_ui->pane_add_control(mod_ctx, left, &input, nullptr);
    UiControlDesc volume = UI_CONTROL_DESC_INIT;
    volume.kind = UI_CONTROL_NUMBER;
    volume.binding = UI_BINDING_CONFIG_VAR;
    volume.min = 0;
    volume.max = 200;
    volume.step = 5;
    volume.suffix = "%";
    volume.label = "Your mic volume";
    volume.config_var = app.config_.micVolume;
    svc_ui->pane_add_control(mod_ctx, left, &volume, nullptr);
    volume.label = "Other player volume";
    volume.config_var = app.config_.playerVolume;
    svc_ui->pane_add_control(mod_ctx, left, &volume, nullptr);
#else
    svc_ui->pane_add_text(mod_ctx, left, "Voice chat is unavailable on this platform.", nullptr);
#endif
    svc_ui->pane_add_section(mod_ctx, right, "Voice chat");
    svc_ui->pane_add_text(mod_ctx, right,
                          "Turn off Proximity mode to hear everyone at the set volume, centered.",
                          nullptr);
    return MOD_OK;
}

ModResult OnlineApp::build_settings_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                        UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Gameplay");
    add_session_toggle(
        left, "Remote Link model", &OnlineApp::dummy_model_get, &OnlineApp::dummy_model_set,
        &OnlineApp::room_setting_locked, &app,
        "<p>Show connected players as fully animated Link models.</p>");
    add_session_toggle(
        left, "Sync flags", &OnlineApp::sync_flags_get, &OnlineApp::sync_flags_set,
        &OnlineApp::room_setting_locked, &app,
        "<p>Share supported story, item and progression state with the lobby.</p>");
    add_session_toggle(
        left, "Savewarp protection", &OnlineApp::save_recovery_get,
        &OnlineApp::save_recovery_set, &OnlineApp::room_setting_locked, &app,
        "<p>For 60 seconds after saving, keep progress received from other players if you reload the same file without closing the game.</p>");
    add_session_toggle(
        left, "Remote collision", &OnlineApp::remote_collision_get,
        &OnlineApp::remote_collision_set, &OnlineApp::remote_collision_setting_locked, &app,
        "<p>Allow connected players' Link models to collide with the local player.</p>");
    add_session_toggle(
        left, "PvP", &OnlineApp::pvp_get, &OnlineApp::pvp_set,
        &OnlineApp::pvp_setting_locked, &app,
        "<p>Allow connected players to damage each other. Remote collision must also be enabled.</p>");
    svc_ui->pane_add_section(mod_ctx, left, "Co-op");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Shared HP", app.config_.sharedHp,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>On: enemies share one health pool between you and your partner, so you can both wear the same enemy down. Bosses get double health when two players are in the stage. The room host's setting is used for everyone.</p>");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Boss doors need both players", app.config_.bossDoorTogether,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>On: while you are in a room, a boss door only opens when your partner is standing at it too, so bosses are always fought together. Hidden Skills and everything else work as normal. The room host's setting is used for everyone.</p>");
    svc_ui->pane_add_section(mod_ctx, left, "Display");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Sync prompts", app.config_.syncPrompts,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>Show prompts to sync and reload when other players reach story milestones.</p>");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Name labels", app.config_.nameLabels,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>Show each connected player's name above their Link model.</p>");
    add_bound_control(
        left, UI_CONTROL_TOGGLE, "Player list overlay", app.config_.playerList,
        0, 0, 1, 0, nullptr, nullptr, nullptr, nullptr,
        "<p>Keep the connected-player list visible during gameplay.</p>");
    svc_ui->pane_add_section(mod_ctx, right, "Session options");
    svc_ui->pane_add_text(mod_ctx, right,
                          "Select an option to see its description.", nullptr);
    return MOD_OK;
}

ModResult OnlineApp::build_sync_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                    UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    svc_ui->elem_set_class(mod_ctx, left, "online-session-pane", true);
    svc_ui->elem_set_class(mod_ctx, right, "online-session-pane", true);
    svc_ui->pane_add_section(mod_ctx, left, "Sync options");
    app.manualSyncFlagsButton_ = add_button(
        left, "Sync flags", &OnlineApp::manual_sync_flags_pressed, &app,
        &OnlineApp::manual_sync_unavailable);
    app.manualSyncWarpButton_ = add_button(
        left, "Sync and warp", &OnlineApp::manual_sync_warp_pressed, &app,
        &OnlineApp::manual_sync_unavailable);
    add_button(left, "Refresh player list", &OnlineApp::refresh_sync_peers_pressed, &app);
    app.set_manual_sync_pending_visual(app.manualSyncCooldownTicks_ > 0);

    svc_ui->pane_add_section(mod_ctx, right, "Players");
    if (app.manualPeerButtonContexts_.empty()) {
        svc_ui->pane_add_text(mod_ctx, right, "No other players are connected.", nullptr);
    } else {
        for (size_t i = 0; i < app.manualPeerButtonContexts_.size(); ++i) {
            const UiElementHandle button = add_button(
                right, app.manualPeerLabels_[i].c_str(), &OnlineApp::manual_peer_pressed,
                &app.manualPeerButtonContexts_[i]);
            app.manualPeerButtonElements_.push_back(button);
            if (button != 0 && app.selectedManualPeer_ == static_cast<int64_t>(i)) {
                svc_ui->elem_set_class(mod_ctx, button, "online-peer-selected", true);
            }
        }
    }
    return MOD_OK;
}

ModResult OnlineApp::build_lobby_tab(ModContext*, UiWindowHandle, UiElementHandle left,
                                     UiElementHandle right, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, left, "online-lobby-choice-pane", true);
    svc_ui->pane_add_section(mod_ctx, left,
                             app.connectionRole_ == ConnectionRole::Host ? "Host lobby" :
                                                                         "Join lobby");
    UiGroupDesc direct = UI_GROUP_DESC_INIT;
    direct.label = "Direct";
    direct.build = app.connectionRole_ == ConnectionRole::Host ?
                       &OnlineApp::build_host_direct_settings :
                       &OnlineApp::build_join_direct_settings;
    direct.user_data = &app;
    svc_ui->pane_add_group(mod_ctx, left, right, &direct, nullptr);

    UiGroupDesc relay = UI_GROUP_DESC_INIT;
    relay.label = "NAT/Relay";
    relay.build = app.connectionRole_ == ConnectionRole::Host ?
                      &OnlineApp::build_host_relay_settings :
                      &OnlineApp::build_join_relay_settings;
    relay.user_data = &app;
    svc_ui->pane_add_group(mod_ctx, left, right, &relay, nullptr);

    svc_ui->pane_add_section(mod_ctx, right, "Connection settings");
    svc_ui->pane_add_text(mod_ctx, right, "Select Direct or NAT/Relay to configure it.", nullptr);
    return MOD_OK;
}

ModResult OnlineApp::build_host_direct_settings(ModContext*, UiElementHandle pane, void* data,
                                                ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, pane, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, pane, "Direct settings");
    add_button(pane, "Host direct lobby", &OnlineApp::host_direct_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action", nullptr,
               "<p>Start hosting with the direct-connection settings below.</p>");
    add_button(pane, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::direct_host_inactive, nullptr, "online-danger-action", nullptr,
               "<p>Close the current direct lobby and disconnect every guest.</p>");
    add_form_string(pane, "Player name", app.config_.playerName, 32, nullptr,
                    "<p>The name other players will see for you.</p>");
    add_form_string(pane, "Lobby name", app.config_.directRoom, 64, "online-half-field",
                    "<p>The name included in the direct invite code.</p>");
    add_form_string(pane, "Host address", app.config_.publicHost, 255, "online-half-field",
                    "<p>The public IP address or hostname guests should connect to.</p>");
    add_bound_control(pane, UI_CONTROL_NUMBER, "Port", app.config_.port, 1, 65535, 1, 0,
                      nullptr, nullptr, "online-form-field", "online-half-field",
                      "<p>The UDP port used for the direct connection.</p>");
    add_code_control(pane, "Invite code", &OnlineApp::direct_code_get,
                     &OnlineApp::direct_code_set, &app,
                     "<p>Share this code with players who want to join directly.</p>");
    add_button(pane, "Copy", &OnlineApp::copy_direct_code_pressed, &app, nullptr, nullptr,
               "online-copy-action", nullptr,
               "<p>Copy the direct invite code to the clipboard.</p>");
    return MOD_OK;
}

ModResult OnlineApp::build_host_relay_settings(ModContext*, UiElementHandle pane, void* data,
                                               ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.manualHostControls_ = {};
    svc_ui->elem_set_class(mod_ctx, pane, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, pane, "NAT/Relay settings");
    add_button(pane, "Create room", &OnlineApp::host_relay_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action", nullptr,
               "<p>Use Cloudflare for room discovery and direct NAT traversal. Manual host uses your own relay.</p>");
    add_button(pane, "Stop hosting", &OnlineApp::stop_hosting_pressed, &app,
               &OnlineApp::relay_host_inactive, nullptr, "online-danger-action", nullptr,
               "<p>Close the current NAT/Relay lobby and disconnect every guest.</p>");
    add_form_string(pane, "Player name", app.config_.playerName, 32, nullptr,
                    "<p>The name other players will see for you.</p>");
    add_form_string(pane, "Room name", app.config_.relayRoom, 64, "online-half-field",
                    "<p>Pick a Room name. Your buddy types this exact Room name to find your game.</p>");
    add_form_string(pane, "Room code", app.config_.lobbyCode, 128, "online-half-field",
                    "<p>Required: 6-128 characters. Share it with your buddy; do not reuse an account password.</p>");
    add_bound_control(pane, UI_CONTROL_TOGGLE, "Manual host",
                      app.config_.relayManualHost, 0, 0, 1, 0, nullptr, nullptr,
                      "online-wide-control", nullptr,
                      "<p>Off: Cloudflare room discovery, direct connections only. On: your own relay with a TP1 code.</p>");
    app.manualHostControls_[0] = add_bound_control(
        pane, UI_CONTROL_TOGGLE, "Use relay on this PC", app.config_.relayLocal,
        0, 0, 1, 0, nullptr, nullptr, "online-wide-control", nullptr,
        "<p>Run the manual relay locally instead of using the server in the TP1 code.</p>");
    app.manualHostControls_[1] = add_code_control(
        pane, "NAT/Relay code", &OnlineApp::relay_code_get,
        &OnlineApp::relay_code_set, &app,
        "<p>The TP1 code for the manually hosted relay.</p>");
    app.manualHostControls_[2] = add_button(
        pane, "Copy", &OnlineApp::copy_relay_code_pressed, &app, nullptr, nullptr,
        "online-copy-action", nullptr,
        "<p>Copy the manual relay code to the clipboard.</p>");
    const bool manual = app.bool_value(app.config_.relayManualHost);
    for (auto handle : app.manualHostControls_)
        if (handle) svc_ui->elem_set_class(mod_ctx, handle, "online-manual-only-hidden", !manual);
    return MOD_OK;
}

ModResult OnlineApp::build_join_direct_settings(ModContext*, UiElementHandle pane, void* data,
                                                ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_ui->elem_set_class(mod_ctx, pane, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, pane, "Direct settings");
    add_button(pane, "Join direct lobby", &OnlineApp::join_direct_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action", nullptr,
               "<p>Join the direct lobby described by the invite code below.</p>");
    add_button(pane, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::direct_join_inactive, nullptr, "online-danger-action", nullptr,
               "<p>Leave the current direct lobby.</p>");
    add_form_string(pane, "Player name", app.config_.playerName, 32, nullptr,
                    "<p>The name other players will see for you.</p>");
    add_code_control(pane, "Invite code", &OnlineApp::direct_code_get,
                     &OnlineApp::direct_code_set, &app,
                     "<p>The direct invite code shared by the host.</p>");
    add_button(pane, "Paste", &OnlineApp::paste_direct_code_pressed, &app, nullptr, nullptr,
               "online-copy-action", nullptr,
               "<p>Paste a direct invite code from the clipboard.</p>");
    return MOD_OK;
}

ModResult OnlineApp::build_join_relay_settings(ModContext*, UiElementHandle pane, void* data,
                                               ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.manualHostControls_ = {};
    svc_ui->elem_set_class(mod_ctx, pane, "online-form-pane", true);
    svc_ui->pane_add_section(mod_ctx, pane, "NAT/Relay settings");
    add_button(pane, "Join room", &OnlineApp::join_relay_pressed, &app,
               &OnlineApp::session_active, nullptr, "online-primary-action", nullptr,
               "<p>Use Cloudflare for room discovery and direct NAT traversal. Manual host uses the host's relay.</p>");
    add_button(pane, "Disconnect", &OnlineApp::disconnect_pressed, &app,
               &OnlineApp::relay_join_inactive, nullptr, "online-danger-action", nullptr,
               "<p>Leave the current NAT/Relay lobby.</p>");
    add_form_string(pane, "Player name", app.config_.playerName, 32, nullptr,
                    "<p>The name other players will see for you.</p>");
    add_form_string(pane, "Room name", app.config_.relayRoom, 64, "online-half-field",
                    "<p>The exact Room name your buddy created.</p>");
    add_form_string(pane, "Room code", app.config_.lobbyCode, 128, "online-half-field",
                    "<p>The 6-128 character room code your buddy set.</p>");
    add_bound_control(pane, UI_CONTROL_TOGGLE, "Manual host",
                      app.config_.relayManualHost, 0, 0, 1, 0, nullptr, nullptr,
                      "online-wide-control", nullptr,
                      "<p>Off: Cloudflare room discovery, direct connections only. On: connect through the host's own relay.</p>");
    app.manualHostControls_[0] = add_bound_control(
        pane, UI_CONTROL_TOGGLE, "Use relay on this PC", app.config_.relayLocal,
        0, 0, 1, 0, nullptr, nullptr, "online-wide-control", nullptr,
        "<p>Connect through a manually hosted relay service running on this PC.</p>");
    app.manualHostControls_[1] = add_code_control(
        pane, "NAT/Relay code", &OnlineApp::relay_code_get,
        &OnlineApp::relay_code_set, &app,
        "<p>The TP1 code for the host's manually hosted relay.</p>");
    app.manualHostControls_[2] = add_button(
        pane, "Paste", &OnlineApp::paste_relay_code_pressed, &app, nullptr, nullptr,
        "online-copy-action", nullptr,
        "<p>Paste the manual relay code from the clipboard.</p>");
    const bool manual = app.bool_value(app.config_.relayManualHost);
    for (auto handle : app.manualHostControls_)
        if (handle) svc_ui->elem_set_class(mod_ctx, handle, "online-manual-only-hidden", !manual);
    return MOD_OK;
}

void OnlineApp::player_options_window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.playerOptionsWindow_ = 0;
}

void OnlineApp::speedrun_window_closed(ModContext*, UiWindowHandle, void* data) {
    static_cast<OnlineApp*>(data)->speedrunWindow_ = 0;
}

void OnlineApp::voice_window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.voiceWindow_ = 0;
    app.voiceMuteHotkeyText_ = 0;
    app.renderedVoiceMuteHotkeyText_.clear();
    game::cancel_voice_mute_hotkey_capture();
}

void OnlineApp::voice_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_voice_window();
}

void OnlineApp::mute_hotkey_pressed(ModContext*, void*) {
    game::begin_voice_mute_hotkey_capture();
}

void OnlineApp::voice_input_get(ModContext*, void* data, UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const auto selected = app.string_value(app.config_.voiceInput);
    value->int_value = 0;
    for (size_t index = 1; index < app.voiceInputLabels_.size(); ++index) {
        if (app.voiceInputLabels_[index] == selected) value->int_value = index;
    }
}

void OnlineApp::voice_input_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (value->int_value < 0 ||
        static_cast<size_t>(value->int_value) >= app.voiceInputLabels_.size()) return;
    const char* name = value->int_value == 0 ? "" :
        app.voiceInputLabels_[static_cast<size_t>(value->int_value)].c_str();
    svc_config->set_string(mod_ctx, app.config_.voiceInput, name);
}

void OnlineApp::player_options_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_player_options_window();
}

void OnlineApp::speedrun_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_speedrun_window();
}

bool OnlineApp::speedrun_unavailable(ModContext*, void* data) {
    const auto& app = *static_cast<OnlineApp*>(data);
    return !app.speedrunAvailable_ || !game::speedrun::mode_active();
}

bool OnlineApp::speedrun_action_unavailable(ModContext*, void* data) {
    const auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool owner = status.mode == net::Mode::DirectHost ||
        (net::is_room_mode(status.mode) && status.isOwner);
    return speedrun_unavailable(nullptr, data) || !status.enabled || !owner ||
           (status.mode != net::Mode::DirectHost && !status.welcomed);
}

bool OnlineApp::start_speedrun_unavailable(ModContext*, void* data) {
    const auto& app = *static_cast<OnlineApp*>(data);
    return speedrun_action_unavailable(nullptr, data) ||
           !game::speedrun::can_start_here() || app.speedrunRequestId_ != 0;
}

void OnlineApp::clear_speedrun_request() {
    if (game_ != nullptr) game_->suppress_progression_prompts(false);
    speedrunProbe_.clear();
    speedrunWaitingPeers_.clear();
    speedrunParticipants_.clear();
    speedrunReadyPeers_.clear();
    speedrunPhase_ = SpeedrunPhase::None;
    speedrunRequestId_ = 0;
    speedrunPeerRequestId_ = 0;
    speedrunLocalReady_ = false;
    speedrunPeerReady_ = false;
    speedrunLocalPlayerReady_ = false;
    speedrunDownHeld_ = false;
    speedrunReadyCount_ = 0;
    speedrunReadyTotal_ = 0;
    speedrunChecksSent_ = false;
    speedrunDeadline_ = {};
    speedrunHoldStart_ = {};
    speedrunCountdownAt_ = {};
    game::set_speedrun_prompt({});
}

void OnlineApp::start_speedrun_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (start_speedrun_unavailable(nullptr, data)) return;
    for (const auto& [peerId, _] : app.transport_.peers()) {
        if (!app.transport_.reliable_peer_ready(peerId)) {
            game::push_online_notification("Wait for every player's reliable connection.",
                                           4.0f, true);
            return;
        }
    }
    app.clear_speedrun_request();
    if (app.speedrunSequence_ == 0)
        app.speedrunSequence_ = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    app.speedrunRequestId_ = ++app.speedrunSequence_;
    app.speedrunPhase_ = SpeedrunPhase::CheckingSaves;
    if (app.game_ != nullptr) app.game_->suppress_progression_prompts(true);
    app.speedrunDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (const auto& [peerId, _] : app.transport_.peers())
        app.speedrunParticipants_.insert(peerId);
    app.speedrunWaitingPeers_ = app.speedrunParticipants_;
    app.speedrunProbe_.begin();
}

void OnlineApp::reset_speedrun_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    if (speedrun_action_unavailable(nullptr, data)) return;
    for (const auto& [peerId, _] : app.transport_.peers()) {
        if (!app.transport_.reliable_peer_ready(peerId)) {
            game::push_online_notification("Wait for every player's reliable connection.",
                                           4.0f, true);
            return;
        }
    }
    if (!app.transport_.send({{"type", "speedrun_reset"}})) {
        game::push_online_notification("Could not send Speedrun reset.", 4.0f, true);
        return;
    }
    app.clear_speedrun_request();
    // Allow the reliable transport to transmit before the host resets its scene.
    app.speedrunResetAt_ = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(500);
}

void OnlineApp::update_speedrun_mods_visibility() {
    const bool hide = speedrunAvailable_ && game::speedrun::mode_active() &&
                      bool_value(config_.hideActiveMods);
    if (!hide) {
        if (hideActiveModsStyle_ != 0) {
            svc_ui->unregister_styles(mod_ctx, hideActiveModsStyle_);
            hideActiveModsStyle_ = 0;
        }
        hideActiveModsStyleUnavailable_ = false;
        return;
    }
    if (hideActiveModsStyle_ != 0 || hideActiveModsStyleUnavailable_) return;

    // The host draws this element in its passive overlay. A scoped sheet hides
    // only the mod list while leaving the speedrun timer and other overlays on.
    constexpr const char* style = "#mods-display[open] { display: none; }";
    if (svc_ui->register_styles(mod_ctx, UI_SCOPE_OVERLAY, style,
                                &hideActiveModsStyle_) != MOD_OK) {
        hideActiveModsStyleUnavailable_ = true;
        log_info("Could not hide Speedrun active mods overlay");
    }
}

void OnlineApp::cancel_speedrun_start(std::string_view reason) {
    if (speedrunRequestId_ != 0)
        (void)transport_.send({{"type", "speedrun_cancel"},
                               {"request_id", speedrunRequestId_}});
    clear_speedrun_request();
    game::push_online_notification(std::string(reason), 4.0f, true);
}

void OnlineApp::begin_speedrun_ready() {
    if (speedrunRequestId_ == 0 || speedrunPhase_ != SpeedrunPhase::CheckingSaves ||
        !speedrunLocalReady_ || !speedrunWaitingPeers_.empty()) return;
    speedrunReadyTotal_ = static_cast<uint32_t>(speedrunParticipants_.size() + 1);
    speedrunReadyCount_ = 0;
    if (!transport_.send({{"type", "speedrun_prompt"},
                          {"request_id", speedrunRequestId_},
                          {"player_count", speedrunReadyTotal_}})) {
        cancel_speedrun_start("Could not open Speedrun ready prompt.");
        return;
    }
    speedrunPhase_ = SpeedrunPhase::WaitingReady;
    if (!game::speedrun::close_menus_after_start())
        log_info("Speedrun ready prompt could not close Dusklight menus");
}

void OnlineApp::begin_speedrun_countdown() {
    if (speedrunRequestId_ == 0 || speedrunPhase_ != SpeedrunPhase::WaitingReady ||
        speedrunReadyCount_ != speedrunReadyTotal_) return;
    if (!transport_.send({{"type", "speedrun_countdown"},
                          {"request_id", speedrunRequestId_}})) {
        cancel_speedrun_start("Could not start Speedrun countdown.");
        return;
    }
    speedrunPhase_ = SpeedrunPhase::Countdown;
    speedrunCountdownAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
}

void OnlineApp::update_speedrun_prompt() {
    game::SpeedrunPromptView view;
    view.active = speedrunPhase_ == SpeedrunPhase::WaitingReady ||
                  speedrunPhase_ == SpeedrunPhase::Countdown;
    if (view.active) {
        view.countdown = speedrunPhase_ == SpeedrunPhase::Countdown;
        view.localReady = speedrunLocalPlayerReady_;
        view.readyCount = speedrunReadyCount_;
        view.playerCount = speedrunReadyTotal_;
        const auto now = std::chrono::steady_clock::now();
        if (view.countdown) {
            view.countdownSeconds = (std::max)(0.0f,
                std::chrono::duration<float>(speedrunCountdownAt_ - now).count());
        } else if (speedrunHoldStart_ != std::chrono::steady_clock::time_point{}) {
            view.holdRatio = std::clamp(
                std::chrono::duration<float>(now - speedrunHoldStart_).count(), 0.0f, 1.0f);
        }
    }
    game::set_speedrun_prompt(view);
}

void OnlineApp::tick_speedrun() {
    if (speedrunResetAt_ != std::chrono::steady_clock::time_point{}) {
        if (std::chrono::steady_clock::now() >= speedrunResetAt_) {
            speedrunResetAt_ = {};
            if (!game::speedrun::reset_run())
                game::push_online_notification("Could not reset Speedrun locally.", 4.0f, true);
        }
        return;
    }
    if (speedrunPhase_ == SpeedrunPhase::None) return;
    const auto now = std::chrono::steady_clock::now();
    if (!game::speedrun::mode_active()) {
        if (speedrunRequestId_ != 0)
            cancel_speedrun_start("Speedrun start cancelled: mode changed.");
        else clear_speedrun_request();
        return;
    }
    if (speedrunPhase_ == SpeedrunPhase::Countdown) {
        if (speedrunRequestId_ != 0 && now >= speedrunCountdownAt_)
            commit_speedrun_start();
        else if (speedrunPeerRequestId_ != 0 && now >= speedrunCountdownAt_ +
                                                 std::chrono::seconds(10)) {
            clear_speedrun_request();
            game::push_online_notification("Speedrun start timed out.", 4.0f, true);
        }
        return;
    }
    if (speedrunPhase_ == SpeedrunPhase::WaitingReady) {
        if (speedrunLocalPlayerReady_) return;
        if (!speedrunDownHeld_) {
            speedrunHoldStart_ = {};
            return;
        }
        if (speedrunHoldStart_ == std::chrono::steady_clock::time_point{})
            speedrunHoldStart_ = now;
        if (now - speedrunHoldStart_ < std::chrono::seconds(1)) return;
        speedrunLocalPlayerReady_ = true;
        speedrunHoldStart_ = {};
        if (speedrunRequestId_ != 0) {
            speedrunReadyCount_ = static_cast<uint32_t>(speedrunReadyPeers_.size() + 1);
            if (!transport_.send({{"type", "speedrun_ready_count"},
                                  {"request_id", speedrunRequestId_},
                                  {"ready_count", speedrunReadyCount_},
                                  {"player_count", speedrunReadyTotal_}})) {
                cancel_speedrun_start("Could not update Speedrun ready count.");
                return;
            }
            begin_speedrun_countdown();
        } else if (!transport_.send_to(
                transport_.status().mode == net::Mode::DirectJoin ? "direct" :
                    transport_.status().ownerClientId,
                {{"type", "speedrun_player_ready"},
                 {"request_id", speedrunPeerRequestId_}})) {
            clear_speedrun_request();
            game::push_online_notification("Could not send Speedrun ready status.", 4.0f, true);
        }
        return;
    }
    if (now > speedrunDeadline_) {
        if (speedrunRequestId_ != 0)
            cancel_speedrun_start("Speedrun save check timed out.");
        else {
            clear_speedrun_request();
            game::push_online_notification("Speedrun save check timed out.", 4.0f, true);
        }
        return;
    }
    if (speedrunRequestId_ != 0) {
        if (!speedrunLocalReady_) {
            const auto result = speedrunProbe_.tick();
            if (result == game::speedrun::SaveProbe::Result::Pending) return;
            if (result != game::speedrun::SaveProbe::Result::Clear) {
                const std::string reason = speedrunProbe_.error();
                cancel_speedrun_start(reason.empty() ?
                    "Speedrun saves are not clear." : reason);
                return;
            }
            speedrunLocalReady_ = true;
        }
        if (!speedrunChecksSent_) {
            for (const auto& peerId : speedrunParticipants_) {
                if (!transport_.reliable_peer_ready(peerId) ||
                    !transport_.send_to(peerId, {{"type", "speedrun_check"},
                                                 {"request_id", speedrunRequestId_}})) {
                    cancel_speedrun_start("Could not check every player's Speedrun saves.");
                    return;
                }
            }
            speedrunChecksSent_ = true;
        }
        if (speedrunWaitingPeers_.empty()) begin_speedrun_ready();
        return;
    }
    if (speedrunPeerRequestId_ != 0 && !speedrunPeerReady_) {
        const auto result = speedrunProbe_.tick();
        if (result == game::speedrun::SaveProbe::Result::Pending) return;
        speedrunPeerReady_ = result == game::speedrun::SaveProbe::Result::Clear;
        const std::string reason = speedrunProbe_.error();
        if (!transport_.send_to(transport_.status().mode == net::Mode::DirectJoin ?
                "direct" : transport_.status().ownerClientId,
                {{"type", "speedrun_ready"}, {"request_id", speedrunPeerRequestId_},
                 {"ready", speedrunPeerReady_}, {"reason", reason}})) {
            clear_speedrun_request();
            game::push_online_notification("Could not answer Speedrun save check.", 4.0f, true);
            return;
        }
        if (!speedrunPeerReady_) {
            clear_speedrun_request();
            game::push_online_notification(reason.empty() ?
                "Speedrun saves are not clear." : reason, 5.0f, true);
        }
    }
}

void OnlineApp::commit_speedrun_start() {
    if (speedrunPhase_ != SpeedrunPhase::Countdown ||
        !speedrunLocalReady_ || !speedrunLocalPlayerReady_ ||
        speedrunReadyCount_ != speedrunReadyTotal_ || !speedrunWaitingPeers_.empty() ||
        !game::speedrun::mode_active() || !game::speedrun::can_start_here() ||
        speedrunParticipants_.size() != transport_.peers().size()) {
        cancel_speedrun_start("Speedrun start cancelled: lobby or mode changed.");
        return;
    }
    const auto id = speedrunRequestId_;
    if (!transport_.send({{"type", "speedrun_start"}, {"request_id", id}})) {
        cancel_speedrun_start("Could not send Speedrun start.");
        return;
    }
    clear_speedrun_request();
    if (router_ != nullptr) router_->clear();
    if (!game::speedrun::start_run())
        game::push_online_notification("Could not start Speedrun locally.", 4.0f, true);
    else {
        if (!game::speedrun::close_menus_after_start())
            log_info("Speedrun started, but Dusklight menu close is unavailable");
        game::push_online_notification("Starting Speedrun", 1.5f);
    }
}

void OnlineApp::handle_speedrun_message(const net::Event& event) {
    if (!speedrunAvailable_ || event.kind != net::EventKind::Message) return;
    const net::Status status = transport_.status();
    if (!status.enabled) return;
    const std::string type = event.message.value("type", std::string());
    const bool localOwner = status.mode == net::Mode::DirectHost ||
        (net::is_room_mode(status.mode) && status.isOwner);
    const bool fromOwner = status.mode == net::Mode::DirectJoin ?
        event.peerId == "direct" :
        net::is_room_mode(status.mode) && !status.isOwner &&
            !status.ownerClientId.empty() && event.peerId == status.ownerClientId;
    if (type == "speedrun_reset") {
        if (fromOwner && game::speedrun::mode_active()) {
            clear_speedrun_request();
            speedrunResetAt_ = std::chrono::steady_clock::now() +
                               std::chrono::milliseconds(500);
        }
        return;
    }
    const auto idField = event.message.find("request_id");
    if (idField == event.message.end() || !idField->is_number_unsigned()) return;
    const uint64_t id = idField->get<uint64_t>();
    if (id == 0) return;
    if (type == "speedrun_check") {
        if (!fromOwner || speedrunRequestId_ != 0) return;
        clear_speedrun_request();
        speedrunPeerRequestId_ = id;
        speedrunPhase_ = SpeedrunPhase::CheckingSaves;
        if (game_ != nullptr) game_->suppress_progression_prompts(true);
        // The host's 10-second deadline can expire just before its commit
        // reaches a guest, so retain the guest's ready state a little longer.
        speedrunDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        speedrunProbe_.begin();
    } else if (type == "speedrun_ready") {
        if (!localOwner || id != speedrunRequestId_ ||
            !speedrunWaitingPeers_.contains(event.peerId)) return;
        const auto ready = event.message.find("ready");
        if (ready == event.message.end() || !ready->is_boolean()) return;
        if (!ready->get<bool>()) {
            const auto reason = event.message.find("reason");
            const std::string detail = reason != event.message.end() && reason->is_string() ?
                reason->get<std::string>() : "Speedrun saves are not clear.";
            cancel_speedrun_start(detail);
            return;
        }
        speedrunWaitingPeers_.erase(event.peerId);
        if (speedrunLocalReady_ && speedrunWaitingPeers_.empty()) begin_speedrun_ready();
    } else if (type == "speedrun_prompt") {
        if (!fromOwner || id != speedrunPeerRequestId_ ||
            speedrunPhase_ != SpeedrunPhase::CheckingSaves || !speedrunPeerReady_) return;
        const auto count = event.message.find("player_count");
        if (count == event.message.end() || !count->is_number_unsigned() ||
            count->get<uint32_t>() < 1 || count->get<uint32_t>() > 8) return;
        if (!game::speedrun::mode_active() || !game::speedrun::can_start_here()) {
            (void)transport_.send_to(status.mode == net::Mode::DirectJoin ?
                "direct" : status.ownerClientId,
                {{"type", "speedrun_player_ready"}, {"request_id", id}, {"ready", false}});
            clear_speedrun_request();
            game::push_online_notification("Could not ready Speedrun here.", 4.0f, true);
            return;
        }
        speedrunReadyTotal_ = count->get<uint32_t>();
        speedrunReadyCount_ = 0;
        speedrunPhase_ = SpeedrunPhase::WaitingReady;
        if (!game::speedrun::close_menus_after_start())
            log_info("Speedrun ready prompt could not close Dusklight menus");
    } else if (type == "speedrun_player_ready") {
        if (!localOwner || id != speedrunRequestId_ ||
            speedrunPhase_ != SpeedrunPhase::WaitingReady ||
            !speedrunParticipants_.contains(event.peerId)) return;
        if (event.message.contains("ready") &&
            event.message["ready"].is_boolean() &&
            !event.message["ready"].get<bool>()) {
            cancel_speedrun_start("Speedrun start cancelled: a player could not ready.");
            return;
        }
        if (!speedrunReadyPeers_.insert(event.peerId).second) return;
        speedrunReadyCount_ = static_cast<uint32_t>(speedrunReadyPeers_.size() +
                                                   (speedrunLocalPlayerReady_ ? 1 : 0));
        if (!transport_.send({{"type", "speedrun_ready_count"},
                              {"request_id", id}, {"ready_count", speedrunReadyCount_},
                              {"player_count", speedrunReadyTotal_}})) {
            cancel_speedrun_start("Could not update Speedrun ready count.");
            return;
        }
        begin_speedrun_countdown();
    } else if (type == "speedrun_ready_count") {
        if (!fromOwner || id != speedrunPeerRequestId_ ||
            speedrunPhase_ != SpeedrunPhase::WaitingReady) return;
        const auto count = event.message.find("ready_count");
        const auto total = event.message.find("player_count");
        if (count == event.message.end() || !count->is_number_unsigned() ||
            total == event.message.end() || !total->is_number_unsigned() ||
            total->get<uint32_t>() != speedrunReadyTotal_ ||
            count->get<uint32_t>() > speedrunReadyTotal_) return;
        speedrunReadyCount_ = count->get<uint32_t>();
    } else if (type == "speedrun_countdown") {
        if (!fromOwner || id != speedrunPeerRequestId_ ||
            speedrunPhase_ != SpeedrunPhase::WaitingReady ||
            !speedrunLocalPlayerReady_) return;
        speedrunReadyCount_ = speedrunReadyTotal_;
        speedrunPhase_ = SpeedrunPhase::Countdown;
        speedrunCountdownAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    } else if (type == "speedrun_cancel") {
        if (!fromOwner || id != speedrunPeerRequestId_) return;
        clear_speedrun_request();
        game::push_online_notification("Speedrun start cancelled.", 4.0f, true);
    } else if (type == "speedrun_start") {
        if (!fromOwner || id != speedrunPeerRequestId_ || !speedrunPeerReady_ ||
            speedrunPhase_ != SpeedrunPhase::Countdown || !speedrunLocalPlayerReady_) return;
        clear_speedrun_request();
        if (router_ != nullptr) router_->clear();
        if (!game::speedrun::start_run())
            game::push_online_notification("Could not start Speedrun locally.", 4.0f, true);
        else {
            if (!game::speedrun::close_menus_after_start())
                log_info("Speedrun started, but Dusklight menu close is unavailable");
            game::push_online_notification("Starting Speedrun", 1.5f);
        }
    }
}

ModResult OnlineApp::update_window(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.refresh_inline_player_rows();
    if (app.windowStatus_ != 0) {
        const std::string status = app.dashboard_rml();
        if (status != app.windowRenderedStatus_ &&
            svc_ui->elem_set_rml(mod_ctx, app.windowStatus_, status.c_str()) != MOD_OK) {
            // A tab rebuild invalidates every element handle from its prior
            // generation. Stop immediately if the host rebuilt underneath us.
            app.windowStatus_ = 0;
            app.windowPlayersEmpty_ = 0;
            for (auto& slot : app.inlineKickRows_) slot = {};
        }
        app.windowRenderedStatus_ = status;
    }
    return MOD_OK;
}

ModResult OnlineApp::update_lobby_window(ModContext*, void* data, ModError*) {
    auto& app = *static_cast<OnlineApp*>(data);
    const bool manual = app.bool_value(app.config_.relayManualHost);
    for (auto& handle : app.manualHostControls_) {
        if (handle && svc_ui->elem_set_class(mod_ctx, handle,
            "online-manual-only-hidden", !manual) != MOD_OK) handle = 0;
    }
    return MOD_OK;
}

void OnlineApp::window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.window_ = 0;
    app.windowStatus_ = 0;
    app.windowPlayersEmpty_ = 0;
    for (auto& slot : app.inlineKickRows_) slot = {};
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
    app.manualHostControls_ = {};
}

void OnlineApp::settings_window_closed(ModContext*, UiWindowHandle, void* data) {
    static_cast<OnlineApp*>(data)->settingsWindow_ = 0;
}

void OnlineApp::sync_window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.syncWindow_ = 0;
    app.manualPeerButtonElements_.clear();
    app.manualSyncFlagsButton_ = 0;
    app.manualSyncWarpButton_ = 0;
}

void OnlineApp::lobby_window_closed(ModContext*, UiWindowHandle, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.lobbyWindow_ = 0;
    app.manualHostControls_ = {};
}

bool OnlineApp::player_colour_locked(ModContext*, void* data) {
    return static_cast<OnlineApp*>(data)->bool_value(
        static_cast<OnlineApp*>(data)->config_.matchOutfitColor, true);
}

bool OnlineApp::voice_proximity_range_locked(ModContext* mod_ctx, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return voice_proximity_setting_locked(mod_ctx, data) ||
        !app.displayed_settings().voiceProximity;
}

bool OnlineApp::voice_proximity_setting_locked(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return status.enabled &&
        (status.mode == net::Mode::DirectJoin ||
         (net::is_room_mode(status.mode) && !status.isOwner));
}

void OnlineApp::reset_player_options(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    svc_config->set_string(mod_ctx, app.config_.outfitColor, "");
    svc_config->set_string(mod_ctx, app.config_.zoraColor, "");
    svc_config->set_string(mod_ctx, app.config_.playerColor, "");
    svc_config->set_bool(mod_ctx, app.config_.matchOutfitColor, true);
}

void OnlineApp::open_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::settings_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_settings_window();
}
void OnlineApp::sync_menu_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_sync_window();
}
void OnlineApp::host_lobby_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_lobby_window(ConnectionRole::Host);
}
void OnlineApp::join_lobby_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->open_lobby_window(ConnectionRole::Join);
}
void OnlineApp::menu_selected(ModContext*, void* data) { static_cast<OnlineApp*>(data)->open_window(); }
void OnlineApp::disconnect_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->disconnect(); }
void OnlineApp::stop_hosting_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.requestedDisconnectStatus_ = "Not connected";
    app.disconnect();
}
void OnlineApp::host_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_direct(); }
void OnlineApp::join_direct_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_direct(); }
void OnlineApp::host_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->host_relay(); }
void OnlineApp::join_relay_pressed(ModContext*, void* data) { static_cast<OnlineApp*>(data)->join_relay(); }
void OnlineApp::direct_code_get(ModContext*, void* data, UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.directCodeDisplay_ = app.string_value(app.config_.directInvite);
    value->string_value = app.directCodeDisplay_.c_str();
}
void OnlineApp::direct_code_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string entered = value->string_value != nullptr ? value->string_value : "";
    if (entered != app.directCodeDisplay_) {
        svc_config->set_string(mod_ctx, app.config_.directInvite,
                               trim_clipboard_text(entered).c_str());
    }
}
void OnlineApp::relay_code_get(ModContext*, void* data, UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.relayCodeDisplay_ = app.string_value(app.config_.relayCode);
    value->string_value = app.relayCodeDisplay_.c_str();
}
void OnlineApp::relay_code_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string entered = value->string_value != nullptr ? value->string_value : "";
    if (entered != app.relayCodeDisplay_) {
        svc_config->set_string(mod_ctx, app.config_.relayCode,
                               trim_clipboard_text(entered).c_str());
    }
}
void OnlineApp::copy_direct_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = !app.activeCode_.empty() ? app.activeCode_ :
        app.string_value(app.config_.directInvite);
    const bool copied = write_clipboard(code);
    push_clipboard_toast(copied ? "Direct invite code copied" :
        "Could not copy the direct invite code", !copied);
}
void OnlineApp::paste_direct_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = read_clipboard();
    if (code.empty()) {
        push_clipboard_toast("Clipboard does not contain a code", true);
        return;
    }
    svc_config->set_string(mod_ctx, app.config_.directInvite, code.c_str());
    push_clipboard_toast("Direct invite code pasted");
}
void OnlineApp::copy_relay_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = !app.activeCode_.empty() ? app.activeCode_ :
        app.string_value(app.config_.relayCode);
    const bool copied = write_clipboard(code);
    push_clipboard_toast(copied ? "Relay code copied" :
        "Could not copy the relay code", !copied);
}
void OnlineApp::paste_relay_code_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const std::string code = read_clipboard();
    if (code.empty()) {
        push_clipboard_toast("Clipboard does not contain a code", true);
        return;
    }
    svc_config->set_string(mod_ctx, app.config_.relayCode, code.c_str());
    push_clipboard_toast("Relay code pasted");
}
void OnlineApp::manual_peer_pressed(ModContext*, void* data) {
    const auto& context = *static_cast<ManualPeerButtonContext*>(data);
    if (context.app != nullptr && context.index >= 0 &&
        context.index < static_cast<int64_t>(context.app->manualPeerIds_.size())) {
        context.app->selectedManualPeer_ = context.index;
        for (size_t i = 0; i < context.app->manualPeerButtonElements_.size(); ++i) {
            const UiElementHandle button = context.app->manualPeerButtonElements_[i];
            if (button != 0) {
                svc_ui->elem_set_class(mod_ctx, button, "online-peer-selected",
                                       static_cast<int64_t>(i) == context.index);
            }
        }
    }
}
bool OnlineApp::manual_peer_selected(ModContext*, void* data) {
    const auto& context = *static_cast<ManualPeerButtonContext*>(data);
    return context.app != nullptr && context.app->selectedManualPeer_ == context.index;
}
void OnlineApp::inline_kick_pressed(ModContext*, void* data) {
    auto& slot = *static_cast<InlineKickRow*>(data);
    if (slot.app == nullptr || slot.peerId.empty() || slot.kickPending) return;

    auto& app = *slot.app;
    const auto peer = app.transport_.peers().find(slot.peerId);
    if (peer == app.transport_.peers().end()) {
        app.refresh_inline_player_rows();
        return;
    }
    const std::string peerName = peer->second.empty() ? "Player" : peer->second;
    std::string error;
    if (!app.transport_.kick_peer(slot.peerId, &error)) {
        game::push_online_notification(
            failure_message("Could not kick " + peerName,
                            error.empty() ? "The request could not be sent." : error),
            5.0f, true);
        return;
    }
    slot.kickPending = true;
    app.statusMessage_ = "Removing " + peerName + " from the lobby";
}
void OnlineApp::manual_sync_warp_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->request_manual_sync(false);
}
void OnlineApp::manual_sync_flags_pressed(ModContext*, void* data) {
    static_cast<OnlineApp*>(data)->request_manual_sync(true);
}
void OnlineApp::refresh_peers_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.reopenWindowPending_ = true;
    if (app.window_ != 0) svc_ui->window_close(mod_ctx, app.window_);
}
void OnlineApp::refresh_sync_peers_pressed(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    app.reopenSyncWindowPending_ = true;
    if (app.syncWindow_ != 0) svc_ui->window_close(mod_ctx, app.syncWindow_);
}
bool OnlineApp::manual_sync_unavailable(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return !app.transport_.status().welcomed || app.manualPeerIds_.empty() ||
           app.manualSyncCooldownTicks_ > 0 ||
           (app.game_ != nullptr && app.game_->manual_sync_waiting());
}
bool OnlineApp::sync_menu_unavailable(ModContext*, void* data) {
    return !static_cast<OnlineApp*>(data)->transport_.status().welcomed;
}
bool OnlineApp::inline_kick_unavailable(ModContext*, void* data) {
    const auto& slot = *static_cast<InlineKickRow*>(data);
    if (slot.app == nullptr || slot.peerId.empty() || slot.kickPending) return true;
    const net::Status status = slot.app->transport_.status();
    const bool owner = status.mode == net::Mode::DirectHost ||
        (net::is_room_mode(status.mode) && status.isOwner);
    return !status.enabled || !owner ||
        slot.app->transport_.peers().find(slot.peerId) == slot.app->transport_.peers().end();
}
bool OnlineApp::session_active(ModContext*, void* data) {
    return static_cast<OnlineApp*>(data)->transport_.status().enabled;
}
bool OnlineApp::host_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled ||
        (status.mode != net::Mode::DirectHost &&
         !(net::is_room_mode(status.mode) && (app.relayHostIntent_ || status.isOwner)));
}
bool OnlineApp::joiner_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || status.mode == net::Mode::DirectHost ||
        (net::is_room_mode(status.mode) && (app.relayHostIntent_ || status.isOwner));
}
bool OnlineApp::direct_host_inactive(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return !status.enabled || status.mode != net::Mode::DirectHost;
}
bool OnlineApp::direct_join_inactive(ModContext*, void* data) {
    const net::Status status = static_cast<OnlineApp*>(data)->transport_.status();
    return !status.enabled || status.mode != net::Mode::DirectJoin;
}
bool OnlineApp::relay_host_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || !net::is_room_mode(status.mode) ||
        (!app.relayHostIntent_ && !status.isOwner);
}
bool OnlineApp::relay_join_inactive(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    return !status.enabled || !net::is_room_mode(status.mode) ||
        app.relayHostIntent_ || status.isOwner;
}
bool OnlineApp::room_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    return room_settings_locked(app.transport_.status(), app.relayHostIntent_);
}
bool OnlineApp::remote_collision_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_locked(status, app.relayHostIntent_);
    const bool modelEnabled = status.enabled ? status.settings.dummyModel :
                                               app.bool_value(app.config_.dummyModel, true);
    return roomLocked || !modelEnabled;
}
bool OnlineApp::pvp_setting_locked(ModContext*, void* data) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    const bool roomLocked = room_settings_locked(status, app.relayHostIntent_);
    // The raw room collision option controls this UI lock. The remote-model
    // option participates only in effective runtime PvP.
    const bool collisionEnabled = status.enabled ? status.settings.remoteCollision :
        app.bool_value(app.config_.remoteCollision, true);
    return roomLocked || !collisionEnabled;
}

void OnlineApp::dummy_model_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().dummyModel;
}

void OnlineApp::dummy_model_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.dummyModel, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.dummyModel = value->bool_value;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::sync_flags_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().syncFlags;
}

void OnlineApp::sync_flags_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.syncFlags, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.syncFlags = value->bool_value;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::save_recovery_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().saveRecovery;
}

void OnlineApp::save_recovery_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.saveRecovery, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        (void)app.transport_.publish_save_recovery_setting(value->bool_value);
    }
}

void OnlineApp::remote_collision_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().remoteCollision;
}

void OnlineApp::remote_collision_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.remoteCollision, value->bool_value);
    if (!value->bool_value) svc_config->set_bool(mod_ctx, app.config_.pvp, false);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.remoteCollision = value->bool_value;
        if (!settings.remoteCollision) settings.pvp = false;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::pvp_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = net::effective_pvp(
        static_cast<OnlineApp*>(data)->displayed_settings());
}

void OnlineApp::pvp_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (room_settings_locked(status, app.relayHostIntent_)) return;
    svc_config->set_bool(mod_ctx, app.config_.pvp, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        net::RoomSettings settings = status.settings;
        settings.pvp = value->bool_value && settings.remoteCollision;
        settings.syncWorld = false;
        app.transport_.publish_room_settings(settings);
    }
}

void OnlineApp::voice_proximity_get(ModContext*, void* data, UiControlValue* value) {
    value->bool_value = static_cast<OnlineApp*>(data)->displayed_settings().voiceProximity;
}

void OnlineApp::voice_proximity_set(ModContext*, void* data, const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (voice_proximity_setting_locked(mod_ctx, data)) return;
    svc_config->set_bool(mod_ctx, app.config_.voiceProximity, value->bool_value);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        app.transport_.publish_voice_settings(value->bool_value,
                                              status.settings.voiceProximityRange);
    }
}

void OnlineApp::voice_proximity_range_get(ModContext*, void* data, UiControlValue* value) {
    value->int_value = static_cast<OnlineApp*>(data)->displayed_settings().voiceProximityRange;
}

void OnlineApp::voice_proximity_range_set(ModContext*, void* data,
                                          const UiControlValue* value) {
    auto& app = *static_cast<OnlineApp*>(data);
    const net::Status status = app.transport_.status();
    if (voice_proximity_setting_locked(mod_ctx, data) ||
        !app.displayed_settings().voiceProximity) return;
    const int percent = static_cast<int>(std::clamp(value->int_value, int64_t{0}, int64_t{200}));
    svc_config->set_int(mod_ctx, app.config_.voiceProximityRange, percent);
    if (status.enabled &&
        (status.mode == net::Mode::DirectHost ||
         (net::is_room_mode(status.mode) && status.isOwner))) {
        app.transport_.publish_voice_settings(status.settings.voiceProximity, percent);
    }
}

}  // namespace dusklight_online
