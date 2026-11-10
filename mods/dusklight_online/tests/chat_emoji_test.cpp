#include "dusklight_online/game/chat_emoji.hpp"

#include <cassert>
#include <string>

using namespace dusklight_online::game;

int main() {
    const auto alias = match_chat_emoji("say :fire: now", 4);
    assert(alias && alias->bytes == 6);
    assert(kChatEmojis[alias->index].canonical == "fire");
    const auto pasted = match_chat_emoji("\xF0\x9F\x94\xA5", 0);
    assert(pasted && pasted->index == alias->index);
    assert(!match_chat_emoji(":not_an_emoji:", 0));

    const std::string message = "go \xF0\x9F\x94\xA5 now";
    const auto converted = replace_chat_emoji_unicode(message, message.size());
    assert(converted.changed);
    assert(converted.text == "go :fire: now");
    assert(converted.cursorByte == converted.text.size());
    const auto cursorInside = replace_chat_emoji_unicode(message, 5);
    assert(cursorInside.cursorByte == 9);
    const auto unchanged = replace_chat_emoji_unicode("go :fire: now", 5);
    assert(!unchanged.changed);
    assert(unchanged.text == "go :fire: now");
}
