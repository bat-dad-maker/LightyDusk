#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dusklight_online::net::auth {

using Digest = std::array<uint8_t, 32>;

Digest hmac_sha256(std::string_view key, std::string_view message);
Digest pbkdf2_sha256(std::string_view password, std::string_view salt,
                     uint32_t iterations = 100000);
std::string hex(const Digest& bytes);
std::optional<Digest> unhex_digest(std::string_view value);
std::string random_nonce();
bool equal(std::string_view left, std::string_view right);

// The endpoint key is empty for Cloudflare, where the encrypted WSS control
// channel protects registration.
// A manually hosted relay uses the secret from its TP1 endpoint code so a
// captured verifier cannot be dictionary-attacked without that separate code.
Digest lobby_verifier(std::string_view code, std::string_view room,
                      std::string_view endpointKey = {});
std::optional<Digest> room_proof_key(const Digest& verifier, std::string_view saltHex);
std::string room_proof(const Digest& key, std::string_view nonceHex);
std::string direct_proof(std::string_view sessionKey, std::string_view nonceHex);

} // namespace dusklight_online::net::auth
