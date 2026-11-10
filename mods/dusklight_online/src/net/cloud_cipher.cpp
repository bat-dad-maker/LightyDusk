#include "dusklight_online/net/cloud_cipher.hpp"
#include "dusklight_online/net/auth_code.hpp"
#include "dusklight_online/net/secure_random.hpp"
#include "monocypher.h"

#include <algorithm>
#include <span>
#include <vector>

namespace dusklight_online::net {
namespace {
constexpr std::string_view prefix = "DCR1:";
constexpr uint64_t maxSequence = 9007199254740991ULL; // JS safe integer

std::string_view view(std::span<const uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string hex(std::span<const uint8_t> bytes) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) { out += alphabet[b >> 4]; out += alphabet[b & 15]; }
    return out;
}
int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
std::array<uint8_t, 12> nonce(uint64_t sequence) {
    std::array<uint8_t, 12> out{};
    for (size_t i = 0; i < 8; ++i) out[11 - i] = uint8_t(sequence >> (8 * i));
    return out;
}
std::string aad(std::span<const uint8_t> sequence) {
    return std::string(prefix) + std::string(view(sequence));
}
} // namespace

std::string cloud_room_route(std::string_view room) {
    std::string normalized(room);
    for (char& c : normalized) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return auth::hex(auth::hmac_sha256("dusklight-cloud-room-route-v1", normalized));
}

CloudCipher::CloudCipher(std::string_view route, bool client)
    : route_(route), client_(client) {
    if (!auth::unhex_digest(route) || !secure_random_bytes(private_)) return;
    crypto_x25519_public_key(public_.data(), private_.data());
    valid_ = true;
}

CloudCipher::~CloudCipher() {
    crypto_wipe(private_.data(), private_.size());
    crypto_wipe(txKey_.data(), txKey_.size());
    crypto_wipe(rxKey_.data(), rxKey_.size());
}

std::string CloudCipher::public_key() const { return valid_ ? auth::hex(public_) : ""; }

bool CloudCipher::exchange(std::string_view remotePublic) {
    if (!valid_ || established_) return false;
    const auto remote = auth::unhex_digest(remotePublic);
    if (!remote) return false;
    std::array<uint8_t, 32> shared{};
    crypto_x25519(shared.data(), private_.data(), remote->data());
    crypto_wipe(private_.data(), private_.size());
    if (std::all_of(shared.begin(), shared.end(), [](uint8_t b) { return b == 0; })) {
        valid_ = false;
        return false;
    }
    std::string salt = "dusklight-cloud-channel-v1:" + route_;
    salt += view(client_ ? public_ : *remote);
    salt += view(client_ ? *remote : public_);
    auto prk = auth::hmac_sha256(salt, view(shared));
    txKey_ = auth::hmac_sha256(view(prk), client_ ? "client-to-server\x01" : "server-to-client\x01");
    rxKey_ = auth::hmac_sha256(view(prk), client_ ? "server-to-client\x01" : "client-to-server\x01");
    crypto_wipe(shared.data(), shared.size());
    crypto_wipe(prk.data(), prk.size());
    established_ = true;
    return true;
}

std::string CloudCipher::seal(std::string_view plaintext) {
    if (!established_ || plaintext.size() > maxPlaintext || tx_ >= maxSequence) return {};
    const auto iv = nonce(tx_++);
    std::vector<uint8_t> record(8 + plaintext.size() + 16);
    std::copy(iv.begin() + 4, iv.end(), record.begin());
    const auto header = aad(std::span<const uint8_t>(record).first(8));
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, txKey_.data(), iv.data());
    crypto_aead_write(&ctx, record.data() + 8, record.data() + 8 + plaintext.size(),
        reinterpret_cast<const uint8_t*>(header.data()), header.size(),
        reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size());
    crypto_wipe(&ctx, sizeof(ctx));
    return std::string(prefix) + hex(record);
}

std::optional<std::string> CloudCipher::open(std::string_view wire) {
    if (!established_ || rx_ >= maxSequence || wire.size() < 53 || wire.size() > maxWire ||
        !wire.starts_with(prefix) || (wire.size() - prefix.size()) % 2) return std::nullopt;
    std::vector<uint8_t> record((wire.size() - prefix.size()) / 2);
    for (size_t i = 0; i < record.size(); ++i) {
        const int hi = nibble(wire[5 + i * 2]), lo = nibble(wire[6 + i * 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        record[i] = uint8_t((hi << 4) | lo);
    }
    const auto iv = nonce(rx_);
    if (!std::equal(iv.begin() + 4, iv.end(), record.begin())) return std::nullopt;
    const auto header = aad(std::span<const uint8_t>(record).first(8));
    std::string plaintext(record.size() - 24, '\0');
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, rxKey_.data(), iv.data());
    const int result = crypto_aead_read(&ctx, reinterpret_cast<uint8_t*>(plaintext.data()),
        record.data() + 8 + plaintext.size(), reinterpret_cast<const uint8_t*>(header.data()),
        header.size(), record.data() + 8, plaintext.size());
    crypto_wipe(&ctx, sizeof(ctx));
    if (result != 0) return std::nullopt;
    ++rx_;
    return plaintext;
}
} // namespace dusklight_online::net
