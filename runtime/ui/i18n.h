// The menus' text. This build ships English menus only, so tr() returns the English string as written; the call
// sites are kept so a translation table could be added back later. Display text only: labels used as ImGui IDs stay
// the English ones.
#pragma once
#include <string>

struct ImFontAtlas;

const char *tr(const char *en);
// In capitals (section titles, row labels): ASCII, the accented Latin-1 letters and Cyrillic in UTF-8 (á -> Á, я -> Я)
std::string ui_upper(const char *s);
// After a menu font is added to atlas: the letters it lacks for the menu language (Cyrillic, Japanese, Korean, Chinese)
// merged into it from the phone's own fonts, at about the same height (size_px: the font's own)
void i18n_merge_fonts(ImFontAtlas *atlas, float size_px);
