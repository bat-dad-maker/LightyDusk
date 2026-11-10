#pragma once

#include "dusklight_online/game/protocol_router.hpp"
#include "dusklight_online/game/pose_playback.hpp"
#include "dusklight_online/game/save_recovery.hpp"
#include "dusklight_online/game/ooccoo_sync.hpp"
#include "dusklight_online/game/visual_bridge.hpp"
#include "dusk/multiplayer/multiplayer.hpp"

#include <mods/api.h>
#include <mods/svc/item.h>
#include <mods/svc/save.h>

#include <cstdint>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dusklight_online::game {

// Version-neutral game integration: every engine call is made through public
// game headers/symbols from the active Dusklight GameService epoch. No fixed
// executable addresses or per-build offsets are stored in the mod.
class GameAdapter final : public MessageConsumer {
public:
    explicit GameAdapter(net::Transport& transport);

    ModResult initialize_hooks(ModError* error);
    void shutdown_hooks();
    void update(bool syncFlagsEnabled, bool syncWorldEnabled, bool remoteModelEnabled,
                bool nameLabelsEnabled, bool displayMidnaEnabled,
                bool matrixStreamingEnabled,
                bool remoteCollisionEnabled,
                bool pvpEnabled, bool playerListEnabled);
    // Capture mutations made by the completed game tick before inbound
    // progression can rebaseline the observer around a remote application.
    void capture_local_mutations_before_remote(bool syncFlagsEnabled);
    // Called from the fapGm_Execute pre-hook, after Dusklight has sampled the
    // controller but before vanilla menu/gameplay code consumes D-Pad Down.
    void consume_progression_prompt_input();
    void set_progression_prompts_enabled(bool enabled);
    void suppress_progression_prompts(bool suppress);
    void publish_local(nlohmann::json message);
    bool request_manual_sync(std::string_view peerId, bool flagsOnly, std::string* error = nullptr);
    [[nodiscard]] bool applying_remote() const;
    [[nodiscard]] bool randomizer_active() const;
    [[nodiscard]] bool ooccoo_sync_active() const;
    [[nodiscard]] bool floor_switch_sync_active() const;

    [[nodiscard]] bool stage_ready() const override;
    [[nodiscard]] bool allow_stage_unready(const RoutedMessage& message) const override;
    [[nodiscard]] bool discard_stage_message(const RoutedMessage& message) const override;
    bool retain_for_save_recovery(const RoutedMessage& message) override;
    ApplyResult consume(const RoutedMessage& message) override;
    ApplyResult consume_udp(const net::Event& event) override;
    void peer_joined(std::string_view peerId, std::string_view name) override;
    void peer_left(std::string_view peerId) override;

    void report_pvp_target_hit(fopAc_ac_c* remoteLinkActor, fopAc_ac_c* attackActor,
                               dCcD_GObjInf* attackInfo);
    void notify_local_save_reset();
    void notify_local_save_new(uint32_t slot);
    void notify_local_save_loaded(uint32_t slot);
    void notify_local_save_written(uint32_t slot);
    void set_save_recovery_policy(bool ready, bool enabled);
    void restore_saved_progress_if_ready();
    void notify_room_scene_initialized(int room);
    void notify_local_event_bit(uint16_t flag);
    void observe_local_memory_item(int stage, int flag);
    void notify_local_fish_caught(int fishIndex);
    void notify_local_light_drop_num(int area, int previous, int value);
    void notify_local_dark_clear(int level);
    void notify_local_max_life(int previous, int value);
    [[nodiscard]] bool should_suppress_local_bottle_source(uint8_t sourceItem) const;
    void notify_local_bottle_slots(int previous, int value, uint8_t sourceItem);
    void notify_local_rupees(int previous, int value);
    void notify_local_poe_pickup(int previous, int value);
    void notify_local_item_grant(const ItemGiveInfo& info);
    void notify_local_ooccoo_warp_out();

    void set_player_color(uint32_t color, uint32_t outfit, uint32_t zora);
    void publish_player_color();
    [[nodiscard]] std::optional<uint32_t> peer_latency_ms(std::string_view peerId) const;
    [[nodiscard]] std::optional<net::udp::VoicePosition> voice_position() const;
    [[nodiscard]] float voice_gain(const net::udp::VoicePosition& position,
                                   int rangePercent) const;
    [[nodiscard]] float voice_pan(const net::udp::VoicePosition& position) const;
    void reset_session();
    [[nodiscard]] const std::string& last_error() const;
    [[nodiscard]] std::string manual_sync_status_text() const;
    [[nodiscard]] bool manual_sync_waiting() const;
    [[nodiscard]] bool manual_sync_failed() const;
    [[nodiscard]] bool manual_sync_timed_out() const;

private:
    void sample_live_field_map_marker();
    [[nodiscard]] std::optional<PlayerLocationView::FieldMapMarker> local_field_map_marker() const;
    void observe_latency_presence(const RoutedMessage& message);
    [[nodiscard]] std::optional<uint32_t> current_peer_latency_ms(
        std::string_view peerId, std::chrono::steady_clock::time_point now) const;
    net::Transport& transport_;
    std::map<std::string, std::string> peerNames_;
    std::map<std::string, nlohmann::json> peerPresence_;
    std::optional<PlayerLocationView::FieldMapMarker> liveFieldMapMarker_;
    mutable std::optional<net::udp::VoicePosition> lastVoicePosition_;
    struct LatencyProbe {
        uint64_t nonce = 0;
        std::chrono::steady_clock::time_point receivedAt{};
    };
    struct LatencySample {
        uint64_t nonce = 0;
        uint32_t milliseconds = 0;
        std::chrono::steady_clock::time_point receivedAt{};
    };
    std::map<std::string, LatencyProbe> latencyEchoes_;
    std::map<std::string, LatencySample> latencySamples_;
    std::deque<std::pair<uint64_t, std::chrono::steady_clock::time_point>> latencySent_;
    uint64_t nextLatencyNonce_ = 0;
    mutable std::map<std::string, uint32_t> displayedLatencies_;
    mutable std::chrono::steady_clock::time_point displayedLatenciesAt_{};
    std::map<std::string, nlohmann::json> peerProgressionStates_;
    std::map<std::string, uint32_t> peerProgressionAges_;
    std::deque<RoutedMessage> deferredFaronInbound_;
    std::map<std::string, dusk::multiplayer::PeerPoseSnapshot> peerPoses_;
    std::map<std::string, PosePlayback<dusk::multiplayer::PeerPoseSnapshot>> posePlayback_;
    std::map<std::string, dusk::multiplayer::PeerPoseSnapshot> presentedPoses_;
    struct PoseTiming {
        std::chrono::steady_clock::time_point received{};
        uint32_t receivedSequence = 0, displayedSequence = 0;
        uint32_t packets = 0, missing = 0, repeats = 0, skipped = 0, burst = 0, pending = 0, predicted = 0;
        double maxReceiveGapMs = 0;
    };
    std::map<std::string, PoseTiming> poseTiming_;
    std::chrono::steady_clock::time_point poseTimingUpdate_{};
    uint32_t poseTimingTicks_ = 0;
    uint32_t posePresentationSequence_ = 0;
    double poseTimingMaxUpdateMs_ = 0;
    std::map<std::string, uint32_t> latestAckSequence_;
    std::map<std::string, uint32_t> pvpRemoteHitLastSequence_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> chatMessageTimes_;
    // Link exposes several sword attack objects during one swing. Remember
    // contacts already reported during this game update so those colliders
    // produce one network hit without imposing a cross-frame cooldown.
    std::set<std::pair<std::string, uintptr_t>> pvpLocalHitContactsThisUpdate_;
    // A Spinner spin attack can remain in contact for several game updates.
    // Send its heavy hit once per target for each attack animation.
    uintptr_t pvpSpinnerActor_ = 0;
    bool pvpSpinnerAttackActive_ = false;
    std::set<std::string> pvpSpinnerAttackTargets_;
    std::deque<nlohmann::json> deferredSwitches_;
    std::map<std::string, uint32_t> permanentPickupSequence_;
    std::map<std::string, uint32_t> fishCatchSequence_;
    std::map<int, std::set<int>> observedMemoryItems_;
    // A randomized location may legitimately contain the same item as many
    // other locations. Deduplicate by ItemService check identity, never by the
    // resolved item byte.
    std::set<std::string> completedRandomizerChecks_;
    // Vanilla has four fixed bottle rewards. Remember their identities so the
    // same world reward observed by two peers is applied once, while distinct
    // rewards acquired concurrently still add separate slots.
    std::set<uint8_t> completedBottleSources_;
    bool bottleSourcesComplete_ = false;
    std::map<std::string, bool> appliedTearEvents_;
    std::string lastError_;
    uint32_t progressionTicks_ = 0;
    uint32_t presenceTicks_ = 0;
    uint32_t collectibleRepairTicks_ = 0;
    uint32_t localPoseSequence_ = 0;
    uint32_t localPvpHitSequence_ = 0;
    bool applyingRemote_ = false;
    bool syncFlagsEnabled_ = true;
    bool saveRecoveryEnabled_ = false;
    bool saveRecoveryPolicyReady_ = false;
    std::string activeSaveName_;
    std::optional<uint32_t> activeSaveSlot_;
    bool syncWorldEnabled_ = false;
    bool hooksInstalled_ = false;
    SaveObserverHandle saveObserver_ = 0;
    struct SaveRecoveryJournal {
        std::string saveName;
        uint32_t slot = 0;
        std::chrono::steady_clock::time_point savedAt{};
        std::chrono::steady_clock::time_point captureUntil{};
        SaveRecoveryDelta delta;
        std::deque<RoutedMessage> titleMessages;
        size_t titleMessageBytes = 0;
        bool restorePending = false;
    };
    std::optional<SaveRecoveryJournal> saveRecovery_;
    bool replayingSaveRecovery_ = false;
    RecoveryFacts recoveryReplayIntents_;
    ItemGiveHandle itemGiveObserver_ = 0;
    // Save-scoped acquisition receipts; never stores a peer's return warp.
    ooccoo::State ooccooState_;
    bool ooccooBoundToSave_ = false;
    bool ooccooCatchupPending_ = true;
    bool ooccooReplyPending_ = false;
    struct BombBagSaveWarpCandidate { int bag; int item; };
    std::optional<BombBagSaveWarpCandidate> observedBombBagRental_;
    std::optional<BombBagSaveWarpCandidate> pendingBombBagSaveWarp_;
    nlohmann::json localObservedState_;
    std::string stableStageName_;
    int stableRoom_ = -128;
    uint32_t stableRoomTicks_ = 0;
    std::string initializedStageName_;
    int initializedRoom_ = -128;
    uint32_t initializedRoomTicks_ = 0;
    std::array<uint8_t, 3> pendingDarkClears_{};
    uint32_t localPermanentSequence_ = 0;
    uint32_t localFishSequence_ = 0;
    std::optional<uint16_t> pendingRupeePublicationToSuppress_;
    std::optional<int> observedRupees_;
    std::optional<uint8_t> pendingMaxLifePublicationToSuppress_;
    std::vector<uint8_t> pendingManualInfo_;
    std::vector<uint8_t> pendingManualFlagsSave_;
    std::optional<uint8_t> pendingManualVibration_;
    bool manualTransitionActive_ = false;
    bool manualReloadPending_ = false;
    std::set<uint16_t> pendingOrdonEventBits_;
    uint32_t ordonReloadSafeTicks_ = 0;
    uint32_t ordonReloadWaitTicks_ = 0;
    bool ordonReloadTransitionActive_ = false;
    bool ordonReloadSawStageLoad_ = false;
    bool mirrorReloadPending_ = false;
    nlohmann::json zoraThawPending_;
    bool localWagonEscortStarted_ = false;
    std::deque<nlohmann::json> deferredStoryEvents_;
    std::deque<nlohmann::json> deferredLocalEvents_;
    uint32_t faronDayBroadcastHoldTicks_ = 0;
    bool localFaronCageSequenceActive_ = false;
    bool localFaronWarpSequenceActive_ = false;
    int lastLocalTboxStage_ = -1;
    int lastLocalTboxFlag_ = -1;
    std::chrono::steady_clock::time_point lastLocalTboxAt_{};

    struct ProgressionPrompt {
        bool active = false;
        std::string peerId;
        std::string peerName;
        std::string cueKey;
        std::string title;
        std::string body;
        uint32_t ageTicks = 0;
        uint32_t holdTicks = 0;
        bool waiting = false;
    } progressionPrompt_;
    bool progressionPromptsEnabled_ = false;
    bool progressionPromptsSuppressed_ = false;
    bool progressionPromptAcceptHeld_ = false;
    struct PendingProgressionCue {
        std::string peerId;
        std::string cueKey;
        std::string title;
        std::string body;
        std::string expectedStage;
    };
    std::vector<PendingProgressionCue> pendingProgressionCues_;
    std::string pendingProgressionPeerId_;
    std::string pendingProgressionCueKey_;
    std::string awaitingManualSyncCueKey_;
    std::string awaitingManualSyncPeerId_;
    uint64_t awaitingManualSyncRequestId_ = 0;
    std::set<std::string> handledProgressionCues_;
    std::set<std::string> shownPoseProgressionCues_;
    struct PendingSyncReply {
        std::string peerId;
        std::string cueKey;
        bool flagsOnly = false;
        uint32_t waitTicks = 0;
        uint64_t requestId = 0;
    };
    std::vector<PendingSyncReply> pendingSyncReplies_;
    enum class ManualSyncState : uint8_t { None, Waiting, Succeeded, Failed };
    ManualSyncState manualSyncState_ = ManualSyncState::None;
    bool manualSyncFlagsOnly_ = false;
    bool manualSyncRequestedFromTitle_ = false;
    std::string manualSyncPeerId_;
    uint64_t manualSyncRequestId_ = 0;
    uint64_t nextManualSyncRequestId_ = 0;
    uint32_t manualSyncWaitTicks_ = 0;
    bool manualSyncTimedOut_ = false;

    ApplyResult consume_progression(const RoutedMessage& message);
    ApplyResult consume_randomizer(const RoutedMessage& message);
    ApplyResult consume_pvp_hit(const RoutedMessage& message);
    nlohmann::json make_save_snapshot();
    nlohmann::json make_recovery_snapshot();
    void record_received_progress(const nlohmann::json& before, ApplyResult result);
    void record_recovery_intent(const RoutedMessage& message, const RecoveryFact& fact,
                                const nlohmann::json& value);
    ApplyResult apply_save_snapshot(const RoutedMessage& message);
    ApplyResult apply_switch_bit(const nlohmann::json& message,
                                 std::string_view peerId = {});
    ApplyResult apply_snapshot_switch_bit(int stage, int flag);
    void flush_deferred_switches();
    ApplyResult apply_dark_clear(int level);
    void flush_pending_dark_clears();
    bool accept_ooccoo_state(const nlohmann::json& state);
    void apply_shared_ooccoo_local_form();
    void bind_ooccoo_to_save();
    void flush_ooccoo_catchup();
    nlohmann::json ooccoo_snapshot_state();
    void send_snapshot_to(std::string_view peerId = {}, bool manual = false,
                          bool flagsOnly = false, uint64_t requestId = 0);
    std::string encode_manual_full_state();
    bool apply_manual_full_state(const std::string& encoded, bool flagsOnly,
                                 std::string_view peerId);
    void tick_manual_transition();
    void update_pending_sync_replies();
    [[nodiscard]] bool engine_stage_ready() const;
    ApplyResult apply_event_bit(const RoutedMessage& message);
    void flush_story_events();
    void flush_story_messages();
    void send_progression_state(bool force = false);
    void set_local_faron_warp_sequence_active(bool active);
    void update_local_faron_cage_sequence_state();
    bool has_active_faron_cage_sequence_peer() const;
    bool should_defer_faron_warp_sequence() const;
    void poll_local_state(bool publish);
    void observe_bomb_bag_rental();
    void persist_bomb_bag_save_warp_candidate();
    void load_bomb_bag_save_warp_candidate();
    void flush_bomb_bag_save_warp_candidate();
    void clear_disabled_sync_flags_state();
    void clear_replaced_save_progression_state();
    void load_bottle_source_state();
    void persist_bottle_source_state() const;
    void replace_bottle_source_state(const nlohmann::json& message);
    void remember_memory_item(int stage, int flag);
    void reapply_observed_memory_items_for_current_stage();
    ApplyResult reject(std::string reason);
    void consume_welcome_membership(const nlohmann::json& message);
    bool request_manual_sync_impl(std::string_view peerId, bool flagsOnly,
                                  std::string_view cueKey, std::string* error,
                                  bool trackStatus = true);
    [[nodiscard]] bool expecting_manual_sync_reply(std::string_view peerId,
                                                    uint64_t requestId) const;
    void update_progression_prompts();
    void clear_progression_prompts();
    void maybe_queue_progression_pose_prompt(std::string_view peerId,
                                             const dusk::multiplayer::PeerPoseSnapshot& pose);
    void maybe_queue_progression_event_prompt(std::string_view peerId, uint16_t flag);
    void maybe_queue_progression_switch_prompt(std::string_view peerId, int stage, int flag);
    bool local_wagon_escort_unfinished();
};

}  // namespace dusklight_online::game
