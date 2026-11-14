/* Original hand-drawn 16x16 sample glyphs; not extracted from a system font.
 * Each row is two bytes, MSB left. Extend via the public font table API. */
#define GLYPH_ROW(value) (uint8_t)((value) >> 8), (uint8_t)((value) & 255)
static const uint8_t chinese_zhong[] = {
    GLYPH_ROW(0x0100), GLYPH_ROW(0x0100), GLYPH_ROW(0x0100), GLYPH_ROW(0x3ff8),
    GLYPH_ROW(0x2108), GLYPH_ROW(0x2108), GLYPH_ROW(0x2108), GLYPH_ROW(0x2108),
    GLYPH_ROW(0x2108), GLYPH_ROW(0x3ff8), GLYPH_ROW(0x2108), GLYPH_ROW(0x0100),
    GLYPH_ROW(0x0100), GLYPH_ROW(0x0100), GLYPH_ROW(0x0100), GLYPH_ROW(0x0000)
};
static const uint8_t chinese_wen[] = {
    GLYPH_ROW(0x0200), GLYPH_ROW(0x0100), GLYPH_ROW(0x0080), GLYPH_ROW(0x7ffc),
    GLYPH_ROW(0x0820), GLYPH_ROW(0x0820), GLYPH_ROW(0x0440), GLYPH_ROW(0x0440),
    GLYPH_ROW(0x0280), GLYPH_ROW(0x0100), GLYPH_ROW(0x0280), GLYPH_ROW(0x0440),
    GLYPH_ROW(0x0820), GLYPH_ROW(0x3018), GLYPH_ROW(0x4004), GLYPH_ROW(0x0000)
};
static const uint8_t chinese_xian[] = {
    GLYPH_ROW(0x1ff0), GLYPH_ROW(0x1010), GLYPH_ROW(0x1010), GLYPH_ROW(0x1ff0),
    GLYPH_ROW(0x1010), GLYPH_ROW(0x1010), GLYPH_ROW(0x1ff0), GLYPH_ROW(0x0000),
    GLYPH_ROW(0x0440), GLYPH_ROW(0x2448), GLYPH_ROW(0x1450), GLYPH_ROW(0x0c60),
    GLYPH_ROW(0x0440), GLYPH_ROW(0x0440), GLYPH_ROW(0x7ffc), GLYPH_ROW(0x0000)
};
static const uint8_t chinese_shi[] = {
    GLYPH_ROW(0x0000), GLYPH_ROW(0x1ff0), GLYPH_ROW(0x0000), GLYPH_ROW(0x0000),
    GLYPH_ROW(0x7ffc), GLYPH_ROW(0x0100), GLYPH_ROW(0x0100), GLYPH_ROW(0x0920),
    GLYPH_ROW(0x1110), GLYPH_ROW(0x2108), GLYPH_ROW(0x4104), GLYPH_ROW(0x0100),
    GLYPH_ROW(0x0100), GLYPH_ROW(0x0500), GLYPH_ROW(0x0200), GLYPH_ROW(0x0000)
};
#undef GLYPH_ROW
static const Ssd1306Glyph chinese_glyphs[] = {
    {0x4e2d, chinese_zhong, sizeof(chinese_zhong)},
    {0x6587, chinese_wen, sizeof(chinese_wen)},
    {0x663e, chinese_xian, sizeof(chinese_xian)},
    {0x793a, chinese_shi, sizeof(chinese_shi)}
};
const Ssd1306Font ssd1306_font_chinese_demo = {
    .width = 16, .height = 16, .stride = 2,
    .glyphs = chinese_glyphs, .glyph_count = 4
};
