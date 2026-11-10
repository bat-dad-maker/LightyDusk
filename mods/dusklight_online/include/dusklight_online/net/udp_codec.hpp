#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace dusklight_online::net::udp {

inline constexpr size_t kChunkPayloadBytes = 1100;
inline constexpr size_t kSenderIdBytes = 32;
inline constexpr size_t kMaxCompressedBytes = 256 * 1024;
inline constexpr size_t kMaxUncompressedBytes = 512 * 1024;
inline constexpr size_t kMaxInflightSequences = 8;

enum class PacketType : uint8_t {
    PoseJson = 1,
    PoseMsgpack = 2,
    RemoteObject = 3,
    MidnaMsgpack = 4,
    PoseAck = 5,
    RelayRegister = 6,
    SemanticPoseMsgpack = 7,
    VoiceOpus = 8,
};

enum AckStressFlags : uint8_t {
    AckParityRecovered = 1 << 0,
    AckSequenceGap = 1 << 1,
    AckReassemblyEvicted = 1 << 2,
    AckBaselineReset = 1 << 3,
};

enum RemoteObjectFlags : uint8_t {
    ObjectActive = 1 << 0,
    ObjectExploding = 1 << 1,
};

#pragma pack(push, 1)
struct RemoteObjectPacket {
    char stageName[8]{};
    uint32_t sequence = 0;
    int32_t objectId = -1;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int16_t angleY = 0;
    int16_t exTime = -1;
    int8_t room = -1;
    uint8_t objectKind = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
};

struct AckPacket {
    uint32_t sequence = 0;
    uint8_t ackedType = 0;
    char ackedSenderId[kSenderIdBytes]{};
    uint8_t stressFlags = 0;
};
struct VoicePosition {
    char stageName[8]{};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int8_t room = -1;
};
#pragma pack(pop)

static_assert(sizeof(RemoteObjectPacket) == 36);
static_assert(sizeof(AckPacket) == 38);
static_assert(sizeof(VoicePosition) == 21);

struct MessageCommitToken {
    std::string senderId;
    PacketType type = PacketType::PoseMsgpack;
    uint32_t sequence = 0;
    uint8_t stressFlags = 0;
    bool valid = false;
};

struct Datagram {
    std::vector<uint8_t> bytes;
    PacketType type = PacketType::PoseMsgpack;
    uint32_t sequence = 0;
    uint16_t chunkIndex = 0;
    uint16_t chunkCount = 0;
    bool parity = false;
};

enum class DecodeKind : uint8_t {
    None,
    Message,
    Ack,
    RemoteObject,
    Voice,
    RelayRegistration,
};

struct DecodeResult {
    DecodeKind kind = DecodeKind::None;
    PacketType type = PacketType::PoseMsgpack;
    std::string senderId;
    uint32_t sequence = 0;
    uint8_t stressFlags = 0;
    nlohmann::json message;
    AckPacket ack;
    RemoteObjectPacket remoteObject;
    std::vector<uint8_t> voice;
    VoicePosition voicePosition;
    std::string relayToken;
    MessageCommitToken messageToken;
};

// Allocation-free admission metadata (apart from the bounded sender string)
// parsed with the same generic header validation as Decoder::accept().
// Transport uses this before a packet can allocate reassembly state.
struct DatagramInfo {
    PacketType type = PacketType::PoseMsgpack;
    std::string senderId;
    uint32_t sequence = 0;
};

std::vector<Datagram> encode_message(const nlohmann::json& message,
                                     std::string_view senderId,
                                     PacketType type = PacketType::PoseMsgpack,
                                     std::string* error = nullptr);
Datagram encode_ack(std::string_view senderId, std::string_view ackedSenderId,
                    PacketType ackedType, uint32_t sequence, uint8_t stressFlags = 0);
Datagram encode_remote_object(std::string_view senderId,
                              const RemoteObjectPacket& object);
Datagram encode_voice(std::string_view senderId, uint32_t sequence,
                      const VoicePosition& position, std::span<const uint8_t> opus);
Datagram encode_relay_registration(std::string_view clientId, std::string_view token);

class Decoder {
public:
    DecodeResult accept(std::span<const uint8_t> datagram);
    void commit_message(const MessageCommitToken& token);
    void discard_message(const MessageCommitToken& token);
    void reset();

private:
    struct Reassembly {
        uint32_t sequence = 0;
        uint16_t chunkCount = 0;
        uint32_t uncompressedSize = 0;
        uint32_t compressedSize = 0;
        uint16_t receivedCount = 0;
        std::vector<uint8_t> compressed;
        std::vector<uint8_t> received;
        std::vector<uint8_t> parity;
        bool parityReceived = false;
    };

    std::map<std::string, std::map<uint32_t, Reassembly>> reassemblies_;
    std::map<std::string, uint32_t> lastProcessed_;
    std::map<std::string, uint8_t> pendingStress_;
};

std::string acked_sender_id(const AckPacket& ack);
std::optional<DatagramInfo> inspect_datagram(std::span<const uint8_t> datagram);
std::optional<PacketType> peek_packet_type(std::span<const uint8_t> datagram);

}  // namespace dusklight_online::net::udp
