#pragma once
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::net {
// Relay-issued client_N identities are represented without string padding.
// 20-byte envelope + 1200-byte payload + IPv6/UDP fits the 1280-byte minimum MTU.
inline uint64_t peer_number(std::string_view id) {
    if (!id.starts_with("client_")) return 0;
    id.remove_prefix(7);
    uint64_t result = 0;
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == id.data() + id.size() ? result : 0;
}
inline uint64_t tunnel_read(std::span<const uint8_t> bytes) {
    uint64_t result = 0;
    for (size_t i = 0; i < 8; ++i) result |= uint64_t(bytes[i]) << (i * 8);
    return result;
}
inline bool is_peer_tunnel(std::span<const uint8_t> bytes) {
    return bytes.size() > 20 && bytes.size() <= 1220 &&
        bytes[0] == 'D' && bytes[1] == 'P' && bytes[2] == 'F' && bytes[3] == '1' &&
        tunnel_read(bytes.subspan(4, 8)) && tunnel_read(bytes.subspan(12, 8));
}
inline std::vector<uint8_t> peer_tunnel(uint64_t sender, uint64_t target, std::span<const uint8_t> payload) {
    if (!sender || !target || payload.empty() || payload.size() > 1200) return {};
    std::vector<uint8_t> bytes{'D','P','F','1'};
    for (const auto id : {sender, target})
        for (size_t i = 0; i < 8; ++i) bytes.push_back(uint8_t(id >> (i * 8)));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
// Group only realtime fallback: preserve one upload for multiple recipients.
// At most 69 bytes of header + 1158-byte visual + 48 IPv6/UDP bytes = 1275.
inline size_t peer_group_header(std::span<const uint8_t> bytes) {
    if (bytes.size() < 21 || bytes.size() > 1227 || bytes[0] != 'D' || bytes[1] != 'P' ||
        bytes[2] != 'G' || bytes[3] != '1' || !bytes[12] || bytes[12] > 7) return 0;
    const size_t header = 13 + bytes[12] * 8;
    return bytes.size() >= header + 58 && bytes.size() <= header + 1158 ? header : 0;
}
inline std::vector<uint8_t> peer_group(uint64_t sender, std::span<const uint64_t> targets, std::span<const uint8_t> payload) {
    if (!sender || targets.empty() || targets.size() > 7 || payload.size() < 58 || payload.size() > 1158) return {};
    std::vector<uint8_t> bytes{'D','P','G','1'};
    for (size_t i = 0; i < 8; ++i) bytes.push_back(uint8_t(sender >> (i * 8)));
    bytes.push_back(uint8_t(targets.size()));
    for (auto id : targets) for (size_t i = 0; i < 8; ++i) bytes.push_back(uint8_t(id >> (i * 8)));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}
}
