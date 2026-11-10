#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dusklight_online::game::appearance {
using Color = uint32_t; // RRGGBB, always opaque.
// Separate from every RGB value, including white. No recolouring requested.
constexpr Color default_color = 0x1000000;
std::optional<Color> parse_color(std::string_view value);
std::string color_string(Color color);
// Decode native GX colour textures and recolour into tiled GX RGBA8.
// Empty output means an unsupported format or invalid/truncated input.
std::vector<uint8_t> recolor(std::span<const uint8_t> source, unsigned format,
                             unsigned width, unsigned height, unsigned mips, Color color);
size_t texture_size(unsigned format, unsigned width, unsigned height, unsigned mips);
}
