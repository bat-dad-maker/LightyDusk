#include "dusklight_online/game/protocol_router.hpp"

#include <array>
#include <utility>

namespace dusklight_online::game {
namespace {

using Domain = MessageDomain;

struct Entry {
    std::string_view type;
    MessageSpec spec;
};

// This is deliberately exhaustive for protocol 2 and local control messages.
// Additions remain visible as Unsupported instead of falling through a
// permissive catch-all.
constexpr std::array kEntries = {
    Entry{"welcome", {Domain::Session, false, false}},
    Entry{"error", {Domain::Session, false, false}},
    Entry{"ping", {Domain::Session, false, false}},
    Entry{"pong", {Domain::Session, false, false}},
    Entry{"ack", {Domain::Session, false, false}},
    Entry{"udp_ready", {Domain::Session, false, false}},
    Entry{"peer_joined", {Domain::Membership, false, false}},
    Entry{"peer_left", {Domain::Membership, false, false}},
    Entry{"owner_changed", {Domain::Membership, false, false}},
    Entry{"room_settings", {Domain::Membership, false, false}},
    Entry{"name_labels", {Domain::Membership, false, false}},
    Entry{"dummy_model", {Domain::Membership, false, false}},
    Entry{"sync_flags", {Domain::Membership, false, false}},
    Entry{"sync_world", {Domain::Membership, false, false}},
    Entry{"remote_collision", {Domain::Membership, false, false}},
    Entry{"pvp_enabled", {Domain::Membership, false, false}},
    Entry{"voice_settings", {Domain::Membership, false, false}},
    Entry{"save_recovery_setting", {Domain::Membership, false, false}},
    Entry{"presence", {Domain::Presence, false, false}},
    Entry{"progression_state", {Domain::Presence, false, true}},
    Entry{"puppet_preference", {Domain::Presence, false, false}},
    Entry{"midna_preference", {Domain::Presence, false, false}},
    Entry{"chat", {Domain::Chat, false, false}},
    // A sync request has its own arrival contract: the game adapter retains
    // unsafe stage/event/title arrivals in its typed, bounded reply queue. It
    // must not enter the generic stage-message queue as a second copy.
    Entry{"sync_request", {Domain::Progression, false, false}},
    Entry{"save_snapshot", {Domain::Progression, true, true}},
    Entry{"event_bit", {Domain::Progression, false, true}},
    Entry{"tbox_bit", {Domain::Progression, true, true}},
    Entry{"switch_bit", {Domain::Progression, true, true}},
    Entry{"room_switch_bit", {Domain::Progression, true, true}},
    Entry{"web_timer", {Domain::Progression, true, true}},
    // A live actor animation is meaningful only at arrival time in the exact
    // room. Durable results travel separately as ordinary save switches.
    Entry{"room_actor_action", {Domain::Progression, false, true}},
    // Metadata only; transient pressure must never enter the stage replay queue.
    Entry{"floor_switch_state", {Domain::Progression, false, true}},
    Entry{"item_bit", {Domain::Progression, true, true}},
    Entry{"dungeon_item_bit", {Domain::Progression, true, true}},
    Entry{"item_get", {Domain::Progression, false, true}},
    Entry{"trade_item", {Domain::Progression, true, true}},
    Entry{"item_first_bit", {Domain::Progression, false, true}},
    Entry{"collect_crystal", {Domain::Progression, false, true}},
    Entry{"collect_mirror", {Domain::Progression, false, true}},
    Entry{"dark_clear_lv", {Domain::Progression, true, true}},
    Entry{"transform_lv", {Domain::Progression, false, true}},
    Entry{"region_bit", {Domain::Progression, false, true}},
    Entry{"collect", {Domain::Progression, false, true}},
    Entry{"visited_room", {Domain::Progression, true, true}},
    Entry{"letter_get", {Domain::Progression, false, true}},
    Entry{"ooccoo_state", {Domain::Progression, false, true}},
    Entry{"collect_smell", {Domain::Progression, false, true}},
    Entry{"key_num", {Domain::Progression, true, true}},
    Entry{"light_drop_num", {Domain::Progression, false, true}},
    Entry{"light_drop_get_flag", {Domain::Progression, false, true}},
    Entry{"max_life_update", {Domain::Progression, false, true}},
    Entry{"bottle_slots", {Domain::Progression, false, true}},
    Entry{"bomb_bag_slot", {Domain::Progression, false, true}},
    Entry{"rupee_count", {Domain::Progression, false, true}},
    Entry{"rupee_delta", {Domain::Progression, false, true}},
    Entry{"poe_count", {Domain::Progression, false, true}},
    Entry{"malo_fundraising", {Domain::Progression, false, true}},
    Entry{"charlo_offering", {Domain::Progression, false, true}},
    Entry{"fish_record", {Domain::Progression, false, true}},
    // Resolved rewards are safe away from a loaded stage and must not wait
    // behind unrelated stage work.
    Entry{"rando_item_get", {Domain::OptionalRandomizer, false, true}},
    Entry{"pose", {Domain::Visual, false, false}},
    Entry{"midna_pose", {Domain::Visual, false, false}},
    Entry{"pvp_hit", {Domain::Interaction, false, false}},
    // Dusklight Co-op: partner presence, host co-op options and shared enemy
    // HP. Interaction messages are live-only and never enter save replay.
    Entry{"coop_presence", {Domain::Interaction, false, false}},
    Entry{"coop_settings", {Domain::Interaction, false, false}},
    Entry{"coop_enemy_damage", {Domain::Interaction, false, false}},
    Entry{"ganondorf_owner_claim", {Domain::Ganondorf, false, true}},
    Entry{"ganondorf_owner", {Domain::Ganondorf, false, true}},
    Entry{"ganondorf_hit", {Domain::Ganondorf, false, true}},
    Entry{"ganondorf_reaction", {Domain::Ganondorf, false, true}},
    Entry{"ganondorf_player_damage", {Domain::Ganondorf, false, true}},
    Entry{"ganondorf_state", {Domain::Ganondorf, false, true}},
};

}  // namespace

ProtocolRouter::ProtocolRouter(MessageConsumer& consumer) : consumer_(consumer) {}

MessageSpec ProtocolRouter::classify(std::string_view type) {
    for (const Entry& entry : kEntries) {
        if (entry.type == type) {
            return entry.spec;
        }
    }
    return {};
}

bool ProtocolRouter::is_known_type(std::string_view type) {
    return classify(type).domain != MessageDomain::Unknown;
}

ApplyResult ProtocolRouter::route(const net::Event& event, bool syncFlagsEnabled) {
    switch (event.kind) {
    case net::EventKind::PeerJoined:
        consumer_.peer_joined(event.peerId, event.detail);
        record(ApplyResult::Applied);
        return ApplyResult::Applied;
    case net::EventKind::PeerLeft:
        consumer_.peer_left(event.peerId);
        record(ApplyResult::Applied);
        return ApplyResult::Applied;
    case net::EventKind::UdpMessage:
    case net::EventKind::UdpRemoteObject:
    case net::EventKind::UdpAck: {
        try {
            const ApplyResult result = consumer_.consume_udp(event);
            record(result);
            return result;
        } catch (const nlohmann::json::exception&) {
            lastError_ = "rejected malformed UDP gameplay message";
            record(ApplyResult::Rejected);
            return ApplyResult::Rejected;
        }
    }
    case net::EventKind::Message:
        break;
    default:
        return ApplyResult::IgnoredByPolicy;
    }

    if (!event.message.is_object()) {
        lastError_ = "rejected non-object gameplay message";
        record(ApplyResult::Rejected);
        return ApplyResult::Rejected;
    }
    const auto typeIt = event.message.find("type");
    if (typeIt == event.message.end() || !typeIt->is_string() || typeIt->get_ref<const std::string&>().empty()) {
        lastError_ = "rejected gameplay message without a string type";
        record(ApplyResult::Rejected);
        return ApplyResult::Rejected;
    }

    const std::string& type = typeIt->get_ref<const std::string&>();
    RoutedMessage routed{event.peerId, event.message, classify(type), event.ingress,
                         std::chrono::steady_clock::now()};
    // Each reliable line carries the authoritative settings which existed at
    // its exact place in the receive stream. Using the batch's final status
    // reorders off/event/on and on/event/off semantics.
    return route_message(std::move(routed), event.ingress.settings.syncFlags, true);
}

ApplyResult ProtocolRouter::route_message(RoutedMessage message, bool syncFlagsEnabled,
                                          bool allowQueue) {
    try {
        if (message.spec.domain == MessageDomain::Unknown) {
            lastError_ = "unsupported protocol message type: " +
                         message.payload.value("type", std::string());
            record(ApplyResult::Unsupported);
            return ApplyResult::Unsupported;
        }
        if (message.spec.syncFlagsControlled && !syncFlagsEnabled) {
            record(ApplyResult::IgnoredByPolicy);
            return ApplyResult::IgnoredByPolicy;
        }
        // A player can save and quit while remaining connected. Keep eligible
        // updates for that save before stage deferral or the title's temporary
        // game state can consume them instead.
        if (message.spec.domain == MessageDomain::Progression &&
            consumer_.retain_for_save_recovery(message)) {
            record(ApplyResult::Retained);
            return ApplyResult::Retained;
        }
        if (message.spec.stageDependent && consumer_.discard_stage_message(message)) {
            record(ApplyResult::IgnoredByPolicy);
            return ApplyResult::IgnoredByPolicy;
        }
        if (message.spec.stageDependent && !consumer_.stage_ready() &&
            !consumer_.allow_stage_unready(message)) {
            if (!allowQueue) {
                return ApplyResult::Deferred;
            }
            return enqueue(std::move(message));
        }

        const ApplyResult result = consumer_.consume(message);
        if (result == ApplyResult::Deferred && allowQueue) {
            return enqueue(std::move(message));
        }
        record(result);
        return result;
    } catch (const nlohmann::json::exception&) {
        lastError_ = "rejected malformed gameplay message";
        record(ApplyResult::Rejected);
        return ApplyResult::Rejected;
    }
}

ApplyResult ProtocolRouter::enqueue(RoutedMessage message) {
    size_t encodedBytes = 0;
    try {
        encodedBytes = message.peerId.size() + message.payload.dump().size();
    } catch (const nlohmann::json::exception&) {
        lastError_ = "rejected gameplay message that could not be retained";
        record(ApplyResult::Rejected);
        return ApplyResult::Rejected;
    }
    if (pending_.size() >= kMaxPendingMessages ||
        encodedBytes > kMaxPendingBytes ||
        pendingBytes_ > kMaxPendingBytes - encodedBytes) {
        lastError_ = "gameplay deferral queue limit reached";
        fatalError_ = true;
        record(ApplyResult::Rejected);
        return ApplyResult::Rejected;
    }

    pendingBytes_ += encodedBytes;
    pending_.push_back(Pending{std::move(message), encodedBytes});
    record(ApplyResult::Deferred);
    return ApplyResult::Deferred;
}

void ProtocolRouter::flush(bool syncFlagsEnabled) {
    if (pending_.empty()) {
        return;
    }

    const size_t attempts = pending_.size();
    for (size_t index = 0; index < attempts && !pending_.empty(); ++index) {
        Pending pending = std::move(pending_.front());
        pending_.pop_front();
        pendingBytes_ -= pending.encodedBytes;
        // Keep the retained copy intact if the consumer still cannot apply
        // it. route_message takes by value so applied messages remain isolated
        // from the queue while a Deferred result can be put back verbatim.
        const ApplyResult result =
            route_message(pending.message, syncFlagsEnabled, false);
        if (result == ApplyResult::Deferred) {
            pendingBytes_ += pending.encodedBytes;
            pending_.push_back(std::move(pending));
        }
    }
    stats_.pendingMessages = pending_.size();
    stats_.pendingBytes = pendingBytes_;
}

void ProtocolRouter::clear() {
    pending_.clear();
    pendingBytes_ = 0;
    stats_.pendingMessages = 0;
    stats_.pendingBytes = 0;
    lastError_.clear();
    fatalError_ = false;
}

RouterStats ProtocolRouter::stats() const {
    RouterStats result = stats_;
    result.pendingMessages = pending_.size();
    result.pendingBytes = pendingBytes_;
    return result;
}

const std::string& ProtocolRouter::last_error() const {
    return lastError_;
}

bool ProtocolRouter::fatal_error() const {
    return fatalError_;
}

void ProtocolRouter::record(ApplyResult result) {
    switch (result) {
    case ApplyResult::Applied: ++stats_.applied; break;
    case ApplyResult::Deferred: ++stats_.deferred; break;
    case ApplyResult::Retained: ++stats_.deferred; break;
    case ApplyResult::IgnoredByPolicy: ++stats_.ignored; break;
    case ApplyResult::Unsupported: ++stats_.unsupported; break;
    case ApplyResult::Rejected: ++stats_.rejected; break;
    }
}

}  // namespace dusklight_online::game
