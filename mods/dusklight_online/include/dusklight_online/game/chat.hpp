#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::game {

inline constexpr size_t kMaxChatLineCodepoints = 50;
inline constexpr size_t kMaxChatLines = 4;
// Four full lines of four-byte UTF-8 code points plus their separators.
inline constexpr size_t kMaxChatTextBytes =
    kMaxChatLineCodepoints * kMaxChatLines * 4 + (kMaxChatLines - 1);

struct ChatAutoBreak {
    size_t offset = 0;
    std::string separator;
};

struct WrappedChatInput {
    std::string text;
    size_t cursorByte = 0;
    std::vector<ChatAutoBreak> autoBreaks;
};

using ChatMeasureWidth = float (*)(std::string_view);

inline size_t chat_utf8_codepoint_bytes(std::string_view input, size_t index) {
    const uint8_t first = static_cast<uint8_t>(input[index]);
    if (first < 0x80) return 1;
    if (first >= 0xc2 && first <= 0xdf) return 2;
    if (first >= 0xe0 && first <= 0xef) return 3;
    if (first >= 0xf0 && first <= 0xf4) return 4;
    return 1;
}

inline WrappedChatInput wrap_chat_input(std::string_view input, size_t cursorByte,
                                       float maxLineWidth = 0.0f,
                                       ChatMeasureWidth measureWidth = nullptr) {
    WrappedChatInput wrapped;
    wrapped.text.reserve(std::min(input.size() + kMaxChatLines - 1,
                                  kMaxChatTextBytes));
    cursorByte = std::min(cursorByte, input.size());

    size_t inputIndex = 0;
    size_t line = 0;
    size_t column = 0;
    size_t lineStart = 0;
    const auto exceeds_width = [&](std::string_view addition) {
        if (measureWidth == nullptr || maxLineWidth <= 0.0f) return false;
        std::string candidate = wrapped.text.substr(lineStart);
        candidate.append(addition);
        return measureWidth(candidate) > maxLineWidth;
    };
    while (inputIndex < input.size() && line < kMaxChatLines) {
        if (inputIndex == cursorByte) wrapped.cursorByte = wrapped.text.size();

        const char character = input[inputIndex];
        if (character == '\r' || character == '\n') {
            const size_t consumed = character == '\r' && inputIndex + 1 < input.size() &&
                    input[inputIndex + 1] == '\n' ? 2 : 1;
            inputIndex += consumed;
            if (line + 1 >= kMaxChatLines) break;
            wrapped.text.push_back('\n');
            ++line;
            column = 0;
            lineStart = wrapped.text.size();
            if (inputIndex <= cursorByte) wrapped.cursorByte = wrapped.text.size();
            continue;
        }

        if (character == ' ') {
            size_t spaceEnd = inputIndex;
            while (spaceEnd < input.size() && input[spaceEnd] == ' ') ++spaceEnd;
            size_t wordEnd = spaceEnd;
            size_t wordCodepoints = 0;
            while (wordEnd < input.size() && input[wordEnd] != ' ' &&
                   input[wordEnd] != '\r' && input[wordEnd] != '\n') {
                wordEnd += std::min(chat_utf8_codepoint_bytes(input, wordEnd),
                                    input.size() - wordEnd);
                ++wordCodepoints;
            }
            const size_t spaces = spaceEnd - inputIndex;
            if (wordCodepoints == 0 && spaceEnd < input.size()) {
                // Do not manufacture an empty wrapped line immediately
                // before a line break explicitly entered by the player.
                inputIndex = spaceEnd;
                if (cursorByte > inputIndex - spaces && cursorByte <= inputIndex) {
                    wrapped.cursorByte = wrapped.text.size();
                }
                continue;
            }
            // A separator belongs to the word following it. If that word
            // would cross the line boundary, replace the separator with one
            // newline rather than starting the next line with spaces or
            // splitting a word that fits on a fresh line.
            if (column > 0 &&
                (column + spaces + wordCodepoints > kMaxChatLineCodepoints ||
                 exceeds_width(input.substr(inputIndex, wordEnd - inputIndex)))) {
                if (line + 1 >= kMaxChatLines) break;
                wrapped.text.push_back('\n');
                wrapped.autoBreaks.push_back({wrapped.text.size() - 1,
                                              std::string(input.substr(inputIndex, spaces))});
                ++line;
                column = 0;
                lineStart = wrapped.text.size();
                inputIndex = spaceEnd;
                if (cursorByte > inputIndex - spaces && cursorByte <= inputIndex) {
                    wrapped.cursorByte = wrapped.text.size();
                }
                continue;
            }
        }

        const size_t codepointBytes = std::min(
            chat_utf8_codepoint_bytes(input, inputIndex), input.size() - inputIndex);
        if (column == kMaxChatLineCodepoints ||
            (column > 0 && exceeds_width(input.substr(inputIndex, codepointBytes)))) {
            if (line + 1 >= kMaxChatLines) break;
            wrapped.text.push_back('\n');
            wrapped.autoBreaks.push_back({wrapped.text.size() - 1, {}});
            ++line;
            column = 0;
            lineStart = wrapped.text.size();
            if (inputIndex == cursorByte) wrapped.cursorByte = wrapped.text.size();
        }
        if (wrapped.text.size() + codepointBytes > kMaxChatTextBytes) break;
        wrapped.text.append(input.substr(inputIndex, codepointBytes));
        inputIndex += codepointBytes;
        ++column;
        if (inputIndex <= cursorByte) wrapped.cursorByte = wrapped.text.size();
    }
    if (cursorByte >= inputIndex) wrapped.cursorByte = wrapped.text.size();
    return wrapped;
}

// ImGui stores the displayed hard wraps in its editing buffer. Reconstruct
// their original separators before every edit so deleting earlier text can
// reclaim space, while genuine player-entered newlines remain untouched.
inline WrappedChatInput reflow_chat_input(
    std::string_view edited, size_t cursorByte, std::string_view previous,
    const std::vector<ChatAutoBreak>& previousBreaks,
    float maxLineWidth = 0.0f, ChatMeasureWidth measureWidth = nullptr) {
    if (previousBreaks.empty()) {
        return wrap_chat_input(edited, cursorByte, maxLineWidth, measureWidth);
    }

    size_t prefix = 0;
    while (prefix < previous.size() && prefix < edited.size() &&
           previous[prefix] == edited[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < previous.size() - prefix && suffix < edited.size() - prefix &&
           previous[previous.size() - suffix - 1] == edited[edited.size() - suffix - 1]) {
        ++suffix;
    }

    std::vector<ChatAutoBreak> surviving;
    for (const ChatAutoBreak& lineBreak : previousBreaks) {
        size_t offset = 0;
        if (lineBreak.offset < prefix) {
            offset = lineBreak.offset;
        } else if (lineBreak.offset >= previous.size() - suffix) {
            offset = edited.size() - suffix +
                     (lineBreak.offset - (previous.size() - suffix));
        } else {
            continue;
        }
        if (offset < edited.size() && edited[offset] == '\n') {
            surviving.push_back({offset, lineBreak.separator});
        }
    }

    std::string unwrapped;
    unwrapped.reserve(edited.size());
    size_t unwrappedCursor = 0;
    size_t breakIndex = 0;
    cursorByte = std::min(cursorByte, edited.size());
    for (size_t index = 0; index < edited.size(); ++index) {
        if (index == cursorByte) unwrappedCursor = unwrapped.size();
        if (breakIndex < surviving.size() && surviving[breakIndex].offset == index) {
            unwrapped += surviving[breakIndex++].separator;
        } else {
            unwrapped.push_back(edited[index]);
        }
    }
    if (cursorByte == edited.size()) unwrappedCursor = unwrapped.size();
    return wrap_chat_input(unwrapped, unwrappedCursor, maxLineWidth, measureWidth);
}

inline bool normalize_chat_text(std::string_view input, std::string& output) {
    while (!input.empty() && (input.front() == ' ' || input.front() == '\n')) {
        input.remove_prefix(1);
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\n')) {
        input.remove_suffix(1);
    }
    if (input.empty() || input.size() > kMaxChatTextBytes) return false;

    const auto* bytes = reinterpret_cast<const uint8_t*>(input.data());
    size_t index = 0;
    size_t line = 0;
    size_t column = 0;
    while (index < input.size()) {
        const uint8_t first = bytes[index];
        if (first < 0x80) {
            if (first == '\n') {
                if (++line >= kMaxChatLines) return false;
                column = 0;
                ++index;
                continue;
            }
            // Only LF is layout-significant. Reject the remaining C0 controls
            // and DEL rather than allowing them to affect logs or terminals.
            if (first < 0x20 || first == 0x7f ||
                ++column > kMaxChatLineCodepoints) return false;
            ++index;
            continue;
        }

        size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > input.size()) return false;
        for (size_t continuation = 1; continuation < length; ++continuation) {
            if ((bytes[index + continuation] & 0xc0) != 0x80) return false;
        }
        // Reject overlong encodings, UTF-16 surrogates, and values above
        // U+10FFFF.
        if (first == 0xe0 && bytes[index + 1] < 0xa0) return false;
        if (first == 0xed && bytes[index + 1] >= 0xa0) return false;
        if (first == 0xf0 && bytes[index + 1] < 0x90) return false;
        if (first == 0xf4 && bytes[index + 1] >= 0x90) return false;
        if (++column > kMaxChatLineCodepoints) return false;
        index += length;
    }

    output.assign(input);
    return true;
}

}  // namespace dusklight_online::game
