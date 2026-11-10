#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::game {

struct VoiceFrame {
    uint32_t sequence = 0;
    std::vector<uint8_t> bytes;
};

// Local capture and playback only. The transport owns sender authentication.
class VoiceChat {
public:
    VoiceChat();
    ~VoiceChat();
    VoiceChat(const VoiceChat&) = delete;
    VoiceChat& operator=(const VoiceChat&) = delete;

    [[nodiscard]] std::vector<std::string> input_devices();
    void configure(bool enabled, bool micMuted, std::string_view input, int micPercent,
                   int playerPercent, bool directMode, float masterGain);
    [[nodiscard]] std::vector<VoiceFrame> capture();
    void receive(std::string_view peerId, uint32_t sequence,
                 std::span<const uint8_t> encoded, float proximityGain, float pan);
    void peer_left(std::string_view peerId);
    void stop();
    [[nodiscard]] const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace dusklight_online::game
