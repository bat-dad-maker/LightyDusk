#include "dusklight_online/net/auth_code.hpp"
#include "dusklight_online/net/secure_random.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <stdexcept>
#include <vector>

namespace dusklight_online::net::auth {
namespace {
constexpr std::array<uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

Digest sha256(std::span<const uint8_t> input) {
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8)
        data.push_back(static_cast<uint8_t>(bits >> shift));
    std::array<uint32_t, 8> state{
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (size_t block = 0; block < data.size(); block += 64) {
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = block + i * 4;
            words[i] = (uint32_t(data[p]) << 24) | (uint32_t(data[p + 1]) << 16) |
                       (uint32_t(data[p + 2]) << 8) | data[p + 3];
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t a = std::rotr(words[i - 15], 7) ^
                               std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const uint32_t b = std::rotr(words[i - 2], 17) ^
                               std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + a + words[i - 7] + b;
        }
        auto [a, b, c, d, e, f, g, h] = state;
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^
                                      std::rotr(e, 25)) + choice + kRound[i] + words[i];
            const uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^
                                 std::rotr(a, 22)) + majority;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    Digest result{};
    for (size_t i = 0; i < state.size(); ++i)
        for (size_t j = 0; j < 4; ++j)
            result[i * 4 + j] = static_cast<uint8_t>(state[i] >> (24 - j * 8));
    return result;
}

std::string_view view(const Digest& digest) {
    return {reinterpret_cast<const char*>(digest.data()), digest.size()};
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::optional<std::vector<uint8_t>> unhex(std::string_view value) {
    if (value.size() % 2) return std::nullopt;
    std::vector<uint8_t> result(value.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        const int high = nibble(value[i * 2]);
        const int low = nibble(value[i * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return result;
}

std::string hex_bytes(std::span<const uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (uint8_t byte : bytes) {
        result.push_back(alphabet[byte >> 4]);
        result.push_back(alphabet[byte & 15]);
    }
    return result;
}
} // namespace

Digest hmac_sha256(std::string_view key, std::string_view message) {
    std::array<uint8_t, 64> block{};
    if (key.size() > block.size()) {
        const auto digest = sha256({reinterpret_cast<const uint8_t*>(key.data()), key.size()});
        std::copy(digest.begin(), digest.end(), block.begin());
    } else {
        std::copy(key.begin(), key.end(), block.begin());
    }
    std::vector<uint8_t> inner(64 + message.size());
    std::vector<uint8_t> outer(64 + 32);
    for (size_t i = 0; i < 64; ++i) {
        inner[i] = block[i] ^ 0x36;
        outer[i] = block[i] ^ 0x5c;
    }
    std::copy(message.begin(), message.end(), inner.begin() + 64);
    const auto innerHash = sha256(inner);
    std::copy(innerHash.begin(), innerHash.end(), outer.begin() + 64);
    return sha256(outer);
}

Digest pbkdf2_sha256(std::string_view password, std::string_view salt,
                     uint32_t iterations) {
    std::string block(salt);
    block.append("\0\0\0\1", 4);
    Digest last = hmac_sha256(password, block);
    Digest result = last;
    for (uint32_t i = 1; i < iterations; ++i) {
        last = hmac_sha256(password, view(last));
        for (size_t j = 0; j < result.size(); ++j) result[j] ^= last[j];
    }
    return result;
}

std::string hex(const Digest& bytes) { return hex_bytes(bytes); }

std::optional<Digest> unhex_digest(std::string_view value) {
    if (value.size() != 64) return std::nullopt;
    auto decoded = unhex(value);
    if (!decoded) return std::nullopt;
    Digest result{};
    std::copy(decoded->begin(), decoded->end(), result.begin());
    return result;
}

std::string random_nonce() {
    std::array<uint8_t, 16> bytes{};
    if (!secure_random_bytes(bytes)) throw std::runtime_error("secure random unavailable");
    return hex_bytes(bytes);
}

bool equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<uint8_t>(left[i] ^ right[i]);
    return difference == 0;
}

Digest lobby_verifier(std::string_view code, std::string_view room,
                      std::string_view endpointKey) {
    std::string salt = "dusklight-lobby-code-v1:";
    for (char c : room) salt.push_back(static_cast<char>(
        c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    salt.push_back('\0');
    salt.append(endpointKey);
    return pbkdf2_sha256(code, salt);
}

std::optional<Digest> room_proof_key(const Digest& verifier, std::string_view saltHex) {
    if (saltHex.size() != 32) return std::nullopt;
    const auto salt = unhex(saltHex);
    if (!salt) return std::nullopt;
    std::string message = "dusklight-room-key-v1:";
    message.append(reinterpret_cast<const char*>(salt->data()), salt->size());
    return hmac_sha256(view(verifier), message);
}

std::string room_proof(const Digest& key, std::string_view nonceHex) {
    if (nonceHex.size() != 32 || !unhex(nonceHex)) return {};
    std::string message = "dusklight-room-auth-v1:";
    message.append(nonceHex);
    return hex(hmac_sha256(view(key), message));
}

std::string direct_proof(std::string_view sessionKey, std::string_view nonceHex) {
    if (sessionKey.empty() || nonceHex.size() != 32 || !unhex(nonceHex)) return {};
    std::string message = "dusklight-direct-auth-v2:";
    message.append(nonceHex);
    return hex(hmac_sha256(sessionKey, message));
}
} // namespace dusklight_online::net::auth
