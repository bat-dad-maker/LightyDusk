#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dusklight_online::net {

// The route is a public, case-insensitive lookup digest. The actual name and
// lobby proofs travel inside CloudCipher records. See tools/cloud_rooms/PROTOCOL.md.
std::string cloud_room_route(std::string_view room);

// Per-WebSocket X25519 / HKDF-SHA256 / ChaCha20-Poly1305 channel. WSS
// authenticates the ephemeral key exchange; no lobby code is used as a key.
class CloudCipher {
public:
    static constexpr size_t maxPlaintext = 16 * 1024;
    static constexpr size_t maxWire = 5 + 2 * (8 + maxPlaintext + 16);
    explicit CloudCipher(std::string_view route, bool client = true);
    ~CloudCipher();
    CloudCipher(const CloudCipher&) = delete;
    CloudCipher& operator=(const CloudCipher&) = delete;

    bool valid() const { return valid_; }
    bool established() const { return established_; }
    std::string public_key() const;
    bool exchange(std::string_view remotePublic);
    std::string seal(std::string_view plaintext);
    std::optional<std::string> open(std::string_view wire);

private:
    std::string route_;
    bool client_, valid_ = false, established_ = false;
    std::array<uint8_t, 32> private_{}, public_{}, txKey_{}, rxKey_{};
    uint64_t tx_ = 0, rx_ = 0;
};

} // namespace dusklight_online::net
