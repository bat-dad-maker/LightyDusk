#include "dusklight_online/game/chat_emoji_atlas.hpp"

#include "dusklight_online/game/chat_emoji.hpp"
#include "dusklight_online/logging.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <aurora/imgui.h>
#include <mods/service.hpp>
#include <mods/svc/resource.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

namespace dusklight_online::game {
namespace {

constexpr size_t kRows = (kChatEmojis.size() + kChatEmojiAtlasColumns - 1) /
                         kChatEmojiAtlasColumns;
constexpr size_t kWidth = kChatEmojiAtlasColumns * 32;
constexpr size_t kHeight = kRows * 32;
bool sAttempted = false;
ImTextureID sTexture = 0;

}  // namespace

ImTextureID chat_emoji_atlas_texture() {
    if (sAttempted) return sTexture;
    if (svc_resource == nullptr || svc_resource->load == nullptr ||
        svc_resource->free == nullptr) return 0;
    sAttempted = true;

    std::vector<uint8_t> atlas(kWidth * kHeight * 4, 0);
    for (size_t index = 0; index < kChatEmojis.size(); ++index) {
        ResourceBuffer resource = RESOURCE_BUFFER_INIT;
        const std::string path = "emoji/" + std::string(kChatEmojis[index].filename);
        if (svc_resource->load(mod_ctx, path.c_str(), &resource) != MOD_OK ||
            resource.data == nullptr || resource.size == 0) {
            dusklight_online::log_info("CHAT_EMOJI missing bundled artwork: " + path);
            if (resource.data != nullptr) svc_resource->free(mod_ctx, &resource);
            return 0;
        }
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(
            static_cast<const stbi_uc*>(resource.data), static_cast<int>(resource.size),
            &width, &height, &channels, 4);
        svc_resource->free(mod_ctx, &resource);
        if (pixels == nullptr || width != 32 || height != 32) {
            dusklight_online::log_info("CHAT_EMOJI could not decode artwork: " + path);
            stbi_image_free(pixels);
            return 0;
        }
        const size_t x = (index % kChatEmojiAtlasColumns) * 32;
        const size_t y = (index / kChatEmojiAtlasColumns) * 32;
        for (size_t row = 0; row < 32; ++row) {
            std::memcpy(atlas.data() + ((y + row) * kWidth + x) * 4,
                        pixels + row * 32 * 4, 32 * 4);
        }
        stbi_image_free(pixels);
    }
    sTexture = aurora_imgui_add_texture(static_cast<uint32_t>(kWidth),
                                        static_cast<uint32_t>(kHeight), atlas.data());
    if (sTexture == 0)
        dusklight_online::log_info("CHAT_EMOJI could not upload artwork atlas");
    return sTexture;
}

ImVec2 chat_emoji_uv_min(size_t index) {
    return ImVec2(static_cast<float>(index % kChatEmojiAtlasColumns) /
                      static_cast<float>(kChatEmojiAtlasColumns),
                  static_cast<float>(index / kChatEmojiAtlasColumns) /
                      static_cast<float>(kRows));
}

ImVec2 chat_emoji_uv_max(size_t index) {
    const ImVec2 min = chat_emoji_uv_min(index);
    return ImVec2(min.x + 1.0f / static_cast<float>(kChatEmojiAtlasColumns),
                  min.y + 1.0f / static_cast<float>(kRows));
}

}  // namespace dusklight_online::game
