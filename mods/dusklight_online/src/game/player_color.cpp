#include "dusklight_online/game/player_color.hpp"
#include <algorithm>
#include <cstdio>

namespace dusklight_online::game::appearance {
std::optional<Color> parse_color(std::string_view s) {
    if (s.empty()) return default_color;
    if (s.size() != 6) return {};
    Color result = 0;
    for (char c : s) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return {};
        result = (result << 4) | digit;
    }
    return result;
}
std::string color_string(Color color) {
    if (color == default_color) return {};
    char text[7];
    std::snprintf(text, sizeof(text), "%06X", color & 0xffffff);
    return text;
}
namespace {
using Pixel = std::array<uint8_t, 4>;
uint16_t word(const uint8_t* p) { return uint16_t(p[0]) * 256 + p[1]; }
Pixel rgb565(uint16_t v) {
    return {uint8_t(((v >> 11) & 31) * 255 / 31),
            uint8_t(((v >> 5) & 63) * 255 / 63), uint8_t((v & 31) * 255 / 31), 255};
}
Pixel rgb5a3(uint16_t v) {
    if (v & 0x8000) return {uint8_t(((v >> 10) & 31) * 255 / 31),
        uint8_t(((v >> 5) & 31) * 255 / 31), uint8_t((v & 31) * 255 / 31), 255};
    return {uint8_t(((v >> 8) & 15) * 17), uint8_t(((v >> 4) & 15) * 17),
            uint8_t((v & 15) * 17), uint8_t(((v >> 12) & 7) * 255 / 7)};
}
Pixel tint(Pixel p, Color color) {
    // Rec.709 luminance, then a piecewise blend retaining black shadows and
    // white highlights. This operates on decoded pixels, not CMPR endpoints.
    const unsigned light = (2126u*p[0] + 7152u*p[1] + 722u*p[2] + 5000) / 10000;
    for (unsigned c = 0; c < 3; ++c) {
        const unsigned chosen = (color >> ((2-c)*8)) & 255;
        p[c] = uint8_t(light < 128 ? (2*light*chosen + 127)/255 :
                       255 - (2*(255-light)*(255-chosen) + 127)/255);
    }
    return p;
}
}
size_t texture_size(unsigned format, unsigned w, unsigned h, unsigned mips) {
    if (!w || !h || w > 2048 || h > 2048 || !mips || mips > 12) return 0;
    if (format != 4 && format != 5 && format != 6 && format != 14) return 0;
    size_t bytes = 0;
    for (unsigned m = 0; m < mips; ++m) {
        const unsigned tile = format == 14 ? 8 : 4;
        bytes += size_t((w+tile-1)/tile) * ((h+tile-1)/tile) * (format == 6 ? 64 : 32);
        w = std::max(1u,w/2); h = std::max(1u,h/2);
    }
    return bytes;
}
std::vector<uint8_t> recolor(std::span<const uint8_t> source, unsigned format,
                             unsigned w, unsigned h, unsigned mips, Color color) {
    const auto required = texture_size(format,w,h,mips);
    if (!required || source.size() < required) return {};
    std::vector<uint8_t> out(texture_size(6,w,h,mips), 0);
    size_t inputBase = 0, outputBase = 0;
    for (unsigned mip=0; mip<mips; ++mip) {
        const unsigned tilesX = (w+3)/4;
        for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x) {
            Pixel p{};
            if (format == 14) {
                const size_t block = (size_t(y/8)*((w+7)/8)+x/8)*32 +
                                     ((y%8)/4*2+(x%8)/4)*8;
                const uint8_t* b = source.data()+inputBase+block;
                const uint16_t a=word(b), z=word(b+2);
                Pixel palette[4] = {rgb565(a),rgb565(z),{}, {}};
                for (int c=0;c<3;++c) {
                    palette[2][c] = uint8_t(a>z ? (2*palette[0][c]+palette[1][c])/3 :
                                                   (palette[0][c]+palette[1][c])/2);
                    palette[3][c] = uint8_t(a>z ? (palette[0][c]+2*palette[1][c])/3 : 0);
                }
                palette[2][3]=255; palette[3][3]=a>z?255:0;
                p=palette[(b[4+y%4] >> (6-2*(x%4))) & 3];
            } else {
                const size_t tile = size_t(y/4)*tilesX+x/4;
                const unsigned index = (y%4)*4+x%4;
                const uint8_t* b=source.data()+inputBase+tile*(format==6?64:32);
                if (format==6) p={b[2*index+1],b[32+2*index],b[33+2*index],b[2*index]};
                else p=format==4 ? rgb565(word(b+2*index)) : rgb5a3(word(b+2*index));
            }
            p=tint(p,color);
            const size_t dest=outputBase+(size_t(y/4)*tilesX+x/4)*64+((y%4)*4+x%4)*2;
            out[dest]=p[3]; out[dest+1]=p[0]; out[dest+32]=p[1]; out[dest+33]=p[2];
        }
        inputBase += texture_size(format,w,h,1);
        outputBase += texture_size(6,w,h,1);
        w=std::max(1u,w/2); h=std::max(1u,h/2);
    }
    return out;
}
}
