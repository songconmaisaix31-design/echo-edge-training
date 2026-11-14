/* Included by ssd1306.c, no extra translation unit or platform dependencies. */
static bool graphics_fits(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    return x < SSD1306_WIDTH && y < SSD1306_HEIGHT && w && h &&
           w <= SSD1306_WIDTH - x && h <= SSD1306_HEIGHT - y;
}

static void graphics_pixel(Ssd1306 *display, int x, int y, bool on)
{
    /* Internal callers prevalidate all bounds before the first write. */
    size_t index = (size_t)x + (size_t)(y / 8) * SSD1306_WIDTH;
    uint8_t mask = (uint8_t)(1u << (unsigned)(y % 8));
    if (on)
    {
        display->framebuffer[index] |= mask;
    }
    else
    {
        display->framebuffer[index] &= (uint8_t)~mask;
    }
}

BspStatus ssd1306_line(Ssd1306 *display, uint16_t x0, uint16_t y0,
                       uint16_t x1, uint16_t y1, bool on)
{
    if (!display || !graphics_fits(x0, y0, 1, 1) || !graphics_fits(x1, y1, 1, 1))
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    int x = x0, y = y0;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;)
    {
        graphics_pixel(display, x, y, on);
        if (x == x1 && y == y1)
        {
            break;
        }
        int twice = error * 2;
        if (twice >= dy)
        {
            error += dy;
            x += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            y += sy;
        }
    }
    return BSP_OK;
}

BspStatus ssd1306_rect(Ssd1306 *display, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, bool filled, bool on)
{
    if (!display || !graphics_fits(x, y, width, height))
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    for (uint16_t row = 0; row < height; ++row)
    {
        for (uint16_t col = 0; col < width; ++col)
        {
            if (filled || !row || row == height - 1u || !col || col == width - 1u)
            {
                graphics_pixel(display, x + col, y + row, on);
            }
        }
    }
    return BSP_OK;
}

static void circle_points(Ssd1306 *d, int cx, int cy, int x, int y, bool on)
{
    graphics_pixel(d, cx + x, cy + y, on);
    graphics_pixel(d, cx - x, cy + y, on);
    graphics_pixel(d, cx + x, cy - y, on);
    graphics_pixel(d, cx - x, cy - y, on);
    graphics_pixel(d, cx + y, cy + x, on);
    graphics_pixel(d, cx - y, cy + x, on);
    graphics_pixel(d, cx + y, cy - x, on);
    graphics_pixel(d, cx - y, cy - x, on);
}

BspStatus ssd1306_circle(Ssd1306 *display, uint16_t cx, uint16_t cy,
                         uint16_t radius, bool filled, bool on)
{
    if (!display || cx >= SSD1306_WIDTH || cy >= SSD1306_HEIGHT ||
        radius > cx || radius > cy ||
        radius >= SSD1306_WIDTH - cx || radius >= SSD1306_HEIGHT - cy)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    int r = radius;
    if (filled)
    {
        /* Integer disk, exact x*x+y*y <= r*r; no floating point or sqrt. */
        for (int y = -r; y <= r; ++y)
        {
            for (int x = -r; x <= r; ++x)
            {
                if (x * x + y * y <= r * r)
                {
                    graphics_pixel(display, cx + x, cy + y, on);
                }
            }
        }
    }
    else
    {
        int x = r, y = 0, error = 1 - r;
        while (x >= y)
        {
            circle_points(display, cx, cy, x, y, on);
            ++y;
            if (error < 0)
            {
                error += 2 * y + 1;
            }
            else
            {
                --x;
                error += 2 * (y - x) + 1;
            }
        }
    }
    return BSP_OK;
}

static bool bitmap_layout(uint16_t w, uint16_t h, size_t stride, size_t *needed)
{
    if (!w || !h || w > SSD1306_WIDTH || h > SSD1306_HEIGHT)
    {
        return false;
    }
    size_t row_bytes = ((size_t)w + 7u) / 8u;
    if (stride < row_bytes || (h > 1 && stride > (SIZE_MAX - row_bytes) / (h - 1u)))
    {
        return false;
    }
    *needed = (size_t)(h - 1u) * stride + row_bytes;
    return true;
}

static void bitmap_draw(Ssd1306 *d, uint16_t x, uint16_t y,
                        uint16_t w, uint16_t h, const uint8_t *data,
                        size_t stride, bool foreground, bool transparent)
{
    for (uint16_t row = 0; row < h; ++row)
    {
        for (uint16_t col = 0; col < w; ++col)
        {
            bool bit = (data[(size_t)row * stride + col / 8u] & (0x80u >> (col % 8u))) != 0;
            if (bit || !transparent)
            {
                graphics_pixel(d, x + col, y + row, bit ? foreground : !foreground);
            }
        }
    }
}

BspStatus ssd1306_bitmap(Ssd1306 *display, uint16_t x, uint16_t y,
                         uint16_t width, uint16_t height,
                         const uint8_t *data, size_t data_size, size_t stride,
                         bool foreground, bool transparent)
{
    size_t needed = 0;
    if (!display || !data || !graphics_fits(x, y, width, height) ||
        !bitmap_layout(width, height, stride, &needed) || data_size < needed)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    bitmap_draw(display, x, y, width, height, data, stride, foreground, transparent);
    return BSP_OK;
}

static bool utf8_next(const char *text, size_t length, size_t *offset, uint32_t *codepoint)
{
    const uint8_t *bytes = (const uint8_t *)text;
    size_t pos = *offset;
    uint8_t first = bytes[pos++];
    uint32_t cp, minimum;
    unsigned rest;
    if (first < 0x80)
    {
        *codepoint = first;
        *offset = pos;
        return true;
    }
    if (first >= 0xc2 && first <= 0xdf)
    {
        cp = first & 0x1fu; minimum = 0x80; rest = 1;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
        cp = first & 0x0fu; minimum = 0x800; rest = 2;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
        cp = first & 7u; minimum = 0x10000; rest = 3;
    }
    else
    {
        return false;
    }
    if (length - pos < rest)
    {
        return false;
    }
    for (unsigned i = 0; i < rest; ++i)
    {
        uint8_t next = bytes[pos++];
        if ((next & 0xc0u) != 0x80u)
        {
            return false;
        }
        cp = (cp << 6) | (next & 0x3fu);
    }
    if (cp < minimum || cp > 0x10ffffu || (cp >= 0xd800 && cp <= 0xdfff))
    {
        return false;
    }
    *codepoint = cp;
    *offset = pos;
    return true;
}

static const Ssd1306Glyph *font_find(const Ssd1306Font *font, uint32_t cp)
{
    for (size_t i = 0; i < font->glyph_count; ++i)
    {
        if (font->glyphs[i].codepoint == cp)
        {
            return &font->glyphs[i];
        }
    }
    return NULL;
}

BspStatus ssd1306_text_utf8(Ssd1306 *display, uint16_t x, uint16_t y,
                            const char *text, size_t byte_length,
                            const Ssd1306Font *font, bool foreground, bool transparent)
{
    size_t needed = 0;
    if (!display || !text || !byte_length || !font || !font->glyphs ||
        !font->glyph_count || !bitmap_layout(font->width, font->height, font->stride, &needed) ||
        !graphics_fits(x, y, font->width, font->height))
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    size_t offset = 0, count = 0;
    while (offset < byte_length)
    {
        uint32_t cp;
        if (!utf8_next(text, byte_length, &offset, &cp))
        {
            return BSP_INVALID_ARG;
        }
        if (++count > (SSD1306_WIDTH - x) / font->width)
        {
            return BSP_INVALID_ARG;
        }
        const Ssd1306Glyph *glyph = font_find(font, cp);
        if (!glyph)
        {
            return BSP_UNSUPPORTED;
        }
        if (!glyph->data || glyph->data_size < needed)
        {
            return BSP_INVALID_ARG;
        }
    }
    offset = 0;
    while (offset < byte_length)
    {
        uint32_t cp = 0;
        (void)utf8_next(text, byte_length, &offset, &cp);
        const Ssd1306Glyph *glyph = font_find(font, cp);
        bitmap_draw(display, x, y, font->width, font->height, glyph->data,
                    font->stride, foreground, transparent);
        x = (uint16_t)(x + font->width);
    }
    return BSP_OK;
}
