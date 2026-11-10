#pragma once

#include "dusklight_online/game/ooccoo_sync.hpp"
#include <nlohmann/json.hpp>
#include <optional>

namespace dusklight_online::game::ooccoo {

inline nlohmann::json encode(Progress state) {
    nlohmann::json anchors = nlohmann::json::array();
    for (int stage = FirstDungeon; stage <= CityDungeon; ++stage) {
        const auto& anchor = state.anchors[stage - FirstDungeon];
        if (!valid_anchor(stage, anchor) || !(state.acquired & dungeon_bit(stage))) {
            anchors.push_back(nullptr);
        } else {
            anchors.push_back({{"variant", anchor.variant}, {"room", anchor.room},
                {"x", anchor.x}, {"y", anchor.y}, {"z", anchor.z}, {"angle", anchor.angle}});
        }
    }
    return {{"version", 4}, {"acquired", state.acquired}, {"completed", state.completed},
            {"city_special", state.citySpecial}, {"unbound_note", state.unboundNote},
            {"anchors", std::move(anchors)}};
}

// Range-check in the JSON's original width BEFORE narrowing. Reject partial,
// malformed and unbounded anchor payloads atomically; don't treat them as clears.
inline std::optional<unsigned> bounded_integer(const nlohmann::json& value, unsigned max) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number <= max) return static_cast<unsigned>(number);
    } else if (value.is_number_integer()) {
        const auto number = value.get<int64_t>();
        if (number >= 0 && static_cast<uint64_t>(number) <= max)
            return static_cast<unsigned>(number);
    }
    return std::nullopt;
}

inline std::optional<Progress> decode(const nlohmann::json& state) {
    if (!state.is_object() || !state.contains("version") ||
        !state.contains("acquired") || !state.contains("completed") ||
        !state.contains("city_special") || !state.contains("unbound_note")) return std::nullopt;
    const auto version = bounded_integer(state["version"], 4);
    const auto acquired = bounded_integer(state["acquired"], DungeonMask);
    const auto completed = bounded_integer(state["completed"], DungeonMask);
    if (!version || (*version != 2 && *version != 3 && *version != 4) ||
        state.size() != (*version >= 3 ? 6 : 5) ||
        (*version >= 3 && !state.contains("anchors")) || !acquired || !completed ||
        !state["city_special"].is_boolean() || !state["unbound_note"].is_boolean()) {
        return std::nullopt;
    }
    Progress result{static_cast<uint8_t>(*acquired), static_cast<uint8_t>(*completed),
                    state["city_special"].get<bool>(), state["unbound_note"].get<bool>()};
    if (*version == 2) return result; // Existing saves remain readable; no invented return mark.
    if (!state["anchors"].is_array() || state["anchors"].size() != result.anchors.size())
        return std::nullopt;
    for (int stage = FirstDungeon; stage <= CityDungeon; ++stage) {
        const auto& encoded = state["anchors"][stage - FirstDungeon];
        if (encoded.is_null()) continue;
        if (!(result.acquired & dungeon_bit(stage)) || !encoded.is_object() ||
            encoded.size() != 6 || !encoded.contains("variant") ||
            !encoded.contains("room") || !encoded.contains("x") ||
            !encoded.contains("y") || !encoded.contains("z") ||
            !encoded.contains("angle")) return std::nullopt;
        const auto variant = bounded_integer(encoded["variant"], 1);
        const auto room = bounded_integer(encoded["room"], 63);
        std::optional<int16_t> angle;
        if (encoded["angle"].is_number_unsigned()) {
            const auto value = encoded["angle"].get<uint64_t>();
            if (value <= 32767) angle = static_cast<int16_t>(value);
        } else if (encoded["angle"].is_number_integer()) {
            const auto value = encoded["angle"].get<int64_t>();
            if (value >= -32768 && value <= 32767) angle = static_cast<int16_t>(value);
        }
        if (!variant || !room || !angle ||
            !encoded["x"].is_number() || !encoded["y"].is_number() ||
            !encoded["z"].is_number()) return std::nullopt;
        ReturnAnchor anchor{true, static_cast<uint8_t>(*variant), static_cast<uint8_t>(*room),
            encoded["x"].get<float>(), encoded["y"].get<float>(),
            encoded["z"].get<float>(), *angle};
        if (!valid_anchor(stage, anchor)) return std::nullopt;
        result.anchors[stage - FirstDungeon] = anchor;
    }
    // The short-lived v3 build captured an anchor at pickup, before any warp.
    // Keep its receipts but discard those points so they cannot mint Jr early.
    if (*version == 3) result.anchors = {};
    return result;
}

} // namespace dusklight_online::game::ooccoo
