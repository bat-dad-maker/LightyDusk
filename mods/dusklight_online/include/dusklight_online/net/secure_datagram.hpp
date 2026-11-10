#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dusklight_online::net {

// Authenticated ephemeral X25519 handshake and XChaCha20-Poly1305 datagrams.
// The pre-shared key must be an independently generated, high-entropy invite
// secret. A human-chosen lobby code is not suitable for this handshake.
class SecureDatagram {
public:
    SecureDatagram() = default;
    SecureDatagram(std::string_view inviteSecret, bool initiator);
    ~SecureDatagram();
    SecureDatagram(const SecureDatagram&) = delete;
    SecureDatagram& operator=(const SecureDatagram&) = delete;
    SecureDatagram(SecureDatagram&&) = default;
    SecureDatagram& operator=(SecureDatagram&&) = default;

    bool valid() const { return state_ != State::Invalid; }
    bool established() const { return state_ == State::Established; }
    void set_session(uint64_t session) { session_ = session; }
    std::vector<uint8_t> begin();
    std::vector<uint8_t> handshake(std::span<const uint8_t> packet);
    std::span<const uint8_t> retry_packet() const { return retry_; }
    std::vector<uint8_t> seal(std::span<const uint8_t> plaintext);
    std::optional<std::vector<uint8_t>> open(std::span<const uint8_t> packet);

private:
    enum class State { Invalid, Idle, ClientHello, ServerHello, Established };
    State state_ = State::Invalid;
    bool initiator_ = false;
    std::array<uint8_t, 32> psk_{}, privateKey_{}, publicKey_{};
    std::array<uint8_t, 32> remotePublic_{}, master_{}, txKey_{}, rxKey_{};
    std::array<uint8_t, 16> localNonce_{}, remoteNonce_{}, txPrefix_{}, rxPrefix_{};
    std::vector<uint8_t> retry_;
    uint64_t txCounter_ = 0, rxHighest_ = 0, rxWindow_ = 0;
    uint64_t session_ = 0;
    bool rxSeen_ = false;

    bool generate_ephemeral();
    std::vector<uint8_t> transcript() const;
    bool derive_keys();
};

} // namespace dusklight_online::net
