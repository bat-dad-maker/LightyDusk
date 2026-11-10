#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace dusklight_online::net {
// Small, stateless RFC 8489 Binding responder for server-reflexive discovery.
// It shares the relay socket; ICE connectivity checks remain inside libjuice.
// No TURN, credentials, alternate destinations or peer allocations are exposed.
inline uint32_t stun_be(std::span<const uint8_t> bytes) {
    uint32_t result = 0;
    for (auto b : bytes) result = (result << 8) | b;
    return result;
}
inline uint32_t stun_crc(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffU;
    for (auto b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
    }
    return ~crc ^ 0x5354554eU;
}
inline bool is_stun_binding(std::span<const uint8_t> bytes) {
    return bytes.size() >= 20 && bytes.size() <= 512 && bytes[0] == 0 && bytes[1] == 1 &&
        stun_be(bytes.subspan(4, 4)) == 0x2112a442U &&
        (bytes[3] & 3) == 0 && stun_be(bytes.subspan(2, 2)) == bytes.size() - 20;
}
// Address and port are supplied in host byte order. Only the observed source
// is reflected, never an address supplied in a request attribute.
inline std::vector<uint8_t> stun_binding_reply(std::span<const uint8_t> request,
    uint32_t ipv4, uint16_t port) {
    if (!is_stun_binding(request)) return {};
    for (size_t offset = 20; offset < request.size();) {
        if (request.size() - offset < 4) return {};
        const auto type = stun_be(request.subspan(offset, 2));
        const size_t length = stun_be(request.subspan(offset + 2, 2));
        const size_t padded = (length + 3) & ~size_t(3);
        if (padded > request.size() - offset - 4) return {};
        // This discovery endpoint does not process authenticated ICE checks
        // or other comprehension-required extensions.
        if (type < 0x8000) return {};
        if (type == 0x8028 && (length != 4 || offset + 8 != request.size() ||
            stun_be(request.subspan(offset + 4, 4)) != stun_crc(request.first(offset)))) return {};
        offset += 4 + padded;
    }
    std::vector<uint8_t> response(request.begin(), request.begin() + 20);
    response[0] = 1; response[1] = 1; response[2] = 0; response[3] = 20;
    const uint16_t xport = port ^ 0x2112;
    const uint32_t xip = ipv4 ^ 0x2112a442U;
    response.insert(response.end(), {0, 0x20, 0, 8, 0, 1, uint8_t(xport >> 8), uint8_t(xport)});
    for (int shift = 24; shift >= 0; shift -= 8) response.push_back(uint8_t(xip >> shift));
    const auto fingerprint = stun_crc(response);
    response.insert(response.end(), {0x80, 0x28, 0, 4});
    for (int shift = 24; shift >= 0; shift -= 8) response.push_back(uint8_t(fingerprint >> shift));
    return response;
}
}
