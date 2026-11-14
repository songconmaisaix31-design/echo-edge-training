#ifndef EPD0426_GRAPHICS_H
#define EPD0426_GRAPHICS_H
#include "epd0426.h"
/* CPU-only drawing into caller-owned 48000-byte buffer. No allocation or SPI.
 * One writer at a time; do not draw while epd0426_show borrows this buffer.
 * All shapes clip to canvas. x/y are signed; lengths include all addressed pixels.
 * Clockwise rotations change logical coordinates only, not existing pixel bytes.
 */
typedef enum {
    EPD0426_ROTATE_0, EPD0426_ROTATE_90,
    EPD0426_ROTATE_180, EPD0426_ROTATE_270
} Epd0426Rotation;
typedef struct {
    uint8_t *buffer;
    uint16_t width, height;
    Epd0426Rotation rotation;
} Epd0426Canvas;
BspStatus epd0426_canvas_init(Epd0426Canvas *canvas, uint8_t *buffer, size_t length,
                             Epd0426Rotation rotation);
BspStatus epd0426_canvas_rotate(Epd0426Canvas *canvas, Epd0426Rotation rotation);
BspStatus epd0426_canvas_fill(Epd0426Canvas *canvas, bool white);
BspStatus epd0426_canvas_invert(Epd0426Canvas *canvas);
BspStatus epd0426_pixel(Epd0426Canvas *canvas, int16_t x, int16_t y, bool white);
/* Unlike drawing, read outside canvas returns INVALID_ARG. */
BspStatus epd0426_pixel_read(const Epd0426Canvas *canvas, int16_t x, int16_t y, bool *white);
BspStatus epd0426_line(Epd0426Canvas *canvas, int16_t x0, int16_t y0,
                      int16_t x1, int16_t y1, bool white);
BspStatus epd0426_rect(Epd0426Canvas *canvas, int16_t x, int16_t y,
                      uint16_t width, uint16_t height, bool filled, bool white);
/* Radius 0..1600; larger values rejected to bound work. */
BspStatus epd0426_circle(Epd0426Canvas *canvas, int16_t x, int16_t y,
                        uint16_t radius, bool filled, bool white);
/* Row-major MSB-first 1bpp, stride=ceil(width/8); white bits may be transparent.
 * Source must not overlap canvas; source length must cover stride*height.
 * Suitable for user-provided icons and Chinese glyph bitmaps. */
BspStatus epd0426_bitmap(Epd0426Canvas *canvas, int16_t x, int16_t y,
                        uint16_t width, uint16_t height, const uint8_t *source,
                        size_t length, bool transparent_white);
/* Built-in 5x7 font, 6x8 cell, scale 1..8. Explicit length <=1024 bytes.
 * ASCII a-z rendered as A-Z; digits and printable punctuation supported.
 * Unsupported bytes render '?' (UTF-8 is not decoded). LF starts next line;
 * CR ignored; no automatic wrap. Background can be transparent.
 * Text never changes the display until caller invokes epd0426_show. */
BspStatus epd0426_text(Epd0426Canvas *canvas, int16_t x, int16_t y,
                      const char *text, size_t length, uint8_t scale,
                      bool white, bool transparent_background);
#endif

