#ifndef MOCE_ST7789_H
#define MOCE_ST7789_H
#include "bsp_display.h"
#include "bsp_gpio.h"
/* Single main-context owner. Zero-initialize; never copy a live instance.
 * Keep the instance alive after any timeout: BSP may still borrow its buffer.
 * Commands and RGB565 data are copied into instance storage before submit.
 * Panel-specific voltage/gamma setup belongs in a verified project profile. */
typedef struct {
    BspDisplayBinding bus;
    BspPort reset;
    uint16_t width, height, x_offset, y_offset;
    uint8_t madctl;
    bool inverted;
    uint32_t timeout_ms;
} St7789Config;
typedef struct {
    St7789Config config;
    BspRequest request;
    uint8_t data[512];
    bool initialized, active, failed, sleeping;
} St7789;
BspStatus st7789_init(St7789 *display, const St7789Config *config);
BspStatus st7789_command(St7789 *display, uint8_t command, const uint8_t *data, size_t length);
BspStatus st7789_fill_rect(St7789 *display, uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t rgb565);
/* Retire an accepted operation only after terminal buffer return; never cancels. */
BspStatus st7789_finish(St7789 *display);
/* Native uint16_t RGB565 input, row-major, exact pixel_count=w*h.
 * Copied in chunks: caller input is no longer borrowed on every return. */
BspStatus st7789_write_pixels(St7789 *d, uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *pixels, size_t pixel_count);
/* Full 240x320 panel only, zero offsets. Rotation 0..3 is clockwise. */
BspStatus st7789_set_rotation(St7789 *d, unsigned rotation);
BspStatus st7789_sleep(St7789 *d);
BspStatus st7789_wake(St7789 *d);
/* Bus must be ready/reinitialized by its owner. Refuses outstanding borrowing. */
BspStatus st7789_recover(St7789 *d);
#endif
