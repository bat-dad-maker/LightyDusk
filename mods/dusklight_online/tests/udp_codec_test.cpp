#include "dusklight_online/net/udp_codec.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace udp = dusklight_online::net::udp;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "UDP codec test failed: " << message << '\n';
    std::exit(1);
}

}  // namespace

int main() {
    const nlohmann::json small = {
        {"type", "pose"}, {"sequence", 4}, {"state", {{"stage", "F_SP103"}}}};
    std::string error;
    auto encoded = udp::encode_message(small, "direct1", udp::PacketType::PoseMsgpack, &error);
    if (encoded.size() != 1 || encoded.front().bytes.size() >= 1200) {
        fail("small MessagePack pose did not fit one VPN-safe datagram: " + error);
    }
    udp::Decoder decoder;
    auto decoded = decoder.accept(encoded.front().bytes);
    if (decoded.kind != udp::DecodeKind::Message || decoded.senderId != "direct1" ||
        decoded.sequence != 4 || decoded.message != small) {
        fail("small pose round trip mismatch");
    }
    encoded = udp::encode_message(small, "direct1",
                                  udp::PacketType::SemanticPoseMsgpack, &error);
    decoder.reset();
    decoded = decoder.accept(encoded.front().bytes);
    if (decoded.kind != udp::DecodeKind::Message ||
        decoded.type != udp::PacketType::SemanticPoseMsgpack || decoded.message != small) {
        fail("semantic pose packet type round trip mismatch");
    }
    // A joined peer can send arbitrary visual payloads. Reject nesting before
    // a recursive parser can exhaust the game's stack.
    nlohmann::json nested = nlohmann::json::object({{"leaf", true}});
    for (int i = 0; i < 80; ++i) nested = nlohmann::json::array({std::move(nested)});
    for (const auto format : {udp::PacketType::PoseJson, udp::PacketType::PoseMsgpack}) {
        encoded = udp::encode_message(nested, "direct1", format, &error);
        if (encoded.empty()) fail("could not encode nested visual test: " + error);
        decoder.reset();
        for (const auto& datagram : encoded) {
            if (decoder.accept(datagram.bytes).kind == udp::DecodeKind::Message)
                fail("deeply nested peer visual was accepted");
        }
    }
    const auto semanticAck = udp::encode_ack(
        "receiver", "direct1", udp::PacketType::SemanticPoseMsgpack, 4);
    decoded = decoder.accept(semanticAck.bytes);
    if (decoded.kind != udp::DecodeKind::Ack ||
        decoded.ack.ackedType !=
            static_cast<uint8_t>(udp::PacketType::SemanticPoseMsgpack)) {
        fail("semantic pose acknowledgement round trip failed");
    }

    nlohmann::json values = nlohmann::json::array();
    uint32_t state = 0x12345678U;
    for (int i = 0; i < 16000; ++i) {
        state = state * 1664525U + 1013904223U;
        values.push_back(state);
    }
    const nlohmann::json large = {
        {"type", "pose"}, {"sequence", 77}, {"state", {{"matrix_words", values}}}};
    encoded = udp::encode_message(large, "peer-with-a-long-identity", udp::PacketType::PoseMsgpack,
                                  &error);
    if (encoded.size() < 4 || !encoded.back().parity) {
        fail("large pose did not produce data chunks plus XOR parity: " + error);
    }

    // Drop one data chunk and reorder the rest. The final parity datagram must
    // reconstruct the exact zstd stream.
    decoder.reset();
    udp::DecodeResult recovered;
    for (size_t i = encoded.size(); i-- > 0;) {
        if (i == 1) {
            continue;
        }
        const auto result = decoder.accept(encoded[i].bytes);
        if (result.kind != udp::DecodeKind::None) {
            recovered = result;
        }
    }
    if (recovered.kind != udp::DecodeKind::Message || recovered.message != large ||
        (recovered.stressFlags & udp::AckParityRecovered) == 0) {
        fail("single-loss parity recovery failed");
    }

    const auto ack = udp::encode_ack("receiver", "peer-with-a-long-identity",
                                     udp::PacketType::PoseMsgpack, 77,
                                     udp::AckParityRecovered);
    decoded = decoder.accept(ack.bytes);
    if (decoded.kind != udp::DecodeKind::Ack || decoded.ack.sequence != 77 ||
        decoded.ack.ackedType != static_cast<uint8_t>(udp::PacketType::PoseMsgpack) ||
        udp::acked_sender_id(decoded.ack) != "peer-with-a-long-identity" ||
        decoded.ack.stressFlags != udp::AckParityRecovered) {
        fail("pose acknowledgement round trip failed");
    }

    udp::RemoteObjectPacket object;
    std::memcpy(object.stageName, "D_MN09B", 7);
    object.sequence = 19;
    object.objectId = 501;
    object.x = 1.5f;
    object.y = -2.0f;
    object.z = 30.25f;
    object.room = 2;
    object.objectKind = 1;
    object.flags = udp::ObjectActive;
    const auto objectDatagram = udp::encode_remote_object("direct2", object);
    decoded = decoder.accept(objectDatagram.bytes);
    if (decoded.kind != udp::DecodeKind::RemoteObject || decoded.senderId != "direct2" ||
        decoded.remoteObject.objectId != 501 || decoded.remoteObject.z != 30.25f) {
        fail("remote object round trip failed");
    }

    udp::VoicePosition voicePosition;
    std::memcpy(voicePosition.stageName, "F_SP103", 7);
    voicePosition.x = 12.5f;
    voicePosition.y = -4.0f;
    voicePosition.z = 99.0f;
    voicePosition.room = 3;
    const std::vector<uint8_t> opusFrame{0x78, 0x12, 0x34, 0x56};
    const auto voiceDatagram = udp::encode_voice("speaker", 42, voicePosition, opusFrame);
    decoded = decoder.accept(voiceDatagram.bytes);
    if (decoded.kind != udp::DecodeKind::Voice || decoded.senderId != "speaker" ||
        decoded.sequence != 42 || decoded.voice != opusFrame ||
        decoded.voicePosition.x != 12.5f || decoded.voicePosition.room != 3 ||
        std::strcmp(decoded.voicePosition.stageName, "F_SP103") != 0) {
        fail("voice packet round trip failed");
    }
    auto badVoice = voiceDatagram.bytes;
    badVoice.pop_back();
    if (decoder.accept(badVoice).kind != udp::DecodeKind::None ||
        !udp::encode_voice("speaker", 0, voicePosition, opusFrame).bytes.empty() ||
        !udp::encode_voice("speaker", 43, voicePosition,
                           std::vector<uint8_t>(401, 0)).bytes.empty()) {
        fail("malformed voice packet was accepted");
    }

    const auto registration = udp::encode_relay_registration("relay-client", "secret-token");
    decoded = decoder.accept(registration.bytes);
    if (decoded.kind != udp::DecodeKind::RelayRegistration ||
        decoded.senderId != "relay-client" || decoded.relayToken != "secret-token") {
        fail("relay registration round trip failed");
    }

    auto malformed = registration.bytes;
    malformed.pop_back();
    if (decoder.accept(malformed).kind != udp::DecodeKind::None) {
        fail("truncated datagram was accepted");
    }

    std::cout << "UDP codec test passed\n";
    return 0;
}
