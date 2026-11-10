#pragma once

#include <compare>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace dusklight_online::game {

// A save recovery record describes final save facts, never network packets.
// The baseline is the successful save on disk; each accepted remote mutation
// replaces only facts it actually changed. This keeps unrelated local changes
// out of the journal and coalesces repeated updates to one final value.
struct RecoveryFact {
    std::string field;
    int key = -1;
    int subkey = -1;
    auto operator<=>(const RecoveryFact&) const = default;
};

using RecoveryFacts = std::map<RecoveryFact, nlohmann::json>;

inline RecoveryFacts recovery_facts(const nlohmann::json& snapshot) {
    using nlohmann::json;
    RecoveryFacts facts;
    constexpr std::string_view sets[] = {
        "event_flags", "key_items", "crystals", "mirrors", "dark_clear_levels",
        "transform_levels", "region_bits", "collect_clothing", "collect_sword",
        "collect_shield", "letter_get_flags", "light_drop_get_flags",
        "item_first_bits", "visited_rooms",
    };
    for (const auto field : sets) {
        const auto it = snapshot.find(std::string(field));
        if (it == snapshot.end() || !it->is_array()) continue;
        for (const auto& value : *it) {
            if (value.is_number_integer())
                facts[{std::string(field), value.get<int>(), -1}] = true;
            else if (field == "visited_rooms" && value.is_object()) {
                const int stage = value.value("stage", -1), room = value.value("room", -1);
                if (stage >= 0 && room >= 0) facts[{std::string(field), stage, room}] = true;
            }
        }
    }
    constexpr std::string_view stageSets[] = {"chests", "switches", "items", "dungeon_items"};
    for (const auto field : stageSets) {
        const auto it = snapshot.find(std::string(field));
        if (it == snapshot.end() || !it->is_array()) continue;
        const char* member = field == "dungeon_items" ? "kinds" : "flags";
        for (const auto& entry : *it) {
            if (!entry.is_object()) continue;
            const int stage = entry.value("stage", -1);
            if (stage < 0 || !entry.contains(member) || !entry[member].is_array()) continue;
            for (const auto& value : entry[member])
                if (value.is_number_integer())
                    facts[{std::string(field), stage, value.get<int>()}] = true;
        }
    }
    constexpr std::pair<std::string_view, std::string_view> indexed[] = {
        {"key_counts", "stage"}, {"light_drop_counts", "area"},
        {"bomb_bag_slots", "bag"}, {"fish_records", "index"},
    };
    for (const auto& [field, index] : indexed) {
        const auto it = snapshot.find(std::string(field));
        if (it == snapshot.end() || !it->is_array()) continue;
        for (const auto& entry : *it) {
            if (!entry.is_object()) continue;
            const int key = entry.value(std::string(index), -1);
            if (key >= 0) facts[{std::string(field), key, -1}] = entry;
        }
    }
    constexpr std::string_view values[] = {
        "max_life", "bottle_slots", "bottle_sources", "bottle_sources_complete",
        "rupees", "poe_count", "malo_fundraising", "charlo_offering",
        "collect_smell", "trade_item", "ooccoo_state",
    };
    for (const auto field : values) {
        const auto it = snapshot.find(std::string(field));
        if (it != snapshot.end()) facts[{std::string(field), -1, -1}] = *it;
    }
    return facts;
}

class SaveRecoveryDelta {
public:
    void begin(const nlohmann::json& saved) {
        baseline_ = recovery_facts(saved);
        changed_.clear();
    }

    void observe(const nlohmann::json& before, const nlohmann::json& after) {
        const auto previous = recovery_facts(before);
        const auto current = recovery_facts(after);
        std::set<RecoveryFact> keys;
        for (const auto& [key, value] : previous) keys.insert(key);
        for (const auto& [key, value] : current) keys.insert(key);
        for (const auto& key : keys) {
            const auto old = previous.find(key), now = current.find(key);
            const nlohmann::json oldValue = old == previous.end() ? nullptr : old->second;
            const nlohmann::json newValue = now == current.end() ? nullptr : now->second;
            if (oldValue == newValue) continue;
            const auto saved = baseline_.find(key);
            const nlohmann::json savedValue = saved == baseline_.end() ? nullptr : saved->second;
            if (newValue == savedValue) changed_.erase(key);
            else changed_[key] = newValue;
        }
    }

    // Accepted updates may be waiting for a safe room or cutscene boundary.
    // Remember their final value even before they reach native save memory.
    void observe_intent(const RecoveryFact& key, const nlohmann::json& target) {
        const auto saved = baseline_.find(key);
        const nlohmann::json savedValue = saved == baseline_.end() ? nullptr : saved->second;
        if (target == savedValue) changed_.erase(key);
        else changed_[key] = target;
    }

    [[nodiscard]] bool empty() const { return changed_.empty(); }
    [[nodiscard]] size_t size() const { return changed_.size(); }

    // A fresh network update may arrive while the file is loading. Never
    // overwrite a fact that has changed since the successful save.
    [[nodiscard]] RecoveryFacts applicable(const nlohmann::json& loaded) const {
        const auto current = recovery_facts(loaded);
        RecoveryFacts result;
        for (const auto& [key, target] : changed_) {
            const auto saved = baseline_.find(key), now = current.find(key);
            const nlohmann::json savedValue = saved == baseline_.end() ? nullptr : saved->second;
            const nlohmann::json currentValue = now == current.end() ? nullptr : now->second;
            if (currentValue == savedValue && currentValue != target) result[key] = target;
        }
        return result;
    }

private:
    RecoveryFacts baseline_;
    RecoveryFacts changed_;
};

}  // namespace dusklight_online::game
