#include "dusklight_online/game/chat_emoji.hpp"

#include <algorithm>

namespace dusklight_online::game {
namespace {

size_t next_codepoint(std::string_view text, size_t offset) {
    if (offset >= text.size()) return text.size();
    ++offset;
    while (offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) ++offset;
    return offset;
}

}  // namespace

std::optional<ChatEmojiMatch> match_chat_emoji(std::string_view text, size_t offset) {
    if (offset >= text.size()) return std::nullopt;
    if (text[offset] == ':') {
        const size_t end = text.find(':', offset + 1);
        if (end != std::string_view::npos && end - offset <= 32) {
            const std::string_view alias = text.substr(offset + 1, end - offset - 1);
            for (const ChatEmojiAlias& entry : kChatEmojiAliases) {
                if (entry.alias == alias)
                    return ChatEmojiMatch{entry.emojiIndex, end + 1 - offset};
            }
        }
    }
    if (static_cast<unsigned char>(text[offset]) < 0x80) return std::nullopt;
    const std::string_view remaining = text.substr(offset);
    for (size_t index = 0; index < kChatEmojis.size(); ++index) {
        const ChatEmoji& emoji = kChatEmojis[index];
        if (remaining.starts_with(emoji.unicode))
            return ChatEmojiMatch{index, emoji.unicode.size()};
        if (emoji.bareUnicode != emoji.unicode &&
            remaining.starts_with(emoji.bareUnicode))
            return ChatEmojiMatch{index, emoji.bareUnicode.size()};
    }
    return std::nullopt;
}

ChatEmojiReplacement replace_chat_emoji_unicode(std::string_view text,
                                                 size_t cursorByte) {
    ChatEmojiReplacement result;
    result.text.reserve(text.size());
    cursorByte = std::min(cursorByte, text.size());
    bool cursorPlaced = false;
    for (size_t offset = 0; offset < text.size();) {
        if (!cursorPlaced && cursorByte <= offset) {
            result.cursorByte = result.text.size();
            cursorPlaced = true;
        }
        const auto match = match_chat_emoji(text, offset);
        if (match && text[offset] != ':') {
            const ChatEmoji& emoji = kChatEmojis[match->index];
            const size_t end = offset + match->bytes;
            result.text.push_back(':');
            result.text.append(emoji.canonical);
            result.text.push_back(':');
            if (!cursorPlaced && cursorByte < end) {
                result.cursorByte = result.text.size();
                cursorPlaced = true;
            }
            result.changed = true;
            offset = end;
        } else {
            const size_t end = next_codepoint(text, offset);
            result.text.append(text.substr(offset, end - offset));
            if (!cursorPlaced && cursorByte < end) {
                result.cursorByte = result.text.size();
                cursorPlaced = true;
            }
            offset = end;
        }
    }
    if (!cursorPlaced) result.cursorByte = result.text.size();
    return result;
}

}  // namespace dusklight_online::game
