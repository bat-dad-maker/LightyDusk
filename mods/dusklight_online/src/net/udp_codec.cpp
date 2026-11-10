#include "dusklight_online/net/udp_codec.hpp"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace dusklight_online::net::udp {
namespace {

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t headerSize;
    uint32_t sequence;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t uncompressedSize;
    uint32_t compressedSize;
    uint16_t payloadSize;
    char senderId[kSenderIdBytes];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 58);

bool is_message_type(PacketType type) {
    return type == PacketType::PoseJson || type == PacketType::PoseMsgpack ||
           type == PacketType::MidnaMsgpack || type == PacketType::SemanticPoseMsgpack;
}

bool is_known_type(uint8_t type) {
    return type >= static_cast<uint8_t>(PacketType::PoseJson) &&
           type <= static_cast<uint8_t>(PacketType::VoiceOpus);
}

void copy_id(char (&destination)[kSenderIdBytes], std::string_view source) {
    std::memset(destination, 0, sizeof(destination));
    std::memcpy(destination, source.data(), std::min(source.size(), sizeof(destination) - 1));
}

std::string read_id(const char (&source)[kSenderIdBytes]) {
    size_t length = 0;
    while (length < sizeof(source) && source[length] != '\0') {
        ++length;
    }
    return std::string(source, length);
}

std::optional<Header> inspect_header(std::span<const uint8_t> datagram) {
    if (datagram.size() < sizeof(Header)) return std::nullopt;
    Header header{};
    std::memcpy(&header, datagram.data(), sizeof(header));
    if (std::memcmp(header.magic, "DMPU", 4) != 0 || header.version != 1 ||
        !is_known_type(header.type) || header.headerSize != sizeof(Header) ||
        header.payloadSize == 0 || header.chunkCount == 0 ||
        header.chunkIndex > header.chunkCount ||
        header.compressedSize == 0 || header.compressedSize > kMaxCompressedBytes ||
        header.uncompressedSize == 0 || header.uncompressedSize > kMaxUncompressedBytes ||
        datagram.size() != sizeof(Header) + header.payloadSize) {
        return std::nullopt;
    }
    return header;
}

std::string message_flow_key(std::string_view senderId, PacketType type) {
    return std::string(senderId) + "\x1f" +
           std::to_string(static_cast<uint8_t>(type));
}

Header make_header(PacketType type, std::string_view senderId, uint32_t sequence,
                   uint16_t chunkIndex, uint16_t chunkCount, uint32_t rawSize,
                   uint32_t compressedSize, uint16_t payloadSize) {
    Header header{};
    std::memcpy(header.magic, "DMPU", 4);
    header.version = 1;
    header.type = static_cast<uint8_t>(type);
    header.headerSize = sizeof(Header);
    header.sequence = sequence;
    header.chunkIndex = chunkIndex;
    header.chunkCount = chunkCount;
    header.uncompressedSize = rawSize;
    header.compressedSize = compressedSize;
    header.payloadSize = payloadSize;
    copy_id(header.senderId, senderId);
    return header;
}

Datagram make_datagram(const Header& header, const void* payload, size_t payloadSize) {
    Datagram result;
    result.type = static_cast<PacketType>(header.type);
    result.sequence = header.sequence;
    result.chunkIndex = header.chunkIndex;
    result.chunkCount = header.chunkCount;
    result.parity = is_message_type(result.type) && header.chunkIndex == header.chunkCount;
    result.bytes.resize(sizeof(header) + payloadSize);
    std::memcpy(result.bytes.data(), &header, sizeof(header));
    if (payloadSize != 0) {
        std::memcpy(result.bytes.data() + sizeof(header), payload, payloadSize);
    }
    return result;
}

void set_error(std::string* output, std::string value) {
    if (output != nullptr) {
        *output = std::move(value);
    }
}

// Parse peer-owned visuals with a fixed nesting budget. A size limit alone
// does not protect the recursive JSON/MessagePack parsers from tiny, deeply
// nested values.
class BoundedPoseSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    nlohmann::json value;

    bool null() override { add(nullptr); return true; }
    bool boolean(bool input) override { add(input); return true; }
    bool number_integer(number_integer_t input) override { add(input); return true; }
    bool number_unsigned(number_unsigned_t input) override { add(input); return true; }
    bool number_float(number_float_t input, const string_t&) override { add(input); return true; }
    bool string(string_t& input) override { add(std::move(input)); return true; }
    bool binary(binary_t& input) override {
        add(nlohmann::json::binary(std::move(input)));
        return true;
    }
    bool start_object(std::size_t) override {
        if (parents_.size() >= kMaxDepth) return false;
        parents_.push_back(add(nlohmann::json::object()));
        return true;
    }
    bool key(string_t& input) override { key_ = std::move(input); return true; }
    bool end_object() override { parents_.pop_back(); return true; }
    bool start_array(std::size_t) override {
        if (parents_.size() >= kMaxDepth) return false;
        parents_.push_back(add(nlohmann::json::array()));
        return true;
    }
    bool end_array() override { parents_.pop_back(); return true; }
    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::detail::exception&) override { return false; }

private:
    static constexpr size_t kMaxDepth = 64;
    std::vector<nlohmann::json*> parents_;
    string_t key_;

    nlohmann::json* add(nlohmann::json input) {
        if (parents_.empty()) {
            value = std::move(input);
            return &value;
        }
        auto* parent = parents_.back();
        if (parent->is_array()) {
            parent->push_back(std::move(input));
            return &parent->back();
        }
        (*parent)[key_] = std::move(input);
        return &(*parent)[key_];
    }
};

}  // namespace

std::vector<Datagram> encode_message(const nlohmann::json& message,
                                     std::string_view senderId, PacketType type,
                                     std::string* error) {
    if (!is_message_type(type)) {
        set_error(error, "packet type is not a pose/message type");
        return {};
    }

    std::vector<uint8_t> raw;
    if (type == PacketType::PoseJson) {
        const std::string text = message.dump();
        raw.assign(text.begin(), text.end());
    } else {
        raw = nlohmann::json::to_msgpack(message);
    }
    if (raw.empty() || raw.size() > kMaxUncompressedBytes) {
        set_error(error, "uncompressed UDP message exceeds the wire limit");
        return {};
    }

    std::vector<uint8_t> compressed(ZSTD_compressBound(raw.size()));
    const size_t compressedSize =
        ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
    if (ZSTD_isError(compressedSize) || compressedSize == 0 ||
        compressedSize > kMaxCompressedBytes) {
        set_error(error, ZSTD_isError(compressedSize)
                             ? std::string("zstd compression failed: ") +
                                   ZSTD_getErrorName(compressedSize)
                             : "compressed UDP message exceeds the wire limit");
        return {};
    }
    compressed.resize(compressedSize);

    const uint32_t sequence = message.is_object() ? message.value("sequence", 0U) : 0U;
    const size_t chunksNeeded =
        (compressed.size() + kChunkPayloadBytes - 1) / kChunkPayloadBytes;
    if (chunksNeeded == 0 || chunksNeeded > std::numeric_limits<uint16_t>::max()) {
        set_error(error, "invalid UDP chunk count");
        return {};
    }
    const uint16_t chunkCount = static_cast<uint16_t>(chunksNeeded);
    std::vector<Datagram> datagrams;
    datagrams.reserve(static_cast<size_t>(chunkCount) + (chunkCount > 1 ? 1 : 0));
    std::array<uint8_t, kChunkPayloadBytes> parity{};

    for (uint16_t index = 0; index < chunkCount; ++index) {
        const size_t offset = static_cast<size_t>(index) * kChunkPayloadBytes;
        const size_t payloadSize = std::min(kChunkPayloadBytes, compressed.size() - offset);
        for (size_t i = 0; i < kChunkPayloadBytes; ++i) {
            const size_t source = offset + i;
            parity[i] ^= source < compressed.size() ? compressed[source] : 0;
        }
        const Header header = make_header(
            type, senderId, sequence, index, chunkCount, static_cast<uint32_t>(raw.size()),
            static_cast<uint32_t>(compressed.size()), static_cast<uint16_t>(payloadSize));
        datagrams.push_back(make_datagram(header, compressed.data() + offset, payloadSize));
    }

    if (chunkCount > 1) {
        const Header header = make_header(
            type, senderId, sequence, chunkCount, chunkCount,
            static_cast<uint32_t>(raw.size()), static_cast<uint32_t>(compressed.size()),
            static_cast<uint16_t>(parity.size()));
        datagrams.push_back(make_datagram(header, parity.data(), parity.size()));
    }
    return datagrams;
}

Datagram encode_ack(std::string_view senderId, std::string_view ackedSenderId,
                    PacketType ackedType, uint32_t sequence, uint8_t stressFlags) {
    AckPacket ack{};
    ack.sequence = sequence;
    ack.ackedType = static_cast<uint8_t>(ackedType);
    ack.stressFlags = stressFlags;
    copy_id(ack.ackedSenderId, ackedSenderId);
    const Header header = make_header(
        PacketType::PoseAck, senderId, sequence, 0, 1, sizeof(ack), sizeof(ack), sizeof(ack));
    return make_datagram(header, &ack, sizeof(ack));
}

Datagram encode_remote_object(std::string_view senderId,
                              const RemoteObjectPacket& object) {
    const Header header = make_header(PacketType::RemoteObject, senderId, object.sequence, 0, 1,
                                      sizeof(object), sizeof(object), sizeof(object));
    return make_datagram(header, &object, sizeof(object));
}

Datagram encode_relay_registration(std::string_view clientId, std::string_view token) {
    if (token.empty() || token.size() > kChunkPayloadBytes) {
        return {};
    }
    const auto tokenSize = static_cast<uint32_t>(token.size());
    const Header header = make_header(PacketType::RelayRegister, clientId, 0, 0, 1, tokenSize,
                                      tokenSize, static_cast<uint16_t>(tokenSize));
    return make_datagram(header, token.data(), token.size());
}

Datagram encode_voice(std::string_view senderId, uint32_t sequence,
                      const VoicePosition& position, std::span<const uint8_t> opus) {
    if (sequence == 0 || opus.empty() || opus.size() > 400) return {};
    const auto size = static_cast<uint16_t>(sizeof(position) + opus.size());
    const Header header = make_header(PacketType::VoiceOpus, senderId, sequence,
                                      0, 1, size, size, size);
    Datagram packet = make_datagram(header, &position, sizeof(position));
    packet.bytes.insert(packet.bytes.end(), opus.begin(), opus.end());
    return packet;
}

DecodeResult Decoder::accept(std::span<const uint8_t> datagram) {
    DecodeResult result;
    const std::optional<Header> inspected = inspect_header(datagram);
    if (!inspected.has_value()) return result;
    const Header header = *inspected;

    const auto type = static_cast<PacketType>(header.type);
    const std::string senderId = read_id(header.senderId);
    const uint8_t* payload = datagram.data() + sizeof(Header);
    result.type = type;
    result.senderId = senderId;
    result.sequence = header.sequence;

    if (type == PacketType::RelayRegister) {
        if (header.chunkIndex != 0 || header.chunkCount != 1 ||
            header.uncompressedSize != header.payloadSize ||
            header.compressedSize != header.payloadSize) {
            return {};
        }
        result.kind = DecodeKind::RelayRegistration;
        result.relayToken.assign(reinterpret_cast<const char*>(payload), header.payloadSize);
        return result;
    }

    if (type == PacketType::PoseAck) {
        const bool legacy = header.payloadSize == sizeof(AckPacket) - 1;
        if (header.chunkIndex != 0 || header.chunkCount != 1 ||
            (!legacy && header.payloadSize != sizeof(AckPacket)) ||
            header.uncompressedSize != header.payloadSize ||
            header.compressedSize != header.payloadSize) {
            return {};
        }
        std::memcpy(&result.ack, payload, header.payloadSize);
        if (result.ack.ackedType != static_cast<uint8_t>(PacketType::PoseMsgpack) &&
            result.ack.ackedType != static_cast<uint8_t>(PacketType::SemanticPoseMsgpack) &&
            result.ack.ackedType != static_cast<uint8_t>(PacketType::MidnaMsgpack)) {
            return {};
        }
        result.kind = DecodeKind::Ack;
        return result;
    }

    if (type == PacketType::RemoteObject) {
        if (header.chunkIndex != 0 || header.chunkCount != 1 ||
            header.payloadSize != sizeof(RemoteObjectPacket) ||
            header.uncompressedSize != sizeof(RemoteObjectPacket) ||
            header.compressedSize != sizeof(RemoteObjectPacket)) {
            return {};
        }
        std::memcpy(&result.remoteObject, payload, sizeof(RemoteObjectPacket));
        result.kind = DecodeKind::RemoteObject;
        return result;
    }

    if (type == PacketType::VoiceOpus) {
        if (header.sequence == 0 || header.chunkIndex != 0 || header.chunkCount != 1 ||
            header.payloadSize <= sizeof(VoicePosition) ||
            header.payloadSize > sizeof(VoicePosition) + 400 ||
            header.uncompressedSize != header.payloadSize ||
            header.compressedSize != header.payloadSize) return {};
        std::memcpy(&result.voicePosition, payload, sizeof(VoicePosition));
        if (std::memchr(result.voicePosition.stageName, '\0', 8) == nullptr) return {};
        result.kind = DecodeKind::Voice;
        result.voice.assign(payload + sizeof(VoicePosition), payload + header.payloadSize);
        return result;
    }

    if (!is_message_type(type)) {
        return {};
    }
    const std::string flowKey = message_flow_key(senderId, type);
    const auto last = lastProcessed_.find(flowKey);
    if (last != lastProcessed_.end() && header.sequence <= last->second) {
        return {};
    }

    auto& sequences = reassemblies_[flowKey];
    auto sequenceIt = sequences.find(header.sequence);
    if (sequenceIt == sequences.end()) {
        while (sequences.size() >= kMaxInflightSequences) {
            sequences.erase(sequences.begin());
            pendingStress_[flowKey] |= AckReassemblyEvicted;
        }
        Reassembly state;
        state.sequence = header.sequence;
        state.chunkCount = header.chunkCount;
        state.uncompressedSize = header.uncompressedSize;
        state.compressedSize = header.compressedSize;
        state.compressed.assign(header.compressedSize, 0);
        state.received.assign(header.chunkCount, 0);
        sequenceIt = sequences.emplace(header.sequence, std::move(state)).first;
    }
    Reassembly& state = sequenceIt->second;
    if (state.chunkCount != header.chunkCount ||
        state.uncompressedSize != header.uncompressedSize ||
        state.compressedSize != header.compressedSize) {
        sequences.erase(sequenceIt);
        return {};
    }

    if (header.chunkIndex == header.chunkCount) {
        if (header.payloadSize != kChunkPayloadBytes) {
            return {};
        }
        state.parity.assign(payload, payload + header.payloadSize);
        state.parityReceived = true;
    } else {
        const size_t offset = static_cast<size_t>(header.chunkIndex) * kChunkPayloadBytes;
        if (offset + header.payloadSize > state.compressed.size()) {
            return {};
        }
        if (!state.received[header.chunkIndex]) {
            state.received[header.chunkIndex] = 1;
            ++state.receivedCount;
        }
        std::memcpy(state.compressed.data() + offset, payload, header.payloadSize);
    }

    uint8_t stress = pendingStress_[flowKey];
    if (state.receivedCount != state.chunkCount) {
        if (!state.parityReceived || state.receivedCount + 1 != state.chunkCount) {
            return {};
        }
        uint16_t missing = 0;
        while (missing < state.chunkCount && state.received[missing]) {
            ++missing;
        }
        if (missing >= state.chunkCount) {
            return {};
        }
        std::array<uint8_t, kChunkPayloadBytes> recovered{};
        std::copy(state.parity.begin(), state.parity.end(), recovered.begin());
        for (uint16_t index = 0; index < state.chunkCount; ++index) {
            if (index == missing) {
                continue;
            }
            const size_t offset = static_cast<size_t>(index) * kChunkPayloadBytes;
            for (size_t i = 0; i < kChunkPayloadBytes; ++i) {
                const size_t source = offset + i;
                recovered[i] ^= source < state.compressed.size() ? state.compressed[source] : 0;
            }
        }
        const size_t offset = static_cast<size_t>(missing) * kChunkPayloadBytes;
        const size_t size = std::min(kChunkPayloadBytes, state.compressed.size() - offset);
        std::memcpy(state.compressed.data() + offset, recovered.data(), size);
        state.received[missing] = 1;
        ++state.receivedCount;
        stress |= AckParityRecovered;
    }

    std::vector<uint8_t> raw(state.uncompressedSize);
    const size_t decompressed = ZSTD_decompress(raw.data(), raw.size(), state.compressed.data(),
                                                state.compressed.size());
    if (ZSTD_isError(decompressed) || decompressed != raw.size()) {
        sequences.erase(header.sequence);
        return {};
    }

    try {
        BoundedPoseSax sax;
        const auto format = type == PacketType::PoseJson ?
            nlohmann::json::input_format_t::json : nlohmann::json::input_format_t::msgpack;
        if (!nlohmann::json::sax_parse(raw.begin(), raw.end(), &sax, format)) {
            sequences.erase(header.sequence);
            return {};
        }
        result.message = std::move(sax.value);
    } catch (const nlohmann::json::exception&) {
        sequences.erase(header.sequence);
        return {};
    }
    if (!result.message.is_object()) {
        sequences.erase(header.sequence);
        return {};
    }

    if (last != lastProcessed_.end() && header.sequence > last->second + 1) {
        stress |= AckSequenceGap;
    }
    result.kind = DecodeKind::Message;
    result.stressFlags = stress;
    result.messageToken = {senderId, type, header.sequence, stress, true};
    sequences.erase(header.sequence);
    return result;
}

void Decoder::commit_message(const MessageCommitToken& token) {
    if (!token.valid) return;
    const std::string flowKey = message_flow_key(token.senderId, token.type);
    const auto last = lastProcessed_.find(flowKey);
    if (last == lastProcessed_.end() || token.sequence > last->second) {
        lastProcessed_[flowKey] = token.sequence;
    }
    pendingStress_[flowKey] = 0;
    auto sequences = reassemblies_.find(flowKey);
    if (sequences != reassemblies_.end()) {
        while (!sequences->second.empty() &&
               sequences->second.begin()->first <= token.sequence) {
            sequences->second.erase(sequences->second.begin());
        }
        if (sequences->second.empty()) reassemblies_.erase(sequences);
    }
}

void Decoder::discard_message(const MessageCommitToken& token) {
    if (!token.valid) return;
    // A decoded pose which fails expansion/materialization was not processed.
    // Preserve every stress signal so the next successful ACK can make the
    // sender downshift instead of hiding a sustained delta failure.
    pendingStress_[message_flow_key(token.senderId, token.type)] |= token.stressFlags;
}

void Decoder::reset() {
    reassemblies_.clear();
    lastProcessed_.clear();
    pendingStress_.clear();
}

std::string acked_sender_id(const AckPacket& ack) {
    return read_id(ack.ackedSenderId);
}

std::optional<PacketType> peek_packet_type(std::span<const uint8_t> datagram) {
    const auto info = inspect_datagram(datagram);
    return info.has_value() ? std::optional(info->type) : std::nullopt;
}

std::optional<DatagramInfo> inspect_datagram(std::span<const uint8_t> datagram) {
    const std::optional<Header> inspected = inspect_header(datagram);
    if (!inspected.has_value()) return std::nullopt;
    return DatagramInfo{
        static_cast<PacketType>(inspected->type),
        read_id(inspected->senderId),
        inspected->sequence,
    };
}

}  // namespace dusklight_online::net::udp
