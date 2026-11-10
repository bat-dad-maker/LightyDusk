#include "dusklight_online/net/secure_datagram.hpp"
#include "dusklight_online/net/secure_random.hpp"
#include "dusklight_online/net/wire_limits.hpp"
#include "monocypher.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace dusklight_online::net {
namespace {
constexpr size_t kHelloSize = 4 + 1 + 32 + 16 + 32;
constexpr size_t kFinishSize = 4 + 1 + 32;
using Key = std::array<uint8_t, 32>;

void append(std::vector<uint8_t>& to, std::span<const uint8_t> from) {
    to.insert(to.end(), from.begin(), from.end());
}
void append(std::vector<uint8_t>& to, std::string_view from) {
    to.insert(to.end(), from.begin(), from.end());
}
Key keyed(const Key& key, std::string_view label, std::span<const uint8_t> data = {}) {
    std::vector<uint8_t> message;
    message.reserve(label.size() + data.size());
    append(message, label);
    append(message, data);
    Key result{};
    crypto_blake2b_keyed(result.data(), result.size(), key.data(), key.size(),
                         message.data(), message.size());
    return result;
}
void write64(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t read64(const uint8_t* in) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= uint64_t(in[i]) << (8 * i);
    return result;
}
bool nonzero(std::span<const uint8_t> bytes) {
    uint8_t value = 0;
    for (uint8_t byte : bytes) value |= byte;
    return value != 0;
}
} // namespace

SecureDatagram::SecureDatagram(std::string_view inviteSecret, bool initiator)
    : initiator_(initiator) {
    if (inviteSecret.size() < 16) return;
    std::vector<uint8_t> source;
    append(source, "dusklight-native-udp-psk-v1:");
    append(source, inviteSecret);
    crypto_blake2b(psk_.data(), psk_.size(), source.data(), source.size());
    state_ = State::Idle;
}

SecureDatagram::~SecureDatagram() {
    crypto_wipe(psk_.data(), psk_.size());
    crypto_wipe(privateKey_.data(), privateKey_.size());
    crypto_wipe(master_.data(), master_.size());
    crypto_wipe(txKey_.data(), txKey_.size());
    crypto_wipe(rxKey_.data(), rxKey_.size());
}

bool SecureDatagram::generate_ephemeral() {
    if (!secure_random_bytes(privateKey_) ||
        !secure_random_bytes(localNonce_)) {
        state_ = State::Invalid;
        return false;
    }
    crypto_x25519_public_key(publicKey_.data(), privateKey_.data());
    return true;
}

std::vector<uint8_t> SecureDatagram::transcript() const {
    std::vector<uint8_t> result;
    result.reserve(96);
    if (initiator_) {
        append(result, publicKey_); append(result, localNonce_);
        append(result, remotePublic_); append(result, remoteNonce_);
    } else {
        append(result, remotePublic_); append(result, remoteNonce_);
        append(result, publicKey_); append(result, localNonce_);
    }
    return result;
}

bool SecureDatagram::derive_keys() {
    Key shared{};
    crypto_x25519(shared.data(), privateKey_.data(), remotePublic_.data());
    if (!nonzero(shared)) { crypto_wipe(shared.data(), shared.size()); return false; }
    auto material = transcript();
    append(material, shared);
    master_ = keyed(psk_, "dusklight-native-master-v1:", material);
    crypto_wipe(shared.data(), shared.size());
    crypto_wipe(privateKey_.data(), privateKey_.size());
    const Key c2s = keyed(master_, "client-to-server-key");
    const Key s2c = keyed(master_, "server-to-client-key");
    const Key c2sNonce = keyed(master_, "client-to-server-nonce");
    const Key s2cNonce = keyed(master_, "server-to-client-nonce");
    txKey_ = initiator_ ? c2s : s2c;
    rxKey_ = initiator_ ? s2c : c2s;
    const auto& txNonce = initiator_ ? c2sNonce : s2cNonce;
    const auto& rxNonce = initiator_ ? s2cNonce : c2sNonce;
    std::copy_n(txNonce.begin(), txPrefix_.size(), txPrefix_.begin());
    std::copy_n(rxNonce.begin(), rxPrefix_.size(), rxPrefix_.begin());
    return true;
}

std::vector<uint8_t> SecureDatagram::begin() {
    if (!initiator_ || state_ != State::Idle || !generate_ephemeral()) return {};
    retry_ = {'D', 'S', 'H', '1', 1};
    append(retry_, publicKey_);
    append(retry_, localNonce_);
    const Key mac = keyed(psk_, "client-hello-v1:",
                          std::span<const uint8_t>(retry_).subspan(5));
    append(retry_, mac);
    state_ = State::ClientHello;
    return retry_;
}

std::vector<uint8_t> SecureDatagram::handshake(std::span<const uint8_t> packet) {
    if (packet.size() < 5 || std::memcmp(packet.data(), "DSH1", 4) != 0) return {};
    if (!initiator_ && packet[4] == 1 && packet.size() == kHelloSize) {
        const Key expected = keyed(psk_, "client-hello-v1:", packet.subspan(5, 48));
        if (crypto_verify32(expected.data(), packet.data() + 53) != 0) return {};
        if (state_ == State::ServerHello) {
            if (std::equal(remotePublic_.begin(), remotePublic_.end(), packet.begin() + 5) &&
                std::equal(remoteNonce_.begin(), remoteNonce_.end(), packet.begin() + 37)) return retry_;
            return {};
        }
        if (state_ != State::Idle || !generate_ephemeral()) return {};
        std::copy_n(packet.begin() + 5, 32, remotePublic_.begin());
        std::copy_n(packet.begin() + 37, 16, remoteNonce_.begin());
        if (!derive_keys()) { state_ = State::Invalid; return {}; }
        retry_ = {'D', 'S', 'H', '1', 2};
        append(retry_, publicKey_);
        append(retry_, localNonce_);
        const auto data = transcript();
        const Key mac = keyed(psk_, "server-hello-v1:", data);
        append(retry_, mac);
        state_ = State::ServerHello;
        return retry_;
    }
    if (initiator_ && packet[4] == 2 && packet.size() == kHelloSize) {
        if (state_ == State::Established) {
            if (!std::equal(remotePublic_.begin(), remotePublic_.end(), packet.begin() + 5) ||
                !std::equal(remoteNonce_.begin(), remoteNonce_.end(), packet.begin() + 37)) return {};
            const Key expected = keyed(psk_, "server-hello-v1:", transcript());
            return crypto_verify32(expected.data(), packet.data() + 53) == 0 ? retry_ :
                std::vector<uint8_t>{};
        }
        if (state_ != State::ClientHello) return {};
        std::copy_n(packet.begin() + 5, 32, remotePublic_.begin());
        std::copy_n(packet.begin() + 37, 16, remoteNonce_.begin());
        const auto data = transcript();
        const Key expected = keyed(psk_, "server-hello-v1:", data);
        if (crypto_verify32(expected.data(), packet.data() + 53) != 0) return {};
        if (!derive_keys()) { state_ = State::Invalid; return {}; }
        retry_ = {'D', 'S', 'H', '1', 3};
        const Key mac = keyed(master_, "client-finish-v1:", data);
        append(retry_, mac);
        state_ = State::Established;
        return retry_;
    }
    if (!initiator_ && packet[4] == 3 && packet.size() == kFinishSize &&
        state_ == State::ServerHello) {
        const auto data = transcript();
        const Key expected = keyed(master_, "client-finish-v1:", data);
        if (crypto_verify32(expected.data(), packet.data() + 5) == 0) {
            state_ = State::Established;
            retry_.clear();
        }
    }
    return {};
}

std::vector<uint8_t> SecureDatagram::seal(std::span<const uint8_t> plaintext) {
    if (!established() || !session_ || txCounter_ == std::numeric_limits<uint64_t>::max()) return {};
    std::vector<uint8_t> packet(kSealedDatagramOverhead + plaintext.size());
    std::memcpy(packet.data(), "DSE1", 4);
    write64(packet.data() + 4, session_);
    write64(packet.data() + 12, txCounter_);
    std::array<uint8_t, 24> nonce{};
    std::copy(txPrefix_.begin(), txPrefix_.end(), nonce.begin());
    write64(nonce.data() + 16, txCounter_++);
    crypto_aead_lock(packet.data() + kSealedDatagramOverhead, packet.data() + 20,
                     txKey_.data(), nonce.data(), packet.data(), 20,
                     plaintext.data(), plaintext.size());
    return packet;
}

std::optional<std::vector<uint8_t>> SecureDatagram::open(std::span<const uint8_t> packet) {
    if (!established() || !session_ || packet.size() < kSealedDatagramOverhead ||
        std::memcmp(packet.data(), "DSE1", 4) != 0) return std::nullopt;
    if (read64(packet.data() + 4) != session_) return std::nullopt;
    const uint64_t counter = read64(packet.data() + 12);
    if (rxSeen_ && counter <= rxHighest_ &&
        (rxHighest_ - counter >= 64 || (rxWindow_ & (uint64_t{1} << (rxHighest_ - counter)))))
        return std::nullopt;
    std::array<uint8_t, 24> nonce{};
    std::copy(rxPrefix_.begin(), rxPrefix_.end(), nonce.begin());
    write64(nonce.data() + 16, counter);
    std::vector<uint8_t> plaintext(packet.size() - kSealedDatagramOverhead);
    if (crypto_aead_unlock(plaintext.data(), packet.data() + 20, rxKey_.data(),
                           nonce.data(), packet.data(), 20,
                           packet.data() + kSealedDatagramOverhead, plaintext.size()) != 0)
        return std::nullopt;
    if (!rxSeen_) { rxSeen_ = true; rxHighest_ = counter; rxWindow_ = 1; }
    else if (counter > rxHighest_) {
        const uint64_t shift = counter - rxHighest_;
        rxWindow_ = (shift >= 64 ? 0 : rxWindow_ << shift) | 1;
        rxHighest_ = counter;
    } else rxWindow_ |= uint64_t{1} << (rxHighest_ - counter);
    return plaintext;
}

} // namespace dusklight_online::net
