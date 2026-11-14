#ifndef MOCE_SSD1306_H
#define MOCE_SSD1306_H
#include "bsp_i2c.h"

#define SSD1306_WIDTH 128u
#define SSD1306_HEIGHT 64u
#define SSD1306_BUFFER_SIZE 1024u

/* SSD1306 128x64, internal charge pump, I2C write-only.
 * Requires initialized public BSP blocking I2C. Address is 7-bit 0x3C/0x3D.
 * Caller zero-initializes the instance and serializes all access. No heap/ISR use.
 * Config scalars are copied; framebuffer is owned by the instance.
 * Power/reset settling is the application's responsibility before init.
 * Each write has its own native timeout, not a hard whole-frame deadline.
 * Errors stop immediately, without retry/recover. Failed init requires explicit
 * power_off before re-init; failed power_off retains state for cleanup.
 * Driver does not initialize/deinitialize the shared BSP bus. */
typedef struct
{
    BspPort port;
    uint8_t address;
    uint32_t timeout_ms;
    uint8_t contrast;
    bool flip;
} Ssd1306Config;

typedef struct
{
    Ssd1306Config config;
    uint8_t framebuffer[SSD1306_BUFFER_SIZE];
    bool configured;
    bool initialized;
} Ssd1306;

BspStatus ssd1306_init(Ssd1306 *display, const Ssd1306Config *config);
BspStatus ssd1306_clear(Ssd1306 *display, bool on);
BspStatus ssd1306_pixel(Ssd1306 *display, uint8_t x, uint8_t y, bool on);
/* Original 3x5 font scaled 2x: A-Z, 0-9 and space. Opaque 8x10 cells.
 * Explicit length <=16, no automatic wrapping; validates whole text first. */
BspStatus ssd1306_text(Ssd1306 *display, uint8_t x, uint8_t y,
                       const char *text, size_t length);
/* All drawing only changes the framebuffer; flush explicitly afterwards.
 * Shapes must fit entirely on screen (no clipping). Invalid arguments leave the
 * framebuffer unchanged. Rectangle dimensions are positive; radius 0 is a pixel.
 * Outline/fill write 'on'; they never implicitly clear the rest of the shape. */
BspStatus ssd1306_line(Ssd1306 *display, uint16_t x0, uint16_t y0,
                       uint16_t x1, uint16_t y1, bool on);
BspStatus ssd1306_rect(Ssd1306 *display, uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, bool filled, bool on);
BspStatus ssd1306_circle(Ssd1306 *display, uint16_t cx, uint16_t cy,
                         uint16_t radius, bool filled, bool on);

/* 1-bit bitmap: rows top-to-bottom; MSB is leftmost pixel, stride in bytes.
 * width/height positive and <= screen; stride >= ceil(width/8).
 * data_size covers (height-1)*stride+ceil(width/8).
 * Caller supplies valid immutable, non-overlapping source storage during call.
 * Set bits write foreground. Zero bits write !foreground unless transparent.
 * Arbitrary dimensions/unaligned y and padded rows supported; no format guessing. */
BspStatus ssd1306_bitmap(Ssd1306 *display, uint16_t x, uint16_t y,
                         uint16_t width, uint16_t height,
                         const uint8_t *data, size_t data_size, size_t stride,
                         bool foreground, bool transparent);

/* Fixed-cell Unicode font, same row-major MSB bitmap layout as bitmap().
 * Tables/bitmap buffers are borrowed only during the call. First matching glyph
 * wins; caller supplies unique Unicode scalar values. No shaping or wrapping.
 * UTF-8 byte length is explicit and nonzero. Strict UTF-8; missing glyph returns
 * BSP_UNSUPPORTED. The whole run is checked before any framebuffer changes.
 * Built-in original 16x16 sample font covers only U+4E2D/6587/663E/793A
 * (Chinese phrase "中文显示"), not a complete Chinese font. Supply another
 * Ssd1306Font to render any other Chinese characters without changing driver. */
typedef struct
{
    uint32_t codepoint;
    const uint8_t *data;
    size_t data_size;
} Ssd1306Glyph;
typedef struct
{
    uint16_t width, height;
    size_t stride;
    const Ssd1306Glyph *glyphs;
    size_t glyph_count;
} Ssd1306Font;
extern const Ssd1306Font ssd1306_font_chinese_demo;
BspStatus ssd1306_text_utf8(Ssd1306 *display, uint16_t x, uint16_t y,
                            const char *text, size_t byte_length,
                            const Ssd1306Font *font, bool foreground,
                            bool transparent);
BspStatus ssd1306_flush(Ssd1306 *display);
BspStatus ssd1306_invert(Ssd1306 *display, bool invert);
BspStatus ssd1306_power_off(Ssd1306 *display);
#endif
