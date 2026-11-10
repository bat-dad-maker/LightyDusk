#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <compare>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <tuple>

namespace dusklight_online::game::floor_switch {

// Profiles from the game's f_pc_name.h, not runtime actor instance IDs.
inline constexpr int Push = 0x16;
inline constexpr int Iron = 0x18;
inline constexpr int Heavy = 0x3D;
inline constexpr std::size_t MaxSwitches = 64;
inline constexpr std::size_t MaxPeers = 64;
inline constexpr uint64_t HeartbeatMs = 250;
inline constexpr uint64_t LeaseMs = 2000;

struct Scene {
    int table = -1;
    std::string stage;
    int room = -1;
    int layer = -1;
    auto operator<=>(const Scene&) const = default;
};

struct Key {
    int actor = -1;
    uint32_t params = 0;
    // Immutable placement coordinates, in quarter units. Current position,
    // animation height, pointers, and process IDs are not network identities.
    std::array<int32_t, 3> home{};
    uint16_t placementZ = 0;
    auto operator<=>(const Key&) const = default;
};

struct Pressure {
    uint8_t ride = 0; // Native ride class: none, ordinary, carry-type-1.
    bool heavy = false;
    bool active() const { return ride != 0; }
    bool operator==(const Pressure&) const = default;
};

inline Pressure combine(Pressure a, Pressure b) {
    return {std::max(a.ride, b.ride), a.heavy || b.heavy};
}

inline bool supported(int actor, uint32_t params) {
    if (actor == Iron || actor == Heavy) return true;
    if (actor != Push) return false;
    const unsigned type = (params >> 24) & 7;
    // The placed Kbota_A, Kbota_B, and Lv3bota variants. Do not silently
    // enable the unused/inverted/toggle switches or obj_swpush2.
    return type == 0 || type == 1 || type == 4;
}

inline int output_flag(int actor, uint32_t params) {
    if (!supported(actor, params)) return -1;
    return actor == Push ? int((params >> 8) & 0xFF) : int(params & 0xFF);
}

inline bool momentary(int actor, uint32_t params) {
    return actor == Push && supported(actor, params) && ((params >> 24) & 7) != 0;
}

inline bool valid_scene(const Scene& scene) {
    if (scene.table < 0 || scene.table >= 32 || scene.room < 0 || scene.room >= 64 ||
        scene.layer < -1 || scene.layer > 15 || scene.stage.empty() || scene.stage.size() > 8)
        return false;
    for (unsigned char c : scene.stage)
        if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

inline bool valid_key(const Key& key) {
    if (!supported(key.actor, key.params)) return false;
    if (key.actor != Push && key.placementZ != 0) return false;
    for (int32_t coordinate : key.home)
        if (coordinate < -4000000 || coordinate > 4000000) return false;
    return true;
}

using Contacts = std::map<Key, Pressure>;
struct Packet {
    uint64_t sequence = 0;
    Scene scene;
    Contacts contacts; // Complete sender-local state, never an OR of peers.
};

inline bool valid_packet(const Packet& packet) {
    if (packet.sequence == 0 || !valid_scene(packet.scene) || packet.contacts.size() > MaxSwitches)
        return false;
    for (const auto& [key, pressure] : packet.contacts)
        if (!valid_key(key) || pressure.ride < 1 || pressure.ride > 2 ||
            (key.actor == Heavy && pressure.ride != 1)) return false;
    return true;
}

// The fpcM_Execute pre-observer feeds this only BEFORE adding received pressure.
// Missing actors release when simulation is running; a genuinely paused,
// unexecuted actor retains its last observation (not an invented contact).
inline Contacts finish_frame(Contacts previous, const Contacts& sampled, bool paused) {
    if (!paused) previous.clear();
    for (const auto& [key, pressure] : sampled) {
        if (!pressure.active()) previous.erase(key);
        else if (previous.contains(key) || previous.size() < MaxSwitches) previous[key] = pressure;
    }
    return previous;
}

class Ledger {
public:
    enum class Result { Accepted, Stale, Invalid, Full };

    Result receive(std::string_view peer, const Packet& packet, uint64_t now) {
        if (peer.empty() || peer.size() > 128 || !valid_packet(packet)) return Result::Invalid;
        auto it = peers_.find(std::string(peer));
        if (it != peers_.end() && packet.sequence <= it->second.sequence) return Result::Stale;
        if (it == peers_.end() && peers_.size() >= MaxPeers) return Result::Full;
        peers_[std::string(peer)] = {packet.sequence, packet.scene, packet.contacts, now};
        return Result::Accepted;
    }

    Pressure pressure(const Scene& scene, const Key& key, uint64_t now) const {
        Pressure result;
        for (const auto& [peer, state] : peers_) {
            (void)peer;
            if (state.scene != scene || now < state.received || now - state.received >= LeaseMs)
                continue;
            const auto it = state.contacts.find(key);
            if (it != state.contacts.end()) result = combine(result, it->second);
        }
        return result;
    }

    // Keep sequence high-water marks across save/room/settings changes. A
    // delayed older press must not resurrect pressure after a newer release.
    void clear_pressure() {
        for (auto& [peer, state] : peers_) { (void)peer; state.contacts.clear(); }
    }
    void forget(std::string_view peer) { peers_.erase(std::string(peer)); }
    void reset_session() { peers_.clear(); }
    std::size_t peer_count() const { return peers_.size(); }

private:
    struct Peer {
        uint64_t sequence;
        Scene scene;
        Contacts contacts;
        uint64_t received;
    };
    std::map<std::string, Peer> peers_;
};

} // namespace dusklight_online::game::floor_switch
