#pragma once

#include "dusklight_online/game/floor_switch_sync.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <optional>

namespace dusklight_online::game::floor_switch {

// Reject floating-point values, booleans, out-of-range values, and overflows before
// narrowing. Malformed snapshots never partially clear a sender's contacts.
inline std::optional<int64_t> integer(const nlohmann::json& value, int64_t low, int64_t high) {
    if (!value.is_number_integer()) return std::nullopt;
    if (value.is_number_unsigned()) {
        const auto n = value.get<uint64_t>();
        if (n > uint64_t(high)) return std::nullopt;
        const auto v = static_cast<int64_t>(n);
        return v >= low ? std::optional<int64_t>(v) : std::nullopt;
    }
    const auto n = value.get<int64_t>();
    return n >= low && n <= high ? std::optional<int64_t>(n) : std::nullopt;
}

inline nlohmann::json encode(const Packet& packet) {
    auto contacts = nlohmann::json::array();
    for (const auto& [key, pressure] : packet.contacts)
        contacts.push_back({{"actor", key.actor}, {"params", key.params}, {"home", key.home},
                            {"placement_z", key.placementZ}, {"ride", pressure.ride},
                            {"heavy", pressure.heavy}});
    return {{"type", "floor_switch_state"}, {"version", 1}, {"sequence", packet.sequence},
            {"stage", packet.scene.table}, {"source_stage", packet.scene.stage},
            {"room", packet.scene.room}, {"layer", packet.scene.layer},
            {"contacts", std::move(contacts)}};
}

inline std::optional<Packet> decode(const nlohmann::json& message) {
    if (!message.is_object()) return std::nullopt;
    for (const char* field : {"type", "version", "sequence", "stage", "source_stage",
                              "room", "layer", "contacts"})
        if (!message.contains(field)) return std::nullopt;
    if (message["type"] != "floor_switch_state" || integer(message["version"], 1, 1) != 1 ||
        !message["source_stage"].is_string() || !message["contacts"].is_array() ||
        message["contacts"].size() > MaxSwitches) return std::nullopt;
    const auto& name = message["source_stage"].get_ref<const std::string&>();
    if (name.empty() || name.size() > 8) return std::nullopt;
    const auto sequence = integer(message["sequence"], 1, std::numeric_limits<int64_t>::max());
    const auto stage = integer(message["stage"], 0, 31);
    const auto room = integer(message["room"], 0, 63);
    const auto layer = integer(message["layer"], -1, 15);
    if (!sequence || !stage || !room || !layer) return std::nullopt;
    Packet result{uint64_t(*sequence), {int(*stage), message["source_stage"].get<std::string>(),
                                      int(*room), int(*layer)}, {}};
    for (const auto& entry : message["contacts"]) {
        if (!entry.is_object() || entry.size() != 6) return std::nullopt;
        for (const char* field : {"actor", "params", "home", "placement_z", "ride", "heavy"})
            if (!entry.contains(field)) return std::nullopt;
        const auto actor = integer(entry["actor"], 0, 0x7FFF);
        const auto params = integer(entry["params"], 0, 0xFFFFFFFFLL);
        const auto placement = integer(entry["placement_z"], 0, 0xFFFF);
        const auto ride = integer(entry["ride"], 1, 2);
        if (!actor || !params || !placement || !ride || !entry["heavy"].is_boolean() ||
            !entry["home"].is_array() || entry["home"].size() != 3) return std::nullopt;
        Key key{int(*actor), uint32_t(*params), {}, uint16_t(*placement)};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto coordinate = integer(entry["home"][i], -4000000, 4000000);
            if (!coordinate) return std::nullopt;
            key.home[i] = int32_t(*coordinate);
        }
        if (!result.contacts.emplace(key, Pressure{uint8_t(*ride), entry["heavy"].get<bool>()}).second)
            return std::nullopt;
    }
    return valid_packet(result) ? std::optional<Packet>(std::move(result)) : std::nullopt;
}

} // namespace dusklight_online::game::floor_switch
