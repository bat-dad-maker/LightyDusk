#pragma once

#include <imgui.h>

#include <cstddef>

namespace dusklight_online::game {

inline constexpr size_t kChatEmojiAtlasColumns = 16;
inline constexpr float kChatEmojiPixels = 32.0f;

// Lazily uploads the bundled Noto artwork to the host's ImGui renderer.
ImTextureID chat_emoji_atlas_texture();
ImVec2 chat_emoji_uv_min(size_t index);
ImVec2 chat_emoji_uv_max(size_t index);

}  // namespace dusklight_online::game
