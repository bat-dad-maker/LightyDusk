// See i18n.h.
#include "i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unordered_map>

#include "imgui.h"

extern "C" {
#include "../settings.h"
}

// Menus are English only: tr() returns the string as written.
const char *tr(const char *en) { return en; }

std::string ui_upper(const char *s) {
    std::string t = s ? s : "";
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x80) t[i] = (char)toupper(c);
#ifdef __ANDROID__  // (the desktop's menus are English: bytes as before)
        else if (c == 0xC3 && i + 1 < t.size()) {  // U+00E0..U+00FE: the lowercase accented letters (but ÷, U+00F7)
            unsigned char d = (unsigned char)t[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) t[i + 1] = (char)(d - 0x20);
            i++;
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < t.size()) {  // Cyrillic: а..п (D0 B0-BF), р..я (D1 80-8F), ё
            unsigned char d = (unsigned char)t[i + 1];
            if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) t[i + 1] = (char)(d - 0x20);
            else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) t[i] = (char)0xD0, t[i + 1] = (char)(d + 0x20);
            else if (c == 0xD1 && d == 0x91) t[i] = (char)0xD0, t[i + 1] = (char)0x81;
            i++;
        } else if (c >= 0xC0) {  // (other letters as they are: skip the rest of the sequence)
            while (i + 1 < t.size() && ((unsigned char)t[i + 1] & 0xC0) == 0x80) i++;
        }
#endif
    }
    return t;
}

void i18n_merge_fonts(ImFontAtlas *atlas, float size_px) {
    // English-only menus: the shipped Latin font (Baloo 2) has every letter they use, so no system font is merged
    (void)atlas, (void)size_px;
}
