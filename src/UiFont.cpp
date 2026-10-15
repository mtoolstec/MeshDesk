#include <lvgl.h>

LV_FONT_DECLARE(lv_font_zh_cn_14);
LV_FONT_DECLARE(lv_font_emoji_14);

namespace {
bool uiGlyph(const lv_font_t *font, lv_font_glyph_dsc_t *glyph, uint32_t letter, uint32_t next) {
    // Presentation selectors and joiners are invisible Unicode controls.
    // This bitmap renderer shows component glyphs rather than shaping ZWJ sequences.
    if (letter == 0x200D || letter == 0xFE0E || letter == 0xFE0F) {
        *glyph = {};
        glyph->format = LV_FONT_GLYPH_FORMAT_A2;
        return true;
    }
    return lv_font_get_glyph_dsc_fmt_txt(font, glyph, letter, next);
}
}

// Keep the original Chinese font metrics and bitmap tables intact. Emoji and
// additional Latin punctuation are resolved only when its glyph is missing.
extern const lv_font_t lv_font_ui_14 = [] {
    lv_font_t font = lv_font_zh_cn_14;
    font.get_glyph_dsc = uiGlyph;
    font.fallback = &lv_font_emoji_14;
    return font;
}();
