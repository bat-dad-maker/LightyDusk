#include "dusklight_online/game/player_color.hpp"
#include <cstdlib>
#include <iostream>
using namespace dusklight_online::game::appearance;
void check(bool condition, const char* label) {
    if (!condition) { std::cerr << label << '\n'; std::exit(1); }
}
int main() {
    check(parse_color("") == default_color, "empty is original, not RGB white");
    check(parse_color("FFFFFF") == 0xffffff && parse_color("FFFFFF") != default_color,
          "white is an explicit recolour");
    check(parse_color("aB12f0") == 0xab12f0, "hex case");
    for (auto invalid : {"#123456", "12345", "1234567", "GG0000", "rainbow", "      "})
        check(!parse_color(invalid), "reject malformed colours");
    check(color_string(default_color).empty(), "original round trip");
    check(color_string(0x000a12) == "000A12", "RGB round trip");
    check(texture_size(14,8,8,1)==32 && texture_size(6,8,8,1)==256,"tile sizes");
    check(texture_size(6,5,3,3)==256,"partial tiles and mip tail");
    check(!texture_size(14,4096,8,1) && !texture_size(8,8,8,1),"bounds and formats");
    std::vector<uint8_t> compressed(32,0);
    // Transparent CMPR mode: palette entry 3 must stay transparent.
    for (int b=0;b<4;++b) for(int row=4;row<8;++row) compressed[b*8+row]=255;
    auto rgba=recolor(compressed,14,8,8,1,0xff0000);
    check(rgba.size()==256,"CMPR output size");
    for (int tile=0;tile<4;++tile) for(int p=0;p<16;++p)
        check(rgba[tile*64+p*2]==0,"CMPR alpha survives recolouring");
    // Opaque mode endpoint white/black and all four selectors.
    compressed.assign(32,0);
    for (int b=0;b<4;++b) {
        compressed[b*8]=255; compressed[b*8+1]=255;
        for(int row=4;row<8;++row) compressed[b*8+row]=0x1b;
    }
    rgba=recolor(compressed,14,8,8,1,0xff0000);
    check(rgba[0]==255 && rgba[1]==255 && rgba[32]==255,"white highlights retained");
    check(rgba[3]==0 && rgba[34]==0,"black shadows retained");
    check(rgba[5]>rgba[36] && rgba[7]>rgba[38],"intermediate tones recoloured red");
    check(recolor(std::span(compressed).first(31),14,8,8,1,0).empty(),"truncated CMPR rejected");
    std::vector<uint8_t> a3(32,0);
    for(int p=0;p<16;++p) {a3[p*2]=0x38; a3[p*2+1]=0x88;}
    rgba=recolor(a3,5,4,4,1,0x00ff00);
    check(rgba[0]==109 && rgba[32]>rgba[1],"RGB5A3 alpha and green tint");
    std::vector<uint8_t> native(texture_size(6,5,3,3),0x80);
    rgba=recolor(native,6,5,3,3,0x0000ff);
    check(rgba.size()==native.size() && rgba[0]==128 && rgba[33]>rgba[1],"RGBA8 mip chain");
    std::cout << "Player colour decoder/default tests passed\n";
}
