#pragma once
#include <nlohmann/json.hpp>
#include <zstd.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dusklight_online::net {
// Line framing stays separate from reliability and the datagram carrier.
// Both encoded input and expanded JSON are bounded by the existing relay limit.
class ReliableJsonError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
inline constexpr size_t reliableJsonLimit = 512 * 1024;
inline constexpr size_t reliablePeerFrameLimit = reliableJsonLimit + 2048;
inline constexpr size_t reliableJsonMaxDepth = 64;
inline void check_reliable_json_depth(std::string_view raw) {
    size_t depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (char c : raw) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > reliableJsonMaxDepth)
                throw ReliableJsonError("reliable JSON nesting too deep");
        } else if (c == '}' || c == ']') {
            if (depth == 0) throw ReliableJsonError("invalid reliable JSON nesting");
            --depth;
        }
    }
}
inline std::string encode_reliable_json(const nlohmann::json& message, size_t limit = reliableJsonLimit) {
    auto raw = message.dump();
    if (raw.size() > limit) return raw; // existing send limits handle oversized messages
    if (raw.size() < 1024) return raw;
    std::string compressed(ZSTD_compressBound(raw.size()), '\0');
    size_t count = ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 1);
    if (ZSTD_isError(count) || 2 + count * 2 >= raw.size()) return raw;
    static constexpr char hex[] = "0123456789abcdef";
    std::string wire = "Z1";
    wire.reserve(2 + count * 2);
    for (size_t i = 0; i < count; ++i) {
        auto byte = static_cast<unsigned char>(compressed[i]);
        wire.push_back(hex[byte >> 4]); wire.push_back(hex[byte & 15]);
    }
    return wire;
}
inline nlohmann::json decode_reliable_json(std::string_view wire, size_t limit = reliableJsonLimit) {
    if (wire.size() > limit) throw ReliableJsonError("reliable JSON too large");
    if (!wire.starts_with("Z1")) {
        check_reliable_json_depth(wire);
        return nlohmann::json::parse(wire);
    }
    wire.remove_prefix(2);
    if (wire.empty() || wire.size() % 2) throw ReliableJsonError("invalid compressed JSON");
    auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        throw ReliableJsonError("invalid compressed JSON encoding");
    };
    std::string compressed(wire.size() / 2, '\0');
    for (size_t i = 0; i < compressed.size(); ++i)
        compressed[i] = static_cast<char>((digit(wire[2*i]) << 4) | digit(wire[2*i+1]));
    auto expanded = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    if (!expanded || expanded > limit ||
        ZSTD_findFrameCompressedSize(compressed.data(), compressed.size()) != compressed.size())
        throw ReliableJsonError("invalid compressed JSON size");
    std::string raw(static_cast<size_t>(expanded), '\0');
    auto count = ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(count) || count != raw.size()) throw ReliableJsonError("invalid compressed JSON frame");
    check_reliable_json_depth(raw);
    return nlohmann::json::parse(raw);
}
}

