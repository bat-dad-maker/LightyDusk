#include "dusklight_online/game/protocol_router.hpp"
#include "dusklight_online/game/chat.hpp"
#include "dusklight_online/net/peer_delivery.hpp"
#include "cave_map_packet.hpp"

#include <cassert>
#include <array>
#include <string>
#include <vector>

using dusklight_online::game::ApplyResult;
using dusklight_online::game::MessageConsumer;
using dusklight_online::game::MessageDomain;
using dusklight_online::game::ProtocolRouter;
using dusklight_online::game::RoutedMessage;
using dusklight_online::net::Event;
using dusklight_online::net::EventKind;

namespace {

class Consumer final : public MessageConsumer {
public:
    enum class MalformedHook { None, Retain, Discard, Allow, Udp };
    bool ready = false;
    bool retainForSave = false;
    MalformedHook malformedHook = MalformedHook::None;
    std::vector<std::string> types;
    std::vector<nlohmann::json> payloads;
    std::vector<std::chrono::steady_clock::time_point> receivedAt;
    std::vector<RoutedMessage> saveMessages;

    bool stage_ready() const override { return ready; }
    bool retain_for_save_recovery(const RoutedMessage& message) override {
        if (malformedHook == MalformedHook::Retain)
            (void)message.payload.value("manual_sync", false);
        if (!retainForSave) return false;
        saveMessages.push_back(message);
        return true;
    }
    bool discard_stage_message(const RoutedMessage& message) const override {
        if (malformedHook == MalformedHook::Discard)
            (void)message.payload.value("manual_sync", false);
        return false;
    }
    bool allow_stage_unready(const RoutedMessage& message) const override {
        if (malformedHook == MalformedHook::Allow)
            (void)message.payload.value("manual_sync", false);
        return false;
    }
    ApplyResult consume(const RoutedMessage& message) override {
        types.push_back(message.payload.at("type").get<std::string>());
        payloads.push_back(message.payload);
        receivedAt.push_back(message.receivedAt);
        return ApplyResult::Applied;
    }
    ApplyResult consume_udp(const Event& event) override {
        if (malformedHook == MalformedHook::Udp)
            (void)event.message.value("manual_sync", false);
        return ApplyResult::Applied;
    }
    void peer_joined(std::string_view, std::string_view) override {}
    void peer_left(std::string_view) override {}
};

Event message(std::string type, bool syncFlags = true) {
    Event event;
    event.kind = EventKind::Message;
    event.peerId = "peer";
    event.message = {{"type", std::move(type)}};
    event.ingress.settings.syncFlags = syncFlags;
    return event;
}

float monospace_chat_width(std::string_view text) {
    return static_cast<float>(text.size());
}

}  // namespace

int main() {
    using Domain = MessageDomain;
    struct Expected { const char* type; Domain domain; bool stageDependent; bool syncFlagGated; };
    constexpr Expected expected[] = {
        Expected{"welcome",Domain::Session,false,false}, {"error",Domain::Session,false,false},
        {"ping",Domain::Session,false,false}, {"pong",Domain::Session,false,false},
        {"ack",Domain::Session,false,false}, {"udp_ready",Domain::Session,false,false},
        {"peer_joined",Domain::Membership,false,false}, {"peer_left",Domain::Membership,false,false},
        {"owner_changed",Domain::Membership,false,false}, {"room_settings",Domain::Membership,false,false},
        {"name_labels",Domain::Membership,false,false}, {"dummy_model",Domain::Membership,false,false},
        {"sync_flags",Domain::Membership,false,false}, {"sync_world",Domain::Membership,false,false},
        {"remote_collision",Domain::Membership,false,false}, {"pvp_enabled",Domain::Membership,false,false},
        {"voice_settings",Domain::Membership,false,false},
        {"save_recovery_setting",Domain::Membership,false,false},
        {"presence",Domain::Presence,false,false}, {"progression_state",Domain::Presence,false,true},
        {"puppet_preference",Domain::Presence,false,false}, {"midna_preference",Domain::Presence,false,false},
        {"chat",Domain::Chat,false,false},
        {"sync_request",Domain::Progression,false,false}, {"save_snapshot",Domain::Progression,true,true},
        {"event_bit",Domain::Progression,false,true}, {"tbox_bit",Domain::Progression,true,true},
        {"switch_bit",Domain::Progression,true,true}, {"room_switch_bit",Domain::Progression,true,true},
        {"web_timer",Domain::Progression,true,true}, {"room_actor_action",Domain::Progression,false,true},
        {"floor_switch_state",Domain::Progression,false,true},
        {"item_bit",Domain::Progression,true,true}, {"dungeon_item_bit",Domain::Progression,true,true},
        {"item_get",Domain::Progression,false,true}, {"trade_item",Domain::Progression,true,true},
        {"item_first_bit",Domain::Progression,false,true},
        {"collect_crystal",Domain::Progression,false,true}, {"collect_mirror",Domain::Progression,false,true},
        {"dark_clear_lv",Domain::Progression,true,true}, {"transform_lv",Domain::Progression,false,true},
        {"region_bit",Domain::Progression,false,true}, {"collect",Domain::Progression,false,true},
        {"visited_room",Domain::Progression,true,true}, {"letter_get",Domain::Progression,false,true},
        {"ooccoo_state",Domain::Progression,false,true}, {"collect_smell",Domain::Progression,false,true},
        {"key_num",Domain::Progression,true,true}, {"light_drop_num",Domain::Progression,false,true},
        {"light_drop_get_flag",Domain::Progression,false,true}, {"max_life_update",Domain::Progression,false,true},
        {"bottle_slots",Domain::Progression,false,true}, {"bomb_bag_slot",Domain::Progression,false,true},
        {"rupee_count",Domain::Progression,false,true}, {"rupee_delta",Domain::Progression,false,true},
        {"poe_count",Domain::Progression,false,true},
        {"malo_fundraising",Domain::Progression,false,true}, {"charlo_offering",Domain::Progression,false,true},
        {"fish_record",Domain::Progression,false,true},
        {"rando_item_get",Domain::OptionalRandomizer,false,true},
        {"pose",Domain::Visual,false,false}, {"midna_pose",Domain::Visual,false,false},
        {"pvp_hit",Domain::Interaction,false,false},
        {"coop_presence",Domain::Interaction,false,false},
        {"coop_settings",Domain::Interaction,false,false},
        {"coop_enemy_damage",Domain::Interaction,false,false},
        {"ganondorf_owner_claim",Domain::Ganondorf,false,true}, {"ganondorf_owner",Domain::Ganondorf,false,true},
        {"ganondorf_hit",Domain::Ganondorf,false,true}, {"ganondorf_reaction",Domain::Ganondorf,false,true},
        {"ganondorf_player_damage",Domain::Ganondorf,false,true}, {"ganondorf_state",Domain::Ganondorf,false,true},
    };
    for (const auto& item : expected) {
        const auto spec = ProtocolRouter::classify(item.type);
        assert(spec.domain == item.domain);
        assert(spec.stageDependent == item.stageDependent);
        assert(spec.syncFlagsControlled == item.syncFlagGated);
        assert(ProtocolRouter::is_known_type(item.type));
        if (item.domain == Domain::Progression || item.domain == Domain::OptionalRandomizer ||
            item.domain == Domain::Interaction || item.domain == Domain::Ganondorf ||
            item.domain == Domain::Chat || std::string_view(item.type) == "progression_state") {
            assert(dusklight_online::net::peer_delivery_type(item.type));
        }
    }
    assert(ProtocolRouter::classify("save_snapshot").domain == MessageDomain::Progression);
    assert(ProtocolRouter::classify("save_snapshot").stageDependent);
    assert(ProtocolRouter::classify("rando_item_get").domain ==
           MessageDomain::OptionalRandomizer);
    assert(!ProtocolRouter::classify("rando_item_get").stageDependent);
    assert(ProtocolRouter::classify("ganondorf_state").domain == MessageDomain::Ganondorf);
    assert(!ProtocolRouter::is_known_type("future_unreviewed_lane"));
    assert(dusklight_online::net::peer_delivery_type("chat"));

    std::string normalizedChat;
    assert(dusklight_online::game::normalize_chat_text("  hello world  ", normalizedChat));
    assert(normalizedChat == "hello world");
    assert(dusklight_online::game::normalize_chat_text("Twilight \xf0\x9f\x90\xba", normalizedChat));
    assert(!dusklight_online::game::normalize_chat_text("   ", normalizedChat));
    assert(dusklight_online::game::normalize_chat_text("\n  line one\nline two  \n", normalizedChat));
    assert(normalizedChat == "line one\nline two");
    assert(!dusklight_online::game::normalize_chat_text(
        std::string(dusklight_online::game::kMaxChatLineCodepoints + 1, 'x'), normalizedChat));
    assert(!dusklight_online::game::normalize_chat_text("1\n2\n3\n4\n5", normalizedChat));
    assert(!dusklight_online::game::normalize_chat_text("\xc0\x80", normalizedChat));

    const auto wrappedChat = dusklight_online::game::wrap_chat_input(
        std::string(dusklight_online::game::kMaxChatLineCodepoints + 1, 'x'),
        dusklight_online::game::kMaxChatLineCodepoints + 1);
    assert(wrappedChat.text ==
        std::string(dusklight_online::game::kMaxChatLineCodepoints, 'x') + "\nx");
    assert(wrappedChat.cursorByte == wrappedChat.text.size());

    const auto wrappedWord = dusklight_online::game::wrap_chat_input(
        std::string(48, 'x') + " hello", 54);
    assert(wrappedWord.text == std::string(48, 'x') + "\nhello");
    assert(wrappedWord.cursorByte == wrappedWord.text.size());
    const auto wrappedSpaces = dusklight_online::game::wrap_chat_input(
        std::string(48, 'x') + "  hello", 49);
    assert(wrappedSpaces.text == std::string(48, 'x') + "\nhello");
    assert(wrappedSpaces.cursorByte == 49);
    const auto reflowedWord = dusklight_online::game::reflow_chat_input(
        std::string(44, 'x') + "\nhello", 50,
        wrappedWord.text, wrappedWord.autoBreaks);
    assert(reflowedWord.text == std::string(44, 'x') + " hello");
    assert(reflowedWord.cursorByte == reflowedWord.text.size());
    const auto reflowedSpaces = dusklight_online::game::reflow_chat_input(
        std::string(43, 'x') + "\nhello", 49,
        wrappedSpaces.text, wrappedSpaces.autoBreaks);
    assert(reflowedSpaces.text == std::string(43, 'x') + "  hello");
    const auto wrappedManualAndAuto = dusklight_online::game::wrap_chat_input(
        "first\n" + std::string(48, 'x') + " hello", 60);
    const auto reflowedManualAndAuto = dusklight_online::game::reflow_chat_input(
        "first\n" + std::string(44, 'x') + "\nhello", 56,
        wrappedManualAndAuto.text, wrappedManualAndAuto.autoBreaks);
    assert(reflowedManualAndAuto.text ==
        "first\n" + std::string(44, 'x') + " hello");
    const auto splitLongWord = dusklight_online::game::wrap_chat_input(
        std::string(51, 'x'), 51);
    assert(splitLongWord.text == std::string(50, 'x') + "\nx");
    const auto reflowedLongWord = dusklight_online::game::reflow_chat_input(
        std::string(49, 'x') + "\nx", 51,
        splitLongWord.text, splitLongWord.autoBreaks);
    assert(reflowedLongWord.text == std::string(50, 'x'));
    assert(dusklight_online::game::normalize_chat_text(wrappedWord.text, normalizedChat));
    const auto fittingWord = dusklight_online::game::wrap_chat_input(
        std::string(47, 'x') + " hi", 50);
    assert(fittingWord.text == std::string(47, 'x') + " hi");
    const auto manualBreak = dusklight_online::game::wrap_chat_input(
        std::string(49, 'x') + "  \nhello", 57);
    assert(manualBreak.text == std::string(49, 'x') + "\nhello");
    const auto wrappedUnicode = dusklight_online::game::wrap_chat_input(
        std::string(49, 'x') + " \xf0\x9f\x90\xba", 54);
    assert(wrappedUnicode.text == std::string(49, 'x') + "\n\xf0\x9f\x90\xba");
    const auto pixelWrapped = dusklight_online::game::wrap_chat_input(
        "abcdefgh ijk", 12, 10.0f, &monospace_chat_width);
    assert(pixelWrapped.text == "abcdefgh\nijk");
    const auto pixelReflowed = dusklight_online::game::reflow_chat_input(
        "abcdef\nijk", 10, pixelWrapped.text, pixelWrapped.autoBreaks,
        10.0f, &monospace_chat_width);
    assert(pixelReflowed.text == "abcdef ijk");
    const auto pixelSplitLongWord = dusklight_online::game::wrap_chat_input(
        "abcdefghijk", 11, 10.0f, &monospace_chat_width);
    assert(pixelSplitLongWord.text == "abcdefghij\nk");
    const auto limitedLines = dusklight_online::game::wrap_chat_input(
        std::string(50, 'x') + "\n" + std::string(50, 'x') + "\n" +
        std::string(50, 'x') + "\n" + std::string(48, 'x') + " hello", 211);
    assert(limitedLines.text == std::string(50, 'x') + "\n" +
        std::string(50, 'x') + "\n" + std::string(50, 'x') + "\n" +
        std::string(48, 'x'));

    // Latest pressure is metadata, never stage-deferred replay. A release
    // arriving during a cutscene must replace the hold immediately.
    {
        Consumer floor;
        ProtocolRouter pressureRouter(floor);
        auto press = message("floor_switch_state");
        press.message["sequence"] = 1;
        auto release = press;
        release.message["sequence"] = 2;
        assert(pressureRouter.route(press, true) == ApplyResult::Applied);
        assert(pressureRouter.route(release, true) == ApplyResult::Applied);
        assert(pressureRouter.stats().pendingMessages == 0);
        assert(floor.payloads.size() == 2);
        assert(floor.payloads.back()["sequence"] == 2);
        assert(pressureRouter.route(message("floor_switch_state", false), true) ==
               ApplyResult::IgnoredByPolicy);
        // Ingress settings, not the batch's final setting, own routing.
        assert(pressureRouter.route(message("floor_switch_state", true), false) ==
               ApplyResult::Applied);
        floor.ready = true;
        pressureRouter.flush(true);
        assert(floor.payloads.size() == 3);
    }

    Consumer consumer;
    ProtocolRouter router(consumer);
    const auto beforeReceive = std::chrono::steady_clock::now();
    assert(router.route(message("save_snapshot"), true) == ApplyResult::Deferred);
    const auto afterReceive = std::chrono::steady_clock::now();
    assert(router.stats().pendingMessages == 1);
    consumer.ready = true;
    router.flush(true);
    assert(router.stats().pendingMessages == 0);
    assert(consumer.types.size() == 1 && consumer.types.front() == "save_snapshot");
    assert(consumer.receivedAt.front() >= beforeReceive);
    assert(consumer.receivedAt.front() <= afterReceive);

    // A saved player at title receives both stage-dependent and immediate
    // progression while the normal stage is unavailable. Neither may mutate
    // the title save or sit in the generic stage queue.
    {
        Consumer savedAtTitle;
        savedAtTitle.retainForSave = true;
        ProtocolRouter titleRouter(savedAtTitle);
        assert(titleRouter.route(message("item_get"), true) == ApplyResult::Retained);
        assert(titleRouter.route(message("switch_bit"), true) == ApplyResult::Retained);
        assert(titleRouter.stats().pendingMessages == 0);
        assert(savedAtTitle.types.empty());
        assert(savedAtTitle.saveMessages.size() == 2);
        savedAtTitle.retainForSave = false;
        savedAtTitle.ready = true;
        for (const RoutedMessage& saved : savedAtTitle.saveMessages)
            assert(savedAtTitle.consume(saved) == ApplyResult::Applied);
        assert(savedAtTitle.types.size() == 2);
    }
    {
        Consumer transitioningToTitle;
        ProtocolRouter titleRouter(transitioningToTitle);
        assert(titleRouter.route(message("switch_bit"), true) == ApplyResult::Deferred);
        transitioningToTitle.retainForSave = true;
        titleRouter.flush(true);
        assert(titleRouter.stats().pendingMessages == 0);
        assert(transitioningToTitle.types.empty());
        assert(transitioningToTitle.saveMessages.size() == 1);
    }

    // Match the AIO: a resolved randomizer reward is never queued behind its
    // non-stage-dependent absolute companion counter.
    consumer.ready = false;
    assert(router.route(message("rando_item_get"), true) == ApplyResult::Applied);
    assert(router.stats().pendingMessages == 0);
    assert(consumer.types.size() == 2 && consumer.types.back() == "rando_item_get");

    assert(router.route(message("event_bit", false), false) == ApplyResult::IgnoredByPolicy);
    assert(consumer.types.size() == 2);

    // Cave-section switch messages need the normal stage-readiness fence and retain
    // the source metadata used by the adapter's narrow trigger exception.
    Event cave = message("switch_bit");
    cave.message = cave_map_packet();
    assert(router.route(cave, true) == ApplyResult::Deferred);
    assert(router.stats().pendingMessages == 1);
    consumer.ready = true;
    router.flush(true);
    assert(router.stats().pendingMessages == 0);
    assert(consumer.types.back() == "switch_bit");
    assert(consumer.payloads.back() == cave.message);
    cave.ingress.settings.syncFlags = false;
    assert(router.route(cave, false) == ApplyResult::IgnoredByPolicy);
    assert(router.route(message("future_unreviewed_lane"), true) == ApplyResult::Unsupported);

    Event malformed;
    malformed.kind = EventKind::Message;
    malformed.message = nullptr;
    assert(router.route(malformed, true) == ApplyResult::Rejected);
    // A joined peer can set a known field to the wrong JSON type. The save
    // recovery and stage hooks run before GameAdapter::consume's own catch.
    for (const auto hook : {Consumer::MalformedHook::Retain,
                            Consumer::MalformedHook::Discard,
                            Consumer::MalformedHook::Allow}) {
        Consumer adversarial;
        adversarial.malformedHook = hook;
        ProtocolRouter guarded(adversarial);
        Event invalidSnapshot = message("save_snapshot");
        invalidSnapshot.message["manual_sync"] = "not a boolean";
        assert(guarded.route(invalidSnapshot, true) == ApplyResult::Rejected);
        assert(guarded.stats().pendingMessages == 0);
        assert(adversarial.types.empty());
    }
    Consumer visual;
    visual.malformedHook = Consumer::MalformedHook::Udp;
    ProtocolRouter visualRouter(visual);
    Event invalidVisual;
    invalidVisual.kind = EventKind::UdpMessage;
    invalidVisual.message = {{"manual_sync", "not a boolean"}};
    assert(visualRouter.route(invalidVisual, true) == ApplyResult::Rejected);
    return 0;
}
