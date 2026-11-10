#include "dusklight_online/game/remote_pose.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dusklight_online::game {
namespace {

using nlohmann::json;
using namespace dusk::multiplayer;

constexpr uint8_t kPackedMatrixModeFloat32 = 0;
constexpr uint8_t kPackedMatrixModeQuantizedBasis = 1;
constexpr uint8_t kPackedModelWeightsIncluded = 1 << 0;
constexpr uint8_t kDeltaUnchanged = 0;
constexpr uint8_t kDeltaFull = 1;
constexpr uint8_t kDeltaMask = 2;
constexpr uint8_t kDeltaResidual = 3;
constexpr uint8_t kResidualUnchanged = 0;
constexpr uint8_t kResidualSmall = 1;
constexpr uint8_t kResidualFull = 2;
constexpr float kResidualTranslationScale = 16.0f;
constexpr size_t kMatrixHistoryLimit = 300;
constexpr uint32_t kSnapshotFullInterval = 120;
std::map<std::string, std::map<uint32_t, std::string>> sMatrixHistory;
std::map<std::string, std::map<uint32_t, json>> sSemanticSnapshotHistory;

bool is_bottle_item(uint16_t item) {
    return (item >= 0x60 && item <= 0x6C) || item == 0x73 || item == 0x74 ||
           item == 0x76 || (item >= 0x77 && item <= 0x7F) || item == 0x9F;
}

bool is_supported_presented_item(uint16_t item) {
    return item == 0x80 || item == 0x81 || item == 0x83 || item == 0x90 ||
           item == 0xE9 || item == 0xEB;
}

bool decode_base64(std::string_view text, std::string& out) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(alphabet[i])] = int8_t(i);
    if (text.empty() || text.size() % 4 != 0) return false;
    out.clear();
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        uint32_t bits = 0;
        int pads = 0;
        for (size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=' && i + 4 == text.size() && j >= 2) {
                ++pads;
                bits <<= 6;
            } else {
                const int value = table[static_cast<uint8_t>(c)];
                if (value < 0 || pads != 0) return false;
                bits = (bits << 6) | uint32_t(value);
            }
        }
        out.push_back(char((bits >> 16) & 0xff));
        if (pads < 2) out.push_back(char((bits >> 8) & 0xff));
        if (pads < 1) out.push_back(char(bits & 0xff));
    }
    return true;
}

bool packed_bytes(const json& source, std::string& out) {
    const auto it = source.find("data");
    if (it == source.end()) return false;
    if (it->is_binary()) {
        const auto& bytes = it->get_binary();
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }
    return it->is_string() && decode_base64(it->get_ref<const std::string&>(), out);
}

std::string history_key(const std::string& peerId, uint8_t packetType) {
    return peerId + '\x1f' + std::to_string(packetType);
}

void store_semantic_snapshot(const std::string& key, uint32_t sequence,
                             const json& message) {
    if (sequence == 0 || !message.is_object()) return;
    auto& history = sSemanticSnapshotHistory[key];
    history[sequence] = message;
    while (history.size() > kMatrixHistoryLimit) history.erase(history.begin());
}

bool expand_semantic_snapshot_delta(json& message, const std::string& key,
                                    uint32_t sequence, std::string& error) {
    if (!message.value("snapshot_delta_v1", false)) return true;
    const uint32_t baselineSequence = message.value("snapshot_base", 0U);
    if (baselineSequence == 0 || baselineSequence >= sequence) {
        error = "semantic snapshot delta has an invalid baseline";
        return false;
    }
    const auto history = sSemanticSnapshotHistory.find(key);
    if (history == sSemanticSnapshotHistory.end()) {
        error = "semantic snapshot delta has no baseline history";
        return false;
    }
    const auto baseline = history->second.find(baselineSequence);
    if (baseline == history->second.end()) {
        error = "semantic snapshot delta baseline is unavailable";
        return false;
    }
    const auto baselineState = baseline->second.find("state");
    const auto patch = message.find("state");
    if (baselineState == baseline->second.end() || !baselineState->is_object() ||
        patch == message.end() || !patch->is_object()) {
        error = "semantic snapshot delta state is invalid";
        return false;
    }
    json expandedState = *baselineState;
    for (auto it = patch->begin(); it != patch->end(); ++it) {
        expandedState[it.key()] = it.value();
    }
    const auto removed = message.find("snapshot_removed");
    if (removed != message.end()) {
        if (!removed->is_array()) {
            error = "semantic snapshot delta removals are invalid";
            return false;
        }
        for (const auto& name : *removed) {
            if (!name.is_string()) {
                error = "semantic snapshot delta removal key is invalid";
                return false;
            }
            expandedState.erase(name.get_ref<const std::string&>());
        }
    }
    message["state"] = std::move(expandedState);
    message.erase("snapshot_delta_v1");
    message.erase("snapshot_base");
    message.erase("snapshot_removed");
    return true;
}

std::optional<std::string> base_format_for_delta(const std::string& format) {
    if (format == "qrot16_trans32_womit_delta_bin_v1")
        return "qrot16_trans32_womit_bin_v1";
    if (format == "qrot16_trans32_womit_delta_v1")
        return "qrot16_trans32_womit_v1";
    if (format == "f32_pack_womit_delta_bin_v1") return "f32_pack_womit_bin_v1";
    if (format == "f32_pack_womit_delta_v1") return "f32_pack_womit_v1";
    return std::nullopt;
}

std::optional<std::string> delta_format_for_base(const std::string& format) {
    if (format == "qrot16_trans32_womit_bin_v1")
        return "qrot16_trans32_womit_delta_bin_v1";
    if (format == "qrot16_trans32_womit_v1")
        return "qrot16_trans32_womit_delta_v1";
    if (format == "f32_pack_womit_bin_v1") return "f32_pack_womit_delta_bin_v1";
    if (format == "f32_pack_womit_v1") return "f32_pack_womit_delta_v1";
    return std::nullopt;
}

bool is_delta_capable_base(const std::string& format) {
    return format == "qrot16_trans32_womit_bin_v1" ||
           format == "qrot16_trans32_womit_v1" ||
           format == "f32_pack_womit_bin_v1" || format == "f32_pack_womit_v1";
}

size_t matrix_bytes_for_format(const std::string& format) {
    if (format == "qrot16_trans32_womit_bin_v1" ||
        format == "qrot16_trans32_womit_v1") return 30;
    if (format == "f32_pack_womit_bin_v1" || format == "f32_pack_womit_v1") return 48;
    return 0;
}

bool read_bytes(const std::string& source, size_t& cursor, void* out, size_t size) {
    if (cursor > source.size() || size > source.size() - cursor) return false;
    std::memcpy(out, source.data() + cursor, size);
    cursor += size;
    return true;
}

template <typename T>
bool read_value(const std::string& source, size_t& cursor, T& out) {
    return read_bytes(source, cursor, &out, sizeof(out));
}

bool split_slots(const std::string& format, const std::string& packed,
                 std::string& header, std::vector<std::string>& slots) {
    const size_t matrixBytes = matrix_bytes_for_format(format);
    if (matrixBytes == 0) return false;
    size_t cursor = 0;
    char magic[4]{};
    uint8_t version = 0, count = 0;
    if (!read_bytes(packed, cursor, magic, 4) || std::memcmp(magic, "DMPM", 4) != 0 ||
        !read_value(packed, cursor, version) || !read_value(packed, cursor, count) ||
        version != 1 || count != 21) return false;
    header.assign(packed.data(), cursor);
    slots.clear();
    slots.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        const size_t start = cursor;
        uint8_t present = 0;
        if (!read_value(packed, cursor, present)) return false;
        if (present == 0) { slots.emplace_back(packed.data() + start, cursor - start); continue; }
        uint16_t joints = 0, weights = 0;
        uint8_t flags = 0;
        if (present != 1 || !read_value(packed, cursor, joints) ||
            !read_value(packed, cursor, weights) || !read_value(packed, cursor, flags)) return false;
        const bool hasWeights = (flags & kPackedModelWeightsIncluded) != 0 || weights == 0;
        const size_t matrices = 1 + size_t(joints) + (hasWeights ? size_t(weights) : 0);
        if (matrices > 1024 || cursor > packed.size() ||
            matrices * matrixBytes > packed.size() - cursor) return false;
        cursor += matrices * matrixBytes;
        slots.emplace_back(packed.data() + start, cursor - start);
    }
    return cursor == packed.size();
}

size_t slot_matrix_count(const std::string& slot, size_t matrixBytes) {
    if (slot.size() < 6 || uint8_t(slot[0]) != 1) return 0;
    size_t cursor = 1;
    uint16_t joints = 0, weights = 0;
    uint8_t flags = 0;
    if (!read_value(slot, cursor, joints) || !read_value(slot, cursor, weights) ||
        !read_value(slot, cursor, flags)) return 0;
    const bool hasWeights = (flags & kPackedModelWeightsIncluded) != 0 || weights == 0;
    const size_t count = 1 + size_t(joints) + (hasWeights ? size_t(weights) : 0);
    return count <= 1024 && slot.size() == 6 + count * matrixBytes ? count : 0;
}

template <typename T>
void append_value(std::string& target, const T& value) {
    target.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool build_mask_delta(const std::string& current, const std::string& baseline,
                      size_t matrixBytes, std::string& payload) {
    if (current.size() < 6 || baseline.size() < 6 ||
        current.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(current, matrixBytes);
    if (count == 0 || count != slot_matrix_count(baseline, matrixBytes)) return false;
    const size_t maskBytes = (count + 7) / 8;
    payload.assign(current.data(), 6);
    const size_t maskOffset = payload.size();
    payload.append(maskBytes, '\0');
    size_t changed = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = 6 + i * matrixBytes;
        if (std::memcmp(current.data() + offset, baseline.data() + offset,
                        matrixBytes) == 0) continue;
        payload[maskOffset + i / 8] = static_cast<char>(
            static_cast<uint8_t>(payload[maskOffset + i / 8]) |
            static_cast<uint8_t>(1u << (i % 8)));
        payload.append(current.data() + offset, matrixBytes);
        ++changed;
    }
    return changed != 0 && payload.size() < current.size();
}

bool build_small_residual(const char* current, const char* baseline,
                          std::string& payload) {
    for (size_t i = 0; i < 9; ++i) {
        int16_t now = 0, old = 0;
        std::memcpy(&now, current + i * sizeof(int16_t), sizeof(now));
        std::memcpy(&old, baseline + i * sizeof(int16_t), sizeof(old));
        const int delta = int(now) - int(old);
        if (delta < (std::numeric_limits<int8_t>::min)() ||
            delta > (std::numeric_limits<int8_t>::max)()) return false;
        append_value(payload, static_cast<int8_t>(delta));
    }
    for (size_t i = 0; i < 3; ++i) {
        float now = 0.0f, old = 0.0f;
        std::memcpy(&now, current + 18 + i * sizeof(float), sizeof(now));
        std::memcpy(&old, baseline + 18 + i * sizeof(float), sizeof(old));
        const float scaled = std::round((now - old) * kResidualTranslationScale);
        if (!std::isfinite(scaled) ||
            scaled < float((std::numeric_limits<int16_t>::min)()) ||
            scaled > float((std::numeric_limits<int16_t>::max)())) return false;
        append_value(payload, static_cast<int16_t>(scaled));
    }
    return true;
}

bool build_residual_delta(const std::string& current, const std::string& baseline,
                          size_t matrixBytes, std::string& payload) {
    if (matrixBytes != 30 || current.size() < 6 || baseline.size() < 6 ||
        current.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(current, matrixBytes);
    if (count == 0 || count != slot_matrix_count(baseline, matrixBytes)) return false;
    payload.assign(current.data(), 6);
    const size_t modesOffset = payload.size();
    payload.append(count, static_cast<char>(kResidualUnchanged));
    size_t changed = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t offset = 6 + i * matrixBytes;
        const char* now = current.data() + offset;
        const char* old = baseline.data() + offset;
        if (std::memcmp(now, old, matrixBytes) == 0) continue;
        std::string residual;
        if (build_small_residual(now, old, residual) && residual.size() == 15) {
            payload[modesOffset + i] = static_cast<char>(kResidualSmall);
            payload += residual;
        } else {
            payload[modesOffset + i] = static_cast<char>(kResidualFull);
            payload.append(now, matrixBytes);
        }
        ++changed;
    }
    return changed != 0 && payload.size() < current.size();
}

bool build_slot_delta(const std::string& format, const std::string& current,
                      const std::string& baseline, uint32_t baselineSequence,
                      std::string& delta) {
    const size_t matrixBytes = matrix_bytes_for_format(format);
    std::string currentHeader, baselineHeader;
    std::vector<std::string> currentSlots, baselineSlots;
    if (matrixBytes == 0 ||
        !split_slots(format, current, currentHeader, currentSlots) ||
        !split_slots(format, baseline, baselineHeader, baselineSlots) ||
        currentHeader != baselineHeader || currentSlots.size() != baselineSlots.size()) {
        return false;
    }
    delta = "DMPD";
    append_value(delta, uint8_t{1});
    append_value(delta, static_cast<uint8_t>(currentSlots.size()));
    append_value(delta, baselineSequence);
    for (size_t i = 0; i < currentSlots.size(); ++i) {
        if (currentSlots[i] == baselineSlots[i]) {
            append_value(delta, kDeltaUnchanged);
            continue;
        }
        std::string payload;
        uint8_t mode = kDeltaFull;
        if (build_residual_delta(currentSlots[i], baselineSlots[i], matrixBytes, payload)) {
            mode = kDeltaResidual;
        } else if (build_mask_delta(currentSlots[i], baselineSlots[i], matrixBytes, payload)) {
            mode = kDeltaMask;
        } else {
            payload = currentSlots[i];
        }
        if (payload.size() > (std::numeric_limits<uint16_t>::max)()) return false;
        append_value(delta, mode);
        append_value(delta, static_cast<uint16_t>(payload.size()));
        delta += payload;
    }
    return true;
}

bool apply_mask_delta(const std::string& payload, const std::string& baseline,
                      size_t matrixBytes, std::string& current) {
    if (payload.size() < 6 || baseline.size() < 6 ||
        payload.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(baseline, matrixBytes);
    if (count == 0) return false;
    const size_t maskBytes = (count + 7) / 8;
    size_t cursor = 6 + maskBytes;
    if (payload.size() < cursor) return false;
    current = baseline;
    for (size_t i = 0; i < count; ++i) {
        if ((uint8_t(payload[6 + i / 8]) & uint8_t(1u << (i % 8))) == 0) continue;
        if (matrixBytes > payload.size() - cursor) return false;
        std::memcpy(current.data() + 6 + i * matrixBytes,
                    payload.data() + cursor, matrixBytes);
        cursor += matrixBytes;
    }
    return cursor == payload.size();
}

void apply_small_residual(const char* baseline, const char* residual, char* current) {
    for (size_t i = 0; i < 9; ++i) {
        int16_t value = 0;
        std::memcpy(&value, baseline + i * sizeof(int16_t), sizeof(value));
        value = int16_t(int(value) + int(*reinterpret_cast<const int8_t*>(residual + i)));
        std::memcpy(current + i * sizeof(int16_t), &value, sizeof(value));
    }
    for (size_t i = 0; i < 3; ++i) {
        float base = 0.0f;
        int16_t delta = 0;
        std::memcpy(&base, baseline + 18 + i * sizeof(float), sizeof(base));
        std::memcpy(&delta, residual + 9 + i * sizeof(int16_t), sizeof(delta));
        const float value = base + float(delta) / kResidualTranslationScale;
        std::memcpy(current + 18 + i * sizeof(float), &value, sizeof(value));
    }
}

bool apply_residual_delta(const std::string& payload, const std::string& baseline,
                          size_t matrixBytes, std::string& current) {
    if (matrixBytes != 30 || payload.size() < 6 || baseline.size() < 6 ||
        payload.compare(0, 6, baseline, 0, 6) != 0) return false;
    const size_t count = slot_matrix_count(baseline, matrixBytes);
    if (count == 0 || payload.size() < 6 + count) return false;
    current = baseline;
    size_t cursor = 6 + count;
    constexpr size_t smallBytes = 15;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t mode = uint8_t(payload[6 + i]);
        char* destination = current.data() + 6 + i * matrixBytes;
        if (mode == kResidualUnchanged) continue;
        if (mode == kResidualSmall) {
            if (smallBytes > payload.size() - cursor) return false;
            apply_small_residual(baseline.data() + 6 + i * matrixBytes,
                                 payload.data() + cursor, destination);
            cursor += smallBytes;
        } else if (mode == kResidualFull) {
            if (matrixBytes > payload.size() - cursor) return false;
            std::memcpy(destination, payload.data() + cursor, matrixBytes);
            cursor += matrixBytes;
        } else return false;
    }
    return cursor == payload.size();
}

bool apply_slot_delta(const std::string& deltaFormat, const std::string& delta,
                      const std::string& baseline, uint32_t expectedSequence,
                      std::string& current) {
    const auto baseFormat = base_format_for_delta(deltaFormat);
    if (!baseFormat) return false;
    const size_t matrixBytes = matrix_bytes_for_format(*baseFormat);
    std::string header;
    std::vector<std::string> baselineSlots;
    if (!split_slots(*baseFormat, baseline, header, baselineSlots)) return false;
    size_t cursor = 0;
    char magic[4]{};
    uint8_t version = 0, count = 0;
    uint32_t sequence = 0;
    if (!read_bytes(delta, cursor, magic, 4) || std::memcmp(magic, "DMPD", 4) != 0 ||
        !read_value(delta, cursor, version) || !read_value(delta, cursor, count) ||
        !read_value(delta, cursor, sequence) || version != 1 ||
        sequence != expectedSequence || count != baselineSlots.size()) return false;
    current = header;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t mode = 0;
        if (!read_value(delta, cursor, mode)) return false;
        if (mode == kDeltaUnchanged) { current += baselineSlots[i]; continue; }
        uint16_t bytes = 0;
        if ((mode != kDeltaFull && mode != kDeltaMask && mode != kDeltaResidual) ||
            !read_value(delta, cursor, bytes) || bytes > delta.size() - cursor) return false;
        if (mode == kDeltaFull) current.append(delta.data() + cursor, bytes);
        else {
            const std::string payload(delta.data() + cursor, bytes);
            std::string slot;
            const bool ok = mode == kDeltaMask ?
                apply_mask_delta(payload, baselineSlots[i], matrixBytes, slot) :
                apply_residual_delta(payload, baselineSlots[i], matrixBytes, slot);
            if (!ok) return false;
            current += slot;
        }
        cursor += bytes;
    }
    return cursor == delta.size();
}

float dequantize_basis(int16_t value) {
    return static_cast<float>(value) / 32767.0f;
}

bool read_quantized_matrix(const std::string& source, size_t& cursor, float* out) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            int16_t value = 0;
            if (!read_value(source, cursor, value)) return false;
            out[row * 4 + col] = dequantize_basis(value);
        }
    }
    for (int row = 0; row < 3; ++row) {
        if (!read_value(source, cursor, out[row * 4 + 3])) return false;
    }
    return true;
}

bool read_matrix(const std::string& source, size_t& cursor, float* out,
                 bool quantized, bool safeQuantized) {
    if (safeQuantized) {
        uint8_t mode = 0;
        if (!read_value(source, cursor, mode)) return false;
        if (mode == kPackedMatrixModeFloat32)
            return read_bytes(source, cursor, out, sizeof(float) * 12);
        if (mode != kPackedMatrixModeQuantizedBasis) return false;
        return read_quantized_matrix(source, cursor, out);
    }
    return quantized ? read_quantized_matrix(source, cursor, out) :
                       read_bytes(source, cursor, out, sizeof(float) * 12);
}

bool read_model(const std::string& packed, size_t& cursor,
                RemoteModelMatrixSnapshot& model, bool quantized,
                bool safeQuantized, bool omitWeights) {
    model = {};
    uint8_t present = 0;
    if (!read_value(packed, cursor, present)) return false;
    if (present == 0) return true;
    if (present != 1 || !read_value(packed, cursor, model.jointCount) ||
        !read_value(packed, cursor, model.weightCount)) return false;
    bool weightsIncluded = true;
    if (omitWeights) {
        uint8_t flags = 0;
        if (!read_value(packed, cursor, flags)) return false;
        weightsIncluded = (flags & kPackedModelWeightsIncluded) != 0 || model.weightCount == 0;
        model.weightsOmitted = !weightsIncluded && model.weightCount > 0;
    }
    const size_t jointFloats = size_t(model.jointCount) * 12;
    const size_t weightFloats = size_t(model.weightCount) * 12;
    if (jointFloats > 64 * 1024 * 12 || weightFloats > 64 * 1024 * 12) return false;
    model.joints.resize(jointFloats);
    model.weights.resize(weightsIncluded ? weightFloats : 0);
    if (!read_matrix(packed, cursor, model.base.data(), quantized, safeQuantized)) return false;
    for (size_t i = 0; i < model.jointCount; ++i)
        if (!read_matrix(packed, cursor, model.joints.data() + i * 12,
                         quantized, safeQuantized)) return false;
    if (weightsIncluded) {
        for (size_t i = 0; i < model.weightCount; ++i)
            if (!read_matrix(packed, cursor, model.weights.data() + i * 12,
                             quantized, safeQuantized)) return false;
    }
    model.valid = true;
    return true;
}

bool read_matrix_array(const json& source, std::array<float, 12>& out) {
    if (!source.is_array() || source.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        if (!source[i].is_number()) return false;
        out[i] = source[i].get<float>();
    }
    return true;
}

RemoteModelMatrixSnapshot parse_json_model(const json& source) {
    RemoteModelMatrixSnapshot out;
    if (!source.is_object()) return out;
    const int joints = source.value("joint_count", -1);
    const int weights = source.value("weight_count", -1);
    if (joints < 0 || weights < 0 || joints > 65535 || weights > 65535) return out;
    out.jointCount = uint16_t(joints);
    out.weightCount = uint16_t(weights);
    const auto jointData = source.value("joints", json::array());
    const auto weightData = source.value("weights", json::array());
    if (!read_matrix_array(source.value("base", json::array()), out.base) ||
        !jointData.is_array() || jointData.size() != size_t(joints) * 12 ||
        !weightData.is_array() || weightData.size() != size_t(weights) * 12) return {};
    out.joints.reserve(jointData.size());
    out.weights.reserve(weightData.size());
    for (const auto& value : jointData) {
        if (!value.is_number()) return {};
        out.joints.push_back(value.get<float>());
    }
    for (const auto& value : weightData) {
        if (!value.is_number()) return {};
        out.weights.push_back(value.get<float>());
    }
    out.valid = true;
    return out;
}

RemoteLinkMatrixSnapshot parse_matrices(const json& source) {
    RemoteLinkMatrixSnapshot out;
    if (!source.is_object()) return out;
    const std::string format = source.value("format", "");
    const bool quantized = format == "qrot16_trans32_v1" ||
        format == "qrot16_trans32_bin_v1" ||
        format == "qrot16_trans32_womit_v1" ||
        format == "qrot16_trans32_womit_bin_v1";
    const bool safe = format == "qbasis16_trans32_safe_v1" ||
        format == "qbasis16_trans32_safe_bin_v1";
    const bool floating = format == "f32_pack_v1" || format == "f32_pack_bin_v1" ||
        format == "f32_pack_womit_v1" || format == "f32_pack_womit_bin_v1";
    const bool omit = format == "qrot16_trans32_womit_v1" ||
        format == "qrot16_trans32_womit_bin_v1" ||
        format == "f32_pack_womit_v1" || format == "f32_pack_womit_bin_v1";
    const bool packedFormat = quantized || safe || floating;
    if (packedFormat) {
        std::string packed;
        if (!packed_bytes(source, packed)) return out;
        size_t cursor = 0;
        char magic[4]{};
        uint8_t version = 0, count = 0;
        if (!read_bytes(packed, cursor, magic, 4) || std::memcmp(magic, "DMPM", 4) != 0 ||
            !read_value(packed, cursor, version) || !read_value(packed, cursor, count) ||
            version != 1 || count != 21) return {};
        RemoteModelMatrixSnapshot ignoredMidna[5];
        RemoteModelMatrixSnapshot* slots[] = {
            &out.body, &out.hat, &out.face, &out.hand, &out.sword, &out.sheath,
            &out.shield, &out.heldItem, &out.hookTip, &out.hookSubItem, &out.hookSubTip,
            &out.arrow, &out.kantera, &out.kanteraGlow, &out.itemActor, &out.rideActor,
            &ignoredMidna[0], &ignoredMidna[1], &ignoredMidna[2], &ignoredMidna[3],
            &ignoredMidna[4],
        };
        for (auto* slot : slots)
            if (!read_model(packed, cursor, *slot, quantized, safe, omit)) return {};
        if (cursor != packed.size()) return {};
    } else {
        out.body = parse_json_model(source.value("body", json::object()));
        out.hat = parse_json_model(source.value("hat", json::object()));
        out.face = parse_json_model(source.value("face", json::object()));
        out.hand = parse_json_model(source.value("hand", json::object()));
        out.sword = parse_json_model(source.value("sword", json::object()));
        out.sheath = parse_json_model(source.value("sheath", json::object()));
        out.shield = parse_json_model(source.value("shield", json::object()));
        out.heldItem = parse_json_model(source.value("held_item", json::object()));
        out.hookTip = parse_json_model(source.value("hook_tip", json::object()));
        out.hookSubItem = parse_json_model(source.value("hook_sub_item", json::object()));
        out.hookSubTip = parse_json_model(source.value("hook_sub_tip", json::object()));
        out.arrow = parse_json_model(source.value("arrow", json::object()));
        out.kantera = parse_json_model(source.value("kantera", json::object()));
        out.kanteraGlow = parse_json_model(source.value("kantera_glow", json::object()));
        out.itemActor = parse_json_model(source.value("item_actor", json::object()));
        out.rideActor = parse_json_model(source.value("ride_actor", json::object()));
    }
    out.midnaHairShape = 0;
    // A packed semantic pose may intentionally contain only small attachment
    // slots, or all-absent slots to explicitly clear old props. Successful
    // container decoding is therefore valid independently of the body slot.
    out.valid = packedFormat || out.body.valid;
    return out;
}

bool hydrate_model(RemoteModelMatrixSnapshot& model,
                   const RemoteModelMatrixSnapshot& previous) {
    if (!model.valid || !model.weightsOmitted) return true;
    if (!previous.valid || previous.weightCount != model.weightCount ||
        previous.weights.size() != size_t(model.weightCount) * 12) return false;
    model.weights = previous.weights;
    model.weightsOmitted = false;
    return true;
}

void hydrate_matrices(RemoteLinkMatrixSnapshot& value,
                      const RemoteLinkMatrixSnapshot& previous) {
    RemoteModelMatrixSnapshot* current[] = {
        &value.body, &value.hat, &value.face, &value.hand, &value.sword, &value.sheath,
        &value.shield, &value.heldItem, &value.hookTip, &value.hookSubItem, &value.hookSubTip,
        &value.arrow, &value.kantera, &value.kanteraGlow, &value.itemActor, &value.rideActor,
    };
    const RemoteModelMatrixSnapshot* old[] = {
        &previous.body, &previous.hat, &previous.face, &previous.hand, &previous.sword,
        &previous.sheath, &previous.shield, &previous.heldItem, &previous.hookTip,
        &previous.hookSubItem, &previous.hookSubTip, &previous.arrow, &previous.kantera,
        &previous.kanteraGlow, &previous.itemActor, &previous.rideActor,
    };
    for (size_t i = 0; i < std::size(current); ++i) hydrate_model(*current[i], *old[i]);
}

template <size_t N>
void parse_i16_array(const json& state, const char* key, std::array<int16_t, N>& out) {
    const auto it = state.find(key);
    if (it == state.end() || !it->is_array() || it->size() != N) return;
    for (size_t i = 0; i < N; ++i) {
        if (!(*it)[i].is_number_integer()) return;
        const int value = (*it)[i].get<int>();
        if (value < -32768 || value > 32767) return;
        out[i] = int16_t(value);
    }
}

std::vector<RemoteAudioEvent> parse_audio(const json& state, const char* key, bool active) {
    std::vector<RemoteAudioEvent> out;
    const auto it = state.find(key);
    if (it == state.end() || !it->is_array()) return out;
    for (const auto& entry : *it) {
        if (!entry.is_object() || out.size() >= 8) continue;
        RemoteAudioEvent event;
        event.sequence = entry.value("seq", 0U);
        event.soundId = entry.value("sound_id", 0U);
        event.mapInfo = entry.value("mapinfo", 0U);
        event.reverb = int8_t(std::clamp(entry.value("reverb", -1), -1, 127));
        event.sourceKind = uint8_t(std::clamp(entry.value("source", 0), 0, 255));
        event.tracked = active && entry.value("tracked", false) && event.sequence != 0;
        event.level = !event.tracked && (active || entry.value("level", false));
        if (event.soundId != 0 && (active || event.sequence != 0)) out.push_back(event);
    }
    return out;
}

}  // namespace

bool decode_peer_pose(const json& message, const std::string& peerId,
                      const PeerPoseSnapshot* previous, PeerPoseSnapshot& pose,
                      std::string& error) {
    try {
        if (!message.is_object() || message.value("type", "") != "pose") {
            error = "not a pose message";
            return false;
        }
        const json state = message.value("state", json::object());
        if (!state.is_object()) { error = "pose state is not an object"; return false; }
        pose = {};
        pose.valid = true;
        pose.peerId = peerId;
        pose.sequence = message.value("sequence", 0U);
        const std::string visualMode = state.value("visual_mode", "");
        if (visualMode == "semantic_gameplay") {
            pose.visualMode = PeerPoseSnapshot::VisualMode::SemanticGameplay;
        } else if (visualMode == "hidden_unsupported") {
            pose.visualMode = PeerPoseSnapshot::VisualMode::HiddenUnsupported;
        } else {
            pose.visualMode = PeerPoseSnapshot::VisualMode::Unknown;
        }
        pose.visualUnsupportedReasons = state.value("visual_unsupported_reasons", 0U);
        pose.stage = state.value("stage", "");
        pose.room = state.value("room", -1);
        pose.layer = state.value("layer", -1);
        pose.x = state.value("x", 0.0f); pose.y = state.value("y", 0.0f);
        pose.z = state.value("z", 0.0f); pose.angleY = state.value("angle_y", 0);
        if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.z)) {
            error = "pose contains non-finite coordinates"; return false;
        }
        pose.finalGanondorfReady = state.value("final_ganondorf_ready", false);
        pose.procId = state.value("proc_id", 0); pose.procVar0 = state.value("proc_v0", 0);
        pose.procVar1 = state.value("proc_v1", 0); pose.procVar2 = state.value("proc_v2", 0);
        pose.procVar3 = state.value("proc_v3", 0); pose.procVar5 = state.value("proc_v5", 0);
        pose.cutType = state.value("cut_type", 0); pose.cutCount = state.value("cut_count", 0);
        pose.jumpCancelTurn = state.value("jump_cancel_turn", false);
        pose.manualSyncReady = state.value("manual_sync_ready", false);
        pose.underFrame = state.value("under_frame", 0.0f);
        pose.underBck0 = state.value("under_bck0", 0);
        pose.underBckArc0 = state.value("under_arc0", 0xFFFF);
        pose.underFrame0 = state.value("under_frame0", pose.underFrame);
        pose.underRate0 = state.value("under_rate0", 1.0f);
        pose.underRatio0 = state.value("under_ratio0", 1.0f);
        pose.underBck1 = state.value("under_bck1", 0);
        pose.underBckArc1 = state.value("under_arc1", 0xFFFF);
        pose.underFrame1 = state.value("under_frame1", 0.0f);
        pose.underRate1 = state.value("under_rate1", 1.0f);
        pose.underRatio1 = state.value("under_ratio1", 0.0f);
        pose.underBck2 = state.value("under_bck2", 0);
        pose.underBckArc2 = state.value("under_arc2", 0xFFFF);
        pose.underFrame2 = state.value("under_frame2", 0.0f);
        pose.underRate2 = state.value("under_rate2", 1.0f);
        pose.underRatio2 = state.value("under_ratio2", 0.0f);
        pose.upperBck0 = state.value("upper_bck0", 0);
        pose.upperBckArc0 = state.value("upper_arc0", 0xFFFF);
        pose.upperFrame0 = state.value("upper_frame0", 0.0f);
        pose.upperRate0 = state.value("upper_rate0", 1.0f);
        pose.upperRatio0 = state.value("upper_ratio0", 1.0f);
        pose.upperBck1 = state.value("upper_bck1", 0);
        pose.upperBckArc1 = state.value("upper_arc1", 0xFFFF);
        pose.upperFrame1 = state.value("upper_frame1", 0.0f);
        pose.upperRate1 = state.value("upper_rate1", 1.0f);
        pose.upperRatio1 = state.value("upper_ratio1", 0.0f);
        pose.upperBck2 = state.value("upper_bck2", 0);
        pose.upperBckArc2 = state.value("upper_arc2", 0xFFFF);
        pose.upperFrame2 = state.value("upper_frame2", 0.0f);
        pose.upperRate2 = state.value("upper_rate2", 1.0f);
        pose.upperRatio2 = state.value("upper_ratio2", 0.0f);
        pose.faceBck = state.value("face_bck", 0);
        pose.faceBckArc = state.value("face_bck_arc", 0xFFFF);
        pose.faceBckFrame = state.value("face_bck_frame", 0.0f);
        pose.faceBtp = state.value("face_btp", 0);
        pose.faceBtpArc = state.value("face_btp_arc", 0xFFFF);
        pose.faceBtpFrame = state.value("face_btp_frame", 0.0f);
        pose.faceBtk = state.value("face_btk", 0);
        pose.faceBtkArc = state.value("face_btk_arc", 0xFFFF);
        pose.faceBtkFrame = state.value("face_btk_frame", 0.0f);
        const float animationValues[] = {
            pose.underFrame, pose.underFrame0, pose.underRate0, pose.underRatio0,
            pose.underFrame1, pose.underRate1, pose.underRatio1, pose.underFrame2,
            pose.underRate2, pose.underRatio2, pose.upperFrame0, pose.upperRate0,
            pose.upperRatio0, pose.upperFrame1, pose.upperRate1, pose.upperRatio1,
            pose.upperFrame2, pose.upperRate2, pose.upperRatio2,
            pose.faceBckFrame, pose.faceBtpFrame, pose.faceBtkFrame,
        };
        if (std::any_of(std::begin(animationValues), std::end(animationValues),
                        [](float value) { return !std::isfinite(value); })) {
            error = "pose contains non-finite animation state";
            return false;
        }
        parse_i16_array(state, "hat_rot_a", pose.hatRotA);
        parse_i16_array(state, "hat_rot_b", pose.hatRotB);
        parse_i16_array(state, "hat_swing", pose.hatSwing);
        pose.hatShapeY = state.value("hat_shape_y", 0);
        pose.shapeAngleX = state.value("shape_angle_x", 0);
        pose.shapeAngleZ = state.value("shape_angle_z", 0);
        pose.bodyAngleX = state.value("body_angle_x", 0);
        pose.bodyAngleY = state.value("body_angle_y", 0);
        pose.bodyAngleZ = state.value("body_angle_z", 0);
        pose.bodyTwistY = state.value("body_twist_y", 0);
        pose.neckJointX = state.value("neck_joint_x", 0);
        pose.neckJointY = state.value("neck_joint_y", 0);
        pose.neckJointZ = state.value("neck_joint_z", 0);
        pose.lowerJointX = state.value("lower_joint_x", 0);
        pose.lowerJointZ = state.value("lower_joint_z", 0);
        pose.rootJointX = state.value("root_joint_x", 0);
        pose.rootJointZ = state.value("root_joint_z", 0);
        pose.blendMode = state.value("blend_mode", 0);
        pose.upperSavedRatio = state.value("upper_saved_ratio", 0.0f);
        pose.bodyRootValid = state.value("body_root_valid", false);
        pose.bodyRootX = state.value("body_root_x", 0.0f);
        pose.bodyRootY = state.value("body_root_y", 0.0f);
        pose.bodyRootZ = state.value("body_root_z", 0.0f);
        if (!std::isfinite(pose.upperSavedRatio) ||
            (pose.bodyRootValid &&
             (!std::isfinite(pose.bodyRootX) || !std::isfinite(pose.bodyRootY) ||
              !std::isfinite(pose.bodyRootZ)))) {
            error = "pose contains non-finite body animation state";
            return false;
        }
        parse_i16_array(state, "leg_ik_angles", pose.legIkAngles);
        parse_i16_array(state, "arm_ik_angles", pose.armIkAngles);
        parse_i16_array(state, "arm_rot_a", pose.armRotA);
        parse_i16_array(state, "arm_rot_b", pose.armRotB);
        parse_i16_array(state, "fishing_arm_1", pose.fishingArm1Angle);
        parse_i16_array(state, "fishing_arm_2", pose.fishingArm2Angle);
        if (state.contains("water_drop_state")) {
            const auto& wet = state["water_drop_state"];
            if (!wet.is_array() || wet.size() != 6) {
                error = "pose contains invalid wet timer state";
                return false;
            }
            for (size_t i = 0; i < 6; ++i) {
                const int64_t minimum = i % 3 == 0 ? 0 : (i % 3 == 1 ? -1 : -20);
                const int64_t maximum = i % 3 == 0 ? pose.sequence : (i % 3 == 1 ? 150 : 0);
                if (!wet[i].is_number_integer() || wet[i] < minimum || wet[i] > maximum) {
                    error = "pose contains invalid wet timer state";
                    return false;
                }
                pose.waterDropState[i] = wet[i].get<int64_t>();
            }
        }
        pose.waterDropStrong = state.value("water_drop_strong", false);
        if (state.contains("power_up_timer")) {
            const auto& timer = state["power_up_timer"];
            if (!timer.is_number_integer() || timer < 0 ||
                timer > std::numeric_limits<uint16_t>::max()) {
                error = "pose contains invalid power-up timer";
                return false;
            }
            pose.powerUpTimer = timer.get<uint16_t>();
        }
        if (state.contains("power_up_intensity")) {
            const auto& intensity = state["power_up_intensity"];
            if (!intensity.is_number()) {
                error = "pose contains invalid power-up intensity";
                return false;
            }
            pose.powerUpIntensity = intensity.get<float>();
            if (!std::isfinite(pose.powerUpIntensity) || pose.powerUpIntensity < 0.0f ||
                pose.powerUpIntensity > 1.0f) {
                error = "pose contains invalid power-up intensity";
                return false;
            }
        }
        pose.isWolf = state.value("is_wolf", false);
        pose.isTransforming = state.value("is_transforming", false);
        pose.transformFromWolf = state.value("transform_from_wolf", pose.isWolf);
        pose.transformToWolf = state.value("transform_to_wolf", !pose.transformFromWolf);
        pose.transformProcVar0 = state.value("transform_proc_v0", 0);
        pose.transformProcVar5 = state.value("transform_proc_v5", 0);
        pose.transformClothesWait = state.value("transform_clothes_wait", 0);
        pose.transformFrame = state.value("transform_frame", 0.0f);
        if (!std::isfinite(pose.transformFrame)) {
            error = "pose contains non-finite transformation state";
            return false;
        }
        pose.transformProcVar2 = state.value("transform_proc_v2", 0);
        pose.transformProcVar3 = state.value("transform_proc_v3", 0);
        pose.transformShapeX = state.value("transform_shape_x", 0);
        const int64_t equipItem = state.value("equip_item", int64_t{0xffff});
        if (equipItem < 0 || equipItem > std::numeric_limits<uint16_t>::max()) {
            error = "pose contains invalid equipped item";
            return false;
        }
        pose.equipItem = static_cast<uint16_t>(equipItem);
        const int64_t presentedItem = state.value("presented_item", int64_t{0xffff});
        if (presentedItem < 0 || presentedItem > 0xffff ||
            (presentedItem != 0xffff &&
             !is_supported_presented_item(static_cast<uint16_t>(presentedItem)))) {
            error = "pose contains invalid presented item";
            return false;
        }
        pose.presentedItem = static_cast<uint16_t>(presentedItem);
        pose.swordVariant = state.value("sword_variant", 0);
        pose.shieldVariant = state.value("shield_variant", 0);
        pose.clothesVariant = state.value("clothes_variant", 0);
        pose.swordDraw = state.value("sword_draw", false);
        pose.shieldDraw = state.value("shield_draw", false);
        pose.shieldGuardActive = state.value("shield_guard_active", false);
        pose.swordOut = state.value("sword_out", false);
        pose.swordHandAttached = state.value("sword_hand_attached", pose.swordOut);
        pose.shieldHandAttached = state.value("shield_hand_attached", false);
        pose.leftHandShape = state.value("left_hand_shape", -1);
        pose.rightHandShape = state.value("right_hand_shape", -1);
        if (pose.leftHandShape < -1 || pose.leftHandShape > 254 ||
            pose.rightHandShape < -1 || pose.rightHandShape > 254) {
            error = "pose contains invalid hand shape";
            return false;
        }
        pose.midnaDraw = false;
        pose.midnaMaskDraw = false;
        pose.midnaHandDraw = false;
        pose.midnaHairDraw = false;
        pose.midnaShadowForm = false;
        pose.heavyBoots = state.value("heavy_boots", false);
        pose.zoraMaskDraw = state.value("zora_mask_draw", pose.clothesVariant == 2);
        pose.magicArmorPowered = state.value("magic_armor_powered", true);
        pose.itemDraw = state.value("item_draw", false);
        pose.kanteraDraw = state.value("kantera_draw", false);
        pose.itemActorKind = state.value("item_actor_kind", 0);
        pose.boomerangVisualValid = false;
        if (state.contains("boomerang_visual")) {
            const json& boomerang = state.at("boomerang_visual");
            if (!boomerang.is_object()) {
                error = "pose contains invalid boomerang visual state";
                return false;
            }
            pose.boomerangLinkAnchored = boomerang.value("link_anchored", false);
            if (!pose.boomerangLinkAnchored) {
                const json pos = boomerang.value("pos", json::array());
                const json angle = boomerang.value("angle", json::array());
                if (pos.size() != 3 || angle.size() != 3) {
                    error = "pose contains invalid boomerang visual state";
                    return false;
                }
                pose.boomerangX = pos.at(0).get<float>();
                pose.boomerangY = pos.at(1).get<float>();
                pose.boomerangZ = pos.at(2).get<float>();
                pose.boomerangAngleX = angle.at(0).get<int>();
                pose.boomerangAngleY = angle.at(1).get<int>();
                pose.boomerangAngleZ = angle.at(2).get<int>();
                if (!std::isfinite(pose.boomerangX) ||
                    !std::isfinite(pose.boomerangY) ||
                    !std::isfinite(pose.boomerangZ)) {
                    error = "pose contains invalid boomerang visual values";
                    return false;
                }
            }
            pose.boomerangVisualValid = true;
        }
        pose.itemActorBombExTime = state.value("item_actor_bomb_ex_time", -1);
        pose.itemActorBombFlash = state.value("item_actor_bomb_flash", -1);
        pose.ironBallVisualValid = state.value("iron_ball_visual_valid", false);
        pose.ironBallLinkAnchored = state.value("iron_ball_link_anchored", false);
        pose.ironBallX = state.value("iron_ball_x", 0.0f);
        pose.ironBallY = state.value("iron_ball_y", 0.0f);
        pose.ironBallZ = state.value("iron_ball_z", 0.0f);
        pose.ironBallAngleX = state.value("iron_ball_angle_x", 0);
        pose.ironBallAngleY = state.value("iron_ball_angle_y", 0);
        pose.ironBallAngleZ = state.value("iron_ball_angle_z", 0);
        if (pose.ironBallVisualValid && !pose.ironBallLinkAnchored &&
            (!std::isfinite(pose.ironBallX) || !std::isfinite(pose.ironBallY) ||
             !std::isfinite(pose.ironBallZ))) {
            error = "pose contains non-finite iron ball visual state";
            return false;
        }
        pose.ironBallChainCount = 0;
        pose.ironBallChainDirections.clear();
        if (pose.ironBallVisualValid && state.contains("iron_ball_chain")) {
            const json& chain = state.at("iron_ball_chain");
            const int count = chain.value("count", -1);
            std::string directions;
            if (count < 0 || count > 100 || !packed_bytes(chain, directions) ||
                directions.size() != static_cast<size_t>(count) * 3) {
                error = "pose contains invalid iron ball chain state";
                return false;
            }
            pose.ironBallChainCount = static_cast<uint8_t>(count);
            pose.ironBallChainDirections.assign(directions.begin(), directions.end());
            std::string endOffset;
            if (chain.contains("end")) {
                json endContainer = {{"data", chain.at("end")}};
                if (!packed_bytes(endContainer, endOffset) || endOffset.size() != 3) {
                    error = "pose contains invalid iron ball chain endpoint";
                    return false;
                }
                pose.ironBallChainEndOffsetX =
                    static_cast<float>(static_cast<int8_t>(endOffset[0])) * 0.25f;
                pose.ironBallChainEndOffsetY =
                    static_cast<float>(static_cast<int8_t>(endOffset[1])) * 0.25f;
                pose.ironBallChainEndOffsetZ =
                    static_cast<float>(static_cast<int8_t>(endOffset[2])) * 0.25f;
                pose.ironBallChainEndOffsetValid = true;
            }
        }
        pose.hookshotVisualValid = false;
        if (state.contains("hookshot_visual")) {
            const json& hookshot = state.at("hookshot_visual");
            if (!hookshot.is_object()) {
                error = "pose contains invalid hookshot visual state";
                return false;
            }
            const json top = hookshot.value("top", json::array());
            const json topAngle = hookshot.value("top_angle", json::array());
            const json subTop = hookshot.value("sub_top", json::array());
            const json subTopAngle = hookshot.value("sub_top_angle", json::array());
            if (top.size() != 3 || topAngle.size() != 3 || subTop.size() != 3 ||
                subTopAngle.size() != 3) {
                error = "pose contains invalid hookshot visual state";
                return false;
            }
            pose.hookshotTopX = top.at(0).get<float>();
            pose.hookshotTopY = top.at(1).get<float>();
            pose.hookshotTopZ = top.at(2).get<float>();
            pose.hookshotTopAngleX = topAngle.at(0).get<int>();
            pose.hookshotTopAngleY = topAngle.at(1).get<int>();
            pose.hookshotTopAngleZ = topAngle.at(2).get<int>();
            pose.hookshotSubTopX = subTop.at(0).get<float>();
            pose.hookshotSubTopY = subTop.at(1).get<float>();
            pose.hookshotSubTopZ = subTop.at(2).get<float>();
            pose.hookshotSubTopAngleX = subTopAngle.at(0).get<int>();
            pose.hookshotSubTopAngleY = subTopAngle.at(1).get<int>();
            pose.hookshotSubTopAngleZ = subTopAngle.at(2).get<int>();
            pose.hookshotLeft = hookshot.value("left", true);
            pose.hookshotArmAimX = hookshot.value("arm_aim_x", 0);
            pose.hookshotArmAimY = hookshot.value("arm_aim_y", 0);
            pose.hookshotTopLinkAnchored =
                hookshot.value("top_link_anchored", false);
            pose.hookshotSubTopLinkAnchored =
                hookshot.value("sub_top_link_anchored", false);
            pose.hookshotStopTime = hookshot.value("stop_time", 0);
            pose.hookshotItemFrame = hookshot.value("item_frame", 0.0f);
            pose.hookshotTipFrame = hookshot.value("tip_frame", 0.0f);
            pose.hookshotSubTipFrame = hookshot.value("sub_tip_frame", 0.0f);
            if (!std::isfinite(pose.hookshotTopX) ||
                !std::isfinite(pose.hookshotTopY) ||
                !std::isfinite(pose.hookshotTopZ) ||
                !std::isfinite(pose.hookshotSubTopX) ||
                !std::isfinite(pose.hookshotSubTopY) ||
                !std::isfinite(pose.hookshotSubTopZ) ||
                !std::isfinite(pose.hookshotItemFrame) ||
                !std::isfinite(pose.hookshotTipFrame) ||
                !std::isfinite(pose.hookshotSubTipFrame) ||
                pose.hookshotArmAimX < -0x8000 ||
                pose.hookshotArmAimX > 0x7FFF ||
                pose.hookshotArmAimY < -0x8000 ||
                pose.hookshotArmAimY > 0x7FFF ||
                pose.hookshotStopTime < 0 || pose.hookshotStopTime > 0x7FFF) {
                error = "pose contains invalid hookshot visual values";
                return false;
            }
            pose.hookshotVisualValid = true;
        }
        pose.copyRodVisualValid = false;
        if (state.contains("copy_rod_visual")) {
            const json& copyRod = state.at("copy_rod_visual");
            if (!copyRod.is_object()) {
                error = "pose contains invalid copy rod visual state";
                return false;
            }
            pose.copyRodTopUse = copyRod.value("top_use", false);
            pose.copyRodVisualValid = true;
        }
        pose.fishingRodVisualValid = false;
        pose.fishingRodLineValid = false;
        pose.fishingRodEndValid = false;
        if (state.contains("fishing_rod_visual")) {
            const json& fishingRod = state.at("fishing_rod_visual");
            if (!fishingRod.is_object() || !fishingRod.contains("segments")) {
                error = "pose contains invalid fishing rod visual state";
                return false;
            }
            json packedContainer = {{"data", fishingRod.at("segments")}};
            std::string packed;
            if (!packed_bytes(packedContainer, packed) || packed.size() != 90) {
                error = "pose contains invalid fishing rod segment state";
                return false;
            }
            const auto readI16 = [&packed](size_t offset) {
                const uint16_t bits = static_cast<uint8_t>(packed[offset]) |
                    (static_cast<uint16_t>(static_cast<uint8_t>(packed[offset + 1])) << 8);
                return static_cast<int16_t>(bits);
            };
            for (size_t i = 0; i < 15; ++i) {
                const size_t offset = i * 6;
                pose.fishingRodSegmentDeltas[i * 3] =
                    static_cast<float>(readI16(offset)) / 32.0f;
                pose.fishingRodSegmentDeltas[i * 3 + 1] =
                    static_cast<float>(readI16(offset + 2)) / 32.0f;
                pose.fishingRodSegmentDeltas[i * 3 + 2] =
                    static_cast<float>(readI16(offset + 4)) / 32.0f;
            }
            pose.fishingRodLineOffsets.fill(0.0f);
            pose.fishingRodLineValid = false;
            if (fishingRod.contains("line")) {
                json packedLineContainer = {{"data", fishingRod.at("line")}};
                std::string packedLine;
                constexpr size_t lineBytes =
                    (kFishingRodLineSampleCount - 1) * 3 * sizeof(int16_t);
                if (!packed_bytes(packedLineContainer, packedLine) ||
                    packedLine.size() != lineBytes) {
                    error = "pose contains invalid fishing rod line state";
                    return false;
                }
                const auto readLineI16 = [&packedLine](size_t offset) {
                    const uint16_t bits = static_cast<uint8_t>(packedLine[offset]) |
                        (static_cast<uint16_t>(static_cast<uint8_t>(
                             packedLine[offset + 1])) << 8);
                    return static_cast<int16_t>(bits);
                };
                for (size_t i = 1; i < kFishingRodLineSampleCount; ++i) {
                    const size_t offset = (i - 1) * 6;
                    pose.fishingRodLineOffsets[i * 3] =
                        static_cast<float>(readLineI16(offset)) / 4.0f;
                    pose.fishingRodLineOffsets[i * 3 + 1] =
                        static_cast<float>(readLineI16(offset + 2)) / 4.0f;
                    pose.fishingRodLineOffsets[i * 3 + 2] =
                        static_cast<float>(readLineI16(offset + 4)) / 4.0f;
                }
                pose.fishingRodLineValid = true;
            }
            if (fishingRod.contains("end")) {
                json packedEndContainer = {{"data", fishingRod.at("end")}};
                std::string packedEnd;
                if (!packed_bytes(packedEndContainer, packedEnd) ||
                    packedEnd.size() != 25) {
                    error = "pose contains invalid fishing rod end state";
                    return false;
                }
                const auto readEndI16 = [&packedEnd](size_t offset) {
                    const uint16_t bits = static_cast<uint8_t>(packedEnd[offset]) |
                        (static_cast<uint16_t>(static_cast<uint8_t>(
                             packedEnd[offset + 1])) << 8);
                    return static_cast<int16_t>(bits);
                };
                pose.fishingRodAction = static_cast<uint8_t>(packedEnd[0]);
                pose.fishingRodHookKind = static_cast<uint8_t>(packedEnd[1]);
                pose.fishingRodBaitKind = static_cast<uint8_t>(packedEnd[2]);
                pose.fishingRodEndRoll = readEndI16(3);
                for (size_t i = 0; i < 3; ++i) {
                    pose.fishingRodBobberOffset[i] =
                        static_cast<float>(readEndI16(5 + i * 2)) / 4.0f;
                }
                for (size_t i = 0; i < 6; ++i) {
                    pose.fishingRodBobberAngles[i] = readEndI16(11 + i * 2);
                }
                pose.fishingRodCounter = readEndI16(23);
                if (pose.fishingRodAction > 6 || pose.fishingRodHookKind > 1 ||
                    pose.fishingRodBaitKind > 2) {
                    error = "pose contains out-of-range fishing rod end state";
                    return false;
                }
                pose.fishingRodEndValid = true;
            }
            pose.fishingRodVisualValid = true;
        }
        pose.bowVisualValid = false;
        if (state.contains("bow_visual")) {
            const json& bow = state.at("bow_visual");
            if (!bow.is_object()) {
                error = "pose contains invalid bow visual state";
                return false;
            }
            pose.bowGrabLeft = bow.value("grab_left", false);
            pose.bowBck = bow.value("bck", 0);
            pose.bowFrame = bow.value("frame", 0.0f);
            pose.bowArrowVisible = bow.value("arrow_visible", false);
            pose.bowArrowBomb = bow.value("arrow_bomb", false);
            if (pose.bowBck <= 0 || pose.bowBck > 0xFFFF ||
                !std::isfinite(pose.bowFrame)) {
                error = "pose contains invalid bow visual values";
                return false;
            }
            pose.bowVisualValid = true;
        }
        pose.lanternVisualValid = false;
        if (state.contains("lantern_visual")) {
            const json& lantern = state.at("lantern_visual");
            if (!lantern.is_object()) {
                error = "pose contains invalid lantern visual state";
                return false;
            }
            pose.lanternLinkAnchored = lantern.value("link_anchored", false);
            pose.lanternHandAttached = lantern.value("hand_attached", false);
            pose.lanternLit = lantern.value("lit", false);
            const json jointAngle = lantern.value("joint_angle", json::array());
            if (jointAngle.size() != 3) {
                error = "pose contains invalid lantern joint state";
                return false;
            }
            pose.lanternJointAngleX = jointAngle.at(0).get<int>();
            pose.lanternJointAngleY = jointAngle.at(1).get<int>();
            pose.lanternJointAngleZ = jointAngle.at(2).get<int>();
            if (!pose.lanternLinkAnchored) {
                const json pos = lantern.value("pos", json::array());
                const json baseAngle = lantern.value("base_angle", json::array());
                if (pos.size() != 3 || baseAngle.size() != 3) {
                    error = "pose contains invalid detached lantern state";
                    return false;
                }
                pose.lanternX = pos.at(0).get<float>();
                pose.lanternY = pos.at(1).get<float>();
                pose.lanternZ = pos.at(2).get<float>();
                pose.lanternBaseAngleX = baseAngle.at(0).get<int>();
                pose.lanternBaseAngleY = baseAngle.at(1).get<int>();
                pose.lanternBaseAngleZ = baseAngle.at(2).get<int>();
                if (!std::isfinite(pose.lanternX) ||
                    !std::isfinite(pose.lanternY) ||
                    !std::isfinite(pose.lanternZ)) {
                    error = "pose contains invalid lantern visual values";
                    return false;
                }
            }
            pose.lanternVisualValid = true;
        }
        pose.bottleVisualValid = false;
        if (state.contains("bottle_visual")) {
            const json& bottle = state.at("bottle_visual");
            if (!bottle.is_object()) {
                error = "pose contains invalid bottle visual state";
                return false;
            }
            pose.bottleOilRightAttached = bottle.value("oil_right", false);
            pose.bottleJointRightAttached = bottle.value("joint_right", false);
            pose.bottleDrinkMaterialSet = bottle.value("drink_set", false);
            pose.bottleMaterialStage = bottle.value("material_stage", 0);
            pose.bottleBrkFrame = bottle.value("brk", 0.0f);
            pose.bottleBtpFrame = bottle.value("btp", 0.0f);
            pose.bottleBtkSwingFrame = bottle.value("btk_swing", 0.0f);
            pose.bottleBtkActionFrame = bottle.value("btk_action", 0.0f);
            pose.bottleBtkFinishFrame = bottle.value("btk_finish", 0.0f);
            pose.bottleContentKind = bottle.value("content", 0);
            pose.bottleContentFrame = bottle.value("content_frame", 0.0f);
            if (pose.bottleMaterialStage < 0 || pose.bottleMaterialStage > 2 ||
                pose.bottleContentKind < 0 || pose.bottleContentKind > 3 ||
                !std::isfinite(pose.bottleBrkFrame) ||
                !std::isfinite(pose.bottleBtpFrame) ||
                !std::isfinite(pose.bottleBtkSwingFrame) ||
                !std::isfinite(pose.bottleBtkActionFrame) ||
                !std::isfinite(pose.bottleBtkFinishFrame) ||
                !std::isfinite(pose.bottleContentFrame)) {
                error = "pose contains invalid bottle visual values";
                return false;
            }
            pose.bottleVisualValid = true;
        }
        pose.rideActorKind = state.value("ride_actor_kind", 0);
        pose.spinnerVisualValid = state.value("spinner_visual_valid", false);
        pose.spinnerLinkAnchored = state.value("spinner_link_anchored", false);
        pose.spinnerX = state.value("spinner_x", 0.0f);
        pose.spinnerY = state.value("spinner_y", 0.0f);
        pose.spinnerZ = state.value("spinner_z", 0.0f);
        pose.spinnerShapeX = state.value("spinner_shape_x", 0);
        pose.spinnerShapeY = state.value("spinner_shape_y", 0);
        pose.spinnerShapeZ = state.value("spinner_shape_z", 0);
        pose.spinnerRotY = state.value("spinner_rot_y", 0);
        pose.spinnerVisualYOffset = state.value("spinner_visual_y_offset", 90.0f);
        pose.spinnerJumpEpoch = state.value("spinner_jump_epoch", uint32_t{0});
        if (pose.spinnerVisualValid &&
            (!std::isfinite(pose.spinnerX) || !std::isfinite(pose.spinnerY) ||
             !std::isfinite(pose.spinnerZ) || !std::isfinite(pose.spinnerVisualYOffset))) {
            error = "pose contains non-finite spinner visual state";
            return false;
        }
        pose.linkMatrices = parse_matrices(state.value("link_matrices", json::object()));
        pose.linkMatricesFresh = pose.linkMatrices.valid;
        if (previous != nullptr && previous->valid) {
            if (pose.sequence <= previous->sequence) { error = "stale pose sequence"; return false; }
            if (pose.linkMatrices.valid) hydrate_matrices(pose.linkMatrices, previous->linkMatrices);
            else pose.linkMatrices = previous->linkMatrices;
        }
        pose.audioEvents = parse_audio(state, "audio_events", false);
        pose.activeAudioEvents = parse_audio(state, "active_audio_events", true);
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid pose: ") + ex.what();
        return false;
    }
}

bool enforce_semantic_pose_invariants(PeerPoseSnapshot& pose, std::string& error) {
    if (!pose.valid) {
        error = "semantic pose is invalid";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::Unknown) {
        error = "semantic pose has no supported visual mode";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::SemanticGameplay &&
        pose.visualUnsupportedReasons != 0) {
        error = "semantic pose has contradictory unsupported reasons";
        return false;
    }
    if (pose.visualMode == PeerPoseSnapshot::VisualMode::HiddenUnsupported) {
        pose.linkMatrices = {};
        pose.linkMatricesFresh = false;
        error.clear();
        return true;
    }
    if (pose.boomerangVisualValid &&
        pose.itemActorKind != dusk::multiplayer::REMOTE_ITEM_ACTOR_BOOMERANG) {
        error = "semantic boomerang state has no boomerang actor";
        return false;
    }
    if (pose.copyRodVisualValid && pose.equipItem != 0x46 &&
        pose.equipItem != 0x4C) {
        error = "semantic copy rod state has no equipped copy rod";
        return false;
    }
    if (pose.fishingRodVisualValid && pose.equipItem != 0x4A &&
        (pose.equipItem < 0x5B || pose.equipItem > 0x5F)) {
        error = "semantic fishing rod state has no equipped fishing rod";
        return false;
    }
    if (pose.bowVisualValid && pose.equipItem != 0x43 &&
        pose.equipItem != 0x59 && pose.equipItem != 0x5A &&
        pose.equipItem != 0x4B) {
        error = "semantic bow/slingshot state has no equipped projectile item";
        return false;
    }
    if (pose.bottleVisualValid && !is_bottle_item(pose.equipItem)) {
        error = "semantic bottle state has no equipped bottle";
        return false;
    }

    // Type-7 is a matrix-free representation. This clears freshly supplied
    // matrices and any snapshot hydrated from a previous packet, preventing
    // an older or malformed peer from silently restoring attachment fallback.
    pose.linkMatrices = {};
    pose.linkMatricesFresh = false;
    error.clear();
    return true;
}

bool merge_midna_pose(const json& message, PeerPoseSnapshot& pose, std::string& error) {
    (void)message;
    (void)pose;
    error.clear();
    return false;
}

bool expand_remote_pose_delta(json& message, const std::string& peerId,
                              uint8_t packetType, uint32_t sequence,
                              std::string& error) {
    try {
        const std::string key = history_key(peerId, packetType);
        if (packetType == 7 &&
            !expand_semantic_snapshot_delta(message, key, sequence, error)) {
            return false;
        }
        auto state = message.find("state");
        if (state != message.end() && state->is_object()) {
            auto matrices = state->find("link_matrices");
            if (matrices != state->end() && matrices->is_object()) {
                std::string format = matrices->value("format", "");
                if (const auto baseFormat = base_format_for_delta(format)) {
                    const uint32_t baselineSequence = matrices->value("baseline_sequence", 0U);
                    const auto history = sMatrixHistory.find(key);
                    if (history == sMatrixHistory.end()) {
                        error = "matrix delta has no baseline history";
                        return false;
                    }
                    const auto baseline = history->second.find(baselineSequence);
                    if (baseline == history->second.end()) {
                        error = "matrix delta baseline is unavailable";
                        return false;
                    }
                    std::string delta, expanded;
                    if (!packed_bytes(*matrices, delta) ||
                        !apply_slot_delta(format, delta, baseline->second,
                                          baselineSequence, expanded)) {
                        error = "matrix delta payload is invalid";
                        return false;
                    }
                    (*matrices)["format"] = *baseFormat;
                    matrices->erase("baseline_sequence");
                    std::vector<uint8_t> bytes(expanded.begin(), expanded.end());
                    (*matrices)["data"] = json::binary(std::move(bytes));
                    format = *baseFormat;
                }
                if (sequence != 0 && is_delta_capable_base(format)) {
                    std::string packed;
                    if (!packed_bytes(*matrices, packed)) {
                        error = "matrix baseline payload is invalid";
                        return false;
                    }
                    auto& history = sMatrixHistory[key];
                    history[sequence] = std::move(packed);
                    while (history.size() > kMatrixHistoryLimit) history.erase(history.begin());
                }
            }
        }
        if (packetType == 7) store_semantic_snapshot(key, sequence, message);
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid matrix delta: ") + ex.what();
        return false;
    }
}

bool prepare_remote_pose_delta(json& message, const std::string& peerId,
                               uint8_t packetType, uint32_t sequence,
                               uint32_t baselineSequence,
                               bool allowSemanticSnapshotDelta,
                               bool collectSnapshotDiagnostics,
                               std::string& error) {
    try {
        const std::string key = history_key(peerId, packetType);
        const json fullMessage = message;
        if (packetType == 7) store_semantic_snapshot(key, sequence, fullMessage);
        const bool independentFullSnapshot = packetType == 7 &&
            allowSemanticSnapshotDelta &&
            (baselineSequence == 0 || sequence == 0 || baselineSequence >= sequence ||
             sequence - baselineSequence >= kMatrixHistoryLimit ||
             (sequence % kSnapshotFullInterval) == 0);
        const uint32_t matrixBaselineSequence =
            independentFullSnapshot ? 0 : baselineSequence;

        auto state = message.find("state");
        if (state != message.end() && state->is_object()) {
            auto matrices = state->find("link_matrices");
            if (matrices != state->end() && matrices->is_object()) {
                const std::string format = matrices->value("format", "");
                const auto deltaFormat = delta_format_for_base(format);
                if (deltaFormat) {
                    std::string current;
                    if (!packed_bytes(*matrices, current)) {
                        error = "matrix baseline payload is invalid";
                        return false;
                    }
                    auto& history = sMatrixHistory[key];
                    if (sequence != 0) {
                        history[sequence] = current;
                        while (history.size() > kMatrixHistoryLimit) history.erase(history.begin());
                    }
                    const auto baseline = history.find(matrixBaselineSequence);
                    if (matrixBaselineSequence != 0 && baseline != history.end()) {
                        std::string delta;
                        if (build_slot_delta(format, current, baseline->second,
                                             matrixBaselineSequence, delta) &&
                            delta.size() < current.size()) {
                            (*matrices)["format"] = *deltaFormat;
                            (*matrices)["baseline_sequence"] = matrixBaselineSequence;
                            std::vector<uint8_t> bytes(delta.begin(), delta.end());
                            (*matrices)["data"] = json::binary(std::move(bytes));
                        }
                    }
                }
            }
        }

        if (packetType != 7) {
            error.clear();
            return true;
        }
        if (!allowSemanticSnapshotDelta) {
            message["_snapshot_debug_reason"] = "snapshot_capability_off";
            error.clear();
            return true;
        }
        if (baselineSequence == 0) {
            message["_snapshot_debug_reason"] = "no_common_ack_or_capability";
            error.clear();
            return true;
        }
        if (sequence == 0 || baselineSequence >= sequence ||
            sequence - baselineSequence >= kMatrixHistoryLimit) {
            message["_snapshot_debug_reason"] = "stale_baseline";
            error.clear();
            return true;
        }
        if ((sequence % kSnapshotFullInterval) == 0) {
            message["_snapshot_debug_reason"] = "periodic_full";
            error.clear();
            return true;
        }
        const auto history = sSemanticSnapshotHistory.find(key);
        if (history == sSemanticSnapshotHistory.end()) {
            message = fullMessage;
            message["_snapshot_debug_reason"] = "missing_history";
            error.clear();
            return true;
        }
        const auto baseline = history->second.find(baselineSequence);
        if (baseline == history->second.end()) {
            message = fullMessage;
            message["_snapshot_debug_reason"] = "missing_baseline";
            error.clear();
            return true;
        }
        const auto currentState = fullMessage.find("state");
        const auto baselineState = baseline->second.find("state");
        const auto preparedState = message.find("state");
        if (currentState == fullMessage.end() || !currentState->is_object() ||
            baselineState == baseline->second.end() || !baselineState->is_object() ||
            preparedState == message.end() || !preparedState->is_object()) {
            message = fullMessage;
            message["_snapshot_debug_reason"] = "invalid_state";
            error.clear();
            return true;
        }

        json patch = json::object();
        json changedKeys = json::array();
        json unchangedKeys = json::array();
        for (auto it = currentState->begin(); it != currentState->end(); ++it) {
            const auto old = baselineState->find(it.key());
            if (old == baselineState->end() || *old != it.value()) {
                const auto prepared = preparedState->find(it.key());
                patch[it.key()] = prepared == preparedState->end() ? it.value() : *prepared;
                if (collectSnapshotDiagnostics) changedKeys.push_back(it.key());
            } else if (collectSnapshotDiagnostics) {
                unchangedKeys.push_back(it.key());
            }
        }
        json removed = json::array();
        for (auto it = baselineState->begin(); it != baselineState->end(); ++it) {
            if (currentState->find(it.key()) == currentState->end()) removed.push_back(it.key());
        }

        json candidate = message;
        candidate["state"] = std::move(patch);
        candidate["snapshot_delta_v1"] = true;
        candidate["snapshot_base"] = baselineSequence;
        if (!removed.empty()) candidate["snapshot_removed"] = removed;
        if (json::to_msgpack(candidate).size() < json::to_msgpack(message).size()) {
            message = std::move(candidate);
            message["_snapshot_debug_reason"] = "delta";
            if (collectSnapshotDiagnostics) {
                message["_snapshot_debug_changed_keys"] = std::move(changedKeys);
                message["_snapshot_debug_unchanged_keys"] = std::move(unchangedKeys);
                message["_snapshot_debug_removed_keys"] = removed;
            }
        } else {
            message["_snapshot_debug_reason"] = "not_smaller";
        }
        error.clear();
        return true;
    } catch (const json::exception& ex) {
        error = std::string("invalid matrix baseline: ") + ex.what();
        return false;
    }
}

void clear_remote_pose_history(const std::string& peerId) {
    if (peerId.empty()) {
        sMatrixHistory.clear();
        sSemanticSnapshotHistory.clear();
        return;
    }
    for (auto it = sMatrixHistory.begin(); it != sMatrixHistory.end();) {
        if (it->first.rfind(peerId + '\x1f', 0) == 0) it = sMatrixHistory.erase(it);
        else ++it;
    }
    for (auto it = sSemanticSnapshotHistory.begin(); it != sSemanticSnapshotHistory.end();) {
        if (it->first.rfind(peerId + '\x1f', 0) == 0) {
            it = sSemanticSnapshotHistory.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace dusklight_online::game
