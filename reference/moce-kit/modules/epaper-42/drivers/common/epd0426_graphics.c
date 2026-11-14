#include "epd0426_graphics.h"
#include <string.h>
static bool valid(const Epd0426Canvas *c)
{
    return c && c->buffer && (unsigned)c->rotation <= EPD0426_ROTATE_270 &&
        c->width == ((c->rotation & 1) ? 480u : 800u) &&
        c->height == ((c->rotation & 1) ? 800u : 480u);
}
static bool inside(const Epd0426Canvas *c, int32_t x, int32_t y)
{
    return x >= 0 && y >= 0 && x < c->width && y < c->height;
}
static size_t address(const Epd0426Canvas *c, int32_t x, int32_t y, uint8_t *mask)
{
    int32_t px = x, py = y;
    switch (c->rotation) {
    case EPD0426_ROTATE_90: px = 799 - y; py = x; break;
    case EPD0426_ROTATE_180: px = 799 - x; py = 479 - y; break;
    case EPD0426_ROTATE_270: px = y; py = 479 - x; break;
    default: break;
    }
    *mask = (uint8_t)(0x80u >> (px % 8));
    return (size_t)py * 100u + (size_t)px / 8u;
}
static void pixel(Epd0426Canvas *c, int32_t x, int32_t y, bool white)
{
    if (inside(c, x, y)) {
        uint8_t mask;
        size_t i = address(c, x, y, &mask);
        if (white) { c->buffer[i] |= mask; }
        else { c->buffer[i] &= (uint8_t)~mask; }
    }
}
static void span(Epd0426Canvas *c, int32_t x0, int32_t x1, int32_t y, bool white)
{
    if (y < 0 || y >= c->height) { return; }
    if (x0 < 0) { x0 = 0; }
    if (x1 >= c->width) { x1 = c->width - 1; }
    for (int32_t x = x0; x <= x1; ++x) { pixel(c, x, y, white); }
}
BspStatus epd0426_canvas_rotate(Epd0426Canvas *c, Epd0426Rotation rotation)
{
    if (!valid(c) || (unsigned)rotation > EPD0426_ROTATE_270) { return BSP_INVALID_ARG; }
    c->rotation = rotation;
    c->width = (rotation & 1) ? 480 : 800;
    c->height = (rotation & 1) ? 800 : 480;
    return BSP_OK;
}
BspStatus epd0426_canvas_init(Epd0426Canvas *c, uint8_t *buffer, size_t length,
                             Epd0426Rotation rotation)
{
    if (!c || !buffer || length != EPD0426_FRAME_BYTES ||
        (unsigned)rotation > EPD0426_ROTATE_270) { return BSP_INVALID_ARG; }
    c->buffer = buffer;
    c->width = 800;
    c->height = 480;
    c->rotation = EPD0426_ROTATE_0;
    return epd0426_canvas_rotate(c, rotation);
}
BspStatus epd0426_canvas_fill(Epd0426Canvas *c, bool white)
{
    if (!valid(c)) { return BSP_INVALID_ARG; }
    memset(c->buffer, white ? 0xFF : 0, EPD0426_FRAME_BYTES);
    return BSP_OK;
}
BspStatus epd0426_canvas_invert(Epd0426Canvas *c)
{
    if (!valid(c)) { return BSP_INVALID_ARG; }
    for (size_t i = 0; i < EPD0426_FRAME_BYTES; ++i) { c->buffer[i] ^= 0xFFu; }
    return BSP_OK;
}
BspStatus epd0426_pixel(Epd0426Canvas *c, int16_t x, int16_t y, bool white)
{
    if (!valid(c)) { return BSP_INVALID_ARG; }
    pixel(c, x, y, white);
    return BSP_OK;
}
BspStatus epd0426_pixel_read(const Epd0426Canvas *c, int16_t x, int16_t y, bool *white)
{
    if (!valid(c) || !white || !inside(c, x, y)) { return BSP_INVALID_ARG; }
    uint8_t mask;
    size_t i = address(c, x, y, &mask);
    *white = (c->buffer[i] & mask) != 0;
    return BSP_OK;
}
BspStatus epd0426_line(Epd0426Canvas *c, int16_t ax, int16_t ay,
                      int16_t bx, int16_t by, bool white)
{
    if (!valid(c)) { return BSP_INVALID_ARG; }
    int32_t x = ax, y = ay, dx = bx - x, dy = by - y;
    int32_t sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) { dx = -dx; }
    if (dy > 0) { dy = -dy; }
    int32_t err = dx + dy;
    for (;;) {
        pixel(c, x, y, white);
        if (x == bx && y == by) { break; }
        int32_t twice = 2 * err;
        if (twice >= dy) { err += dy; x += sx; }
        if (twice <= dx) { err += dx; y += sy; }
    }
    return BSP_OK;
}
BspStatus epd0426_rect(Epd0426Canvas *c, int16_t x, int16_t y,
                      uint16_t w, uint16_t h, bool filled, bool white)
{
    if (!valid(c)) { return BSP_INVALID_ARG; }
    if (!w || !h) { return BSP_OK; }
    int32_t right = (int32_t)x + w - 1, bottom = (int32_t)y + h - 1;
    int32_t top = y < 0 ? 0 : y, end = bottom;
    if (end >= c->height) { end = c->height - 1; }
    for (int32_t row = top; row <= end; ++row) {
        if (filled || row == y || row == bottom) { span(c, x, right, row, white); }
        else { pixel(c, x, row, white); pixel(c, right, row, white); }
    }
    return BSP_OK;
}
BspStatus epd0426_circle(Epd0426Canvas *c, int16_t cx, int16_t cy,
                        uint16_t radius, bool filled, bool white)
{
    if (!valid(c) || radius > 1600u) { return BSP_INVALID_ARG; }
    int32_t x = radius, y = 0, err = 1 - x;
    while (x >= y) {
        if (filled) {
            span(c, cx - x, cx + x, cy + y, white);
            span(c, cx - x, cx + x, cy - y, white);
            span(c, cx - y, cx + y, cy + x, white);
            span(c, cx - y, cx + y, cy - x, white);
        } else {
            pixel(c, cx+x, cy+y, white); pixel(c, cx-x, cy+y, white);
            pixel(c, cx+x, cy-y, white); pixel(c, cx-x, cy-y, white);
            pixel(c, cx+y, cy+x, white); pixel(c, cx-y, cy+x, white);
            pixel(c, cx+y, cy-x, white); pixel(c, cx-y, cy-x, white);
        }
        ++y;
        if (err < 0) { err += 2*y + 1; }
        else { --x; err += 2*(y-x) + 1; }
    }
    return BSP_OK;
}
BspStatus epd0426_bitmap(Epd0426Canvas *c, int16_t x, int16_t y,
                        uint16_t w, uint16_t h, const uint8_t *source,
                        size_t length, bool transparent)
{
    if (!valid(c) || !source) { return BSP_INVALID_ARG; }
    size_t stride = ((size_t)w + 7u) / 8u, needed = stride * h;
    if (length < needed) { return BSP_INVALID_ARG; }
    uintptr_t a = (uintptr_t)source, b = (uintptr_t)c->buffer;
    if (needed && (a >= b ? a-b < EPD0426_FRAME_BYTES : b-a < needed)) {
        return BSP_INVALID_ARG;
    }
    int32_t y0 = y < 0 ? 0 : y, y1 = (int32_t)y + h;
    int32_t x0 = x < 0 ? 0 : x, x1 = (int32_t)x + w;
    if (y1 > c->height) { y1 = c->height; }
    if (x1 > c->width) { x1 = c->width; }
    for (int32_t row = y0; row < y1; ++row) {
        for (int32_t col = x0; col < x1; ++col) {
            size_t sx = (size_t)(col - x), sy = (size_t)(row - y);
            bool white = (source[sy * stride + sx / 8u] & (0x80u >> (sx % 8u))) != 0;
            if (!transparent || !white) { pixel(c, col, row, white); }
        }
    }
    return BSP_OK;
}
/* Compact hand-specified 5x7 column masks; bit zero is top row.
 * Lowercase intentionally folds to uppercase for a readable small ROM font. */
static const uint8_t alphabet[36][5] = {
 {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
 {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
 {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
 {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
 {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
 {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
 {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
 {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
 {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
 {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
 {0x7F,0x20,0x18,0x20,0x7F},{0x63,0x14,0x08,0x14,0x63},
 {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},
 {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
 {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
 {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
 {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E}
};
static void glyph(unsigned ch, uint8_t out[5])
{
    static const char symbols[] = " !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    static const uint8_t punctuation[][5] = {
      {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},
      {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
      {0,5,3,0,0},{0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},
      {0x14,8,0x3E,8,0x14},{8,8,0x3E,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},
      {0,0x60,0x60,0,0},{0x20,0x10,8,4,2},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},
      {8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},
      {2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3E},{0,0x7F,0x41,0x41,0},
      {2,4,8,0x10,0x20},{0,0x41,0x41,0x7F,0},{4,2,1,2,4},
      {0x40,0x40,0x40,0x40,0x40},{0,1,2,4,0},{0,8,0x36,0x41,0},
      {0,0,0x7F,0,0},{0,0x41,0x36,8,0},{8,4,8,0x10,8}
    };
    if (ch >= 'a' && ch <= 'z') { ch -= 'a' - 'A'; }
    if (ch >= 'A' && ch <= 'Z') { memcpy(out, alphabet[ch-'A'], 5); return; }
    if (ch >= '0' && ch <= '9') { memcpy(out, alphabet[26+ch-'0'], 5); return; }
    const char *p = ch && ch < 128 ? strchr(symbols, (int)ch) : NULL;
    if (!p) { p = strchr(symbols, '?'); }
    memcpy(out, punctuation[p-symbols], 5);
}
BspStatus epd0426_text(Epd0426Canvas *c, int16_t x, int16_t y,
                      const char *text, size_t length, uint8_t scale,
                      bool white, bool transparent)
{
    if (!valid(c) || !text || length > 1024u || !scale || scale > 8u) { return BSP_INVALID_ARG; }
    int32_t px = x, py = y;
    for (size_t i = 0; i < length; ++i) {
        unsigned ch = (unsigned char)text[i];
        if (ch == '\n') { px = x; py += 8 * scale; continue; }
        if (ch == '\r') { continue; }
        uint8_t columns[5];
        glyph(ch, columns);
        for (unsigned row = 0; row < 8; ++row) {
            for (unsigned col = 0; col < 6; ++col) {
                bool ink = col < 5 && row < 7 && (columns[col] & (1u << row));
                if (!ink && transparent) { continue; }
                for (unsigned sy = 0; sy < scale; ++sy) {
                    for (unsigned sx = 0; sx < scale; ++sx) {
                        pixel(c, px + (int32_t)(col*scale+sx), py + (int32_t)(row*scale+sy),
                              ink ? white : !white);
                    }
                }
            }
        }
        px += 6 * scale;
    }
    return BSP_OK;
}

