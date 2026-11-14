#include "ws2812b_do_dma.h"
#include <string.h>

enum { GUARD_CELLS = 1024, CELLS_PER_BIT = 4 };
_Static_assert(2 * GUARD_CELLS + WS2812B_DO_PIXELS * 24 * CELLS_PER_BIT == WS2812B_DO_CELLS,
               "One whole frame, guarded by reset-low cells on both sides");
static BspStatus editable(const Ws2812bDo *d)
{
    if (!d) return BSP_INVALID_ARG;
    if (!d->initialized) return BSP_NOT_READY;
    return d->state.borrowed ? BSP_INVALID_STATE : BSP_OK;
}
static size_t pixel_index(const Ws2812bDo *d, unsigned x, unsigned y)
{
    if (d->config.layout.flip_x) x = 7u - x;
    if (d->config.layout.flip_y) y = 7u - y;
    unsigned line = d->config.layout.column_major ? x : y;
    unsigned offset = d->config.layout.column_major ? y : x;
    if (d->config.layout.serpentine && (line & 1u)) offset = 7u - offset;
    return line * 8u + offset;
}
BspStatus ws2812b_do_init(Ws2812bDo *d, const Ws2812bDoConfig *c)
{
    if (!d || !c) return BSP_INVALID_ARG;
    if (d->initialized) return BSP_INVALID_STATE;
    uintptr_t words = (uintptr_t)c->words, object = (uintptr_t)d;
    const size_t bytes = WS2812B_DO_CELLS * sizeof(uint32_t);
    if ((c->port >> 8) != BSP_GPIO || !c->pin_mask || (c->pin_mask & (c->pin_mask - 1u)) ||
        !words || (words & 3u) || c->capacity < WS2812B_DO_CELLS ||
        bytes > UINTPTR_MAX - words || sizeof(*d) > UINTPTR_MAX - object ||
        (words < object + sizeof(*d) && object < words + bytes) ||
        c->period_ticks < 2u || c->period_ticks > 65536u ||
        (uint64_t)c->tick_hz < 3000000ull * c->period_ticks ||
        (uint64_t)c->tick_hz > 3200000ull * c->period_ticks) return BSP_INVALID_ARG;
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(c->port, BSP_CAP_BLOCKING, &missing);
    if (s != BSP_OK) return s;
    d->config = *c;
    memset(d->pixels, 0, sizeof(d->pixels));
    d->state = (Ws2812bDoState){0};
    d->initialized = true;
    return BSP_OK;
}
BspStatus ws2812b_do_set_pixel(Ws2812bDo *d, unsigned x, unsigned y, Ws2812bDoColor color)
{
    BspStatus s = editable(d);
    if (s != BSP_OK) return s;
    if (x >= 8u || y >= 8u) return BSP_INVALID_ARG;
    d->pixels[pixel_index(d, x, y)] = color;
    return BSP_OK;
}
BspStatus ws2812b_do_fill(Ws2812bDo *d, Ws2812bDoColor color)
{
    BspStatus s = editable(d);
    if (s != BSP_OK) return s;
    for (size_t i = 0; i < WS2812B_DO_PIXELS; ++i) d->pixels[i] = color;
    return BSP_OK;
}
BspStatus ws2812b_do_draw_bitmap(Ws2812bDo *d, const uint8_t rows[8],
                                Ws2812bDoColor foreground, Ws2812bDoColor background)
{
    BspStatus s = editable(d);
    if (s != BSP_OK) return s;
    if (!rows) return BSP_INVALID_ARG;
    for (unsigned y = 0; y < 8u; ++y)
        for (unsigned x = 0; x < 8u; ++x)
            d->pixels[pixel_index(d, x, y)] = (rows[y] & (0x80u >> x)) ? foreground : background;
    return BSP_OK;
}
BspStatus ws2812b_do_set_brightness(Ws2812bDo *d, uint8_t brightness)
{
    BspStatus s = editable(d);
    if (s == BSP_OK) d->config.brightness = brightness;
    return s;
}
static void encode(Ws2812bDo *d)
{
    const uint32_t high = d->config.pin_mask, low = high << 16;
    uint32_t *p = d->config.words;
    for (size_t i = 0; i < WS2812B_DO_CELLS; ++i) p[i] = low;
    p += GUARD_CELLS;
    for (size_t i = 0; i < WS2812B_DO_PIXELS; ++i) {
        Ws2812bDoColor color = d->pixels[i];
        const uint8_t grb[] = {color.green, color.red, color.blue};
        for (unsigned channel = 0; channel < 3u; ++channel) {
            uint8_t value = (uint8_t)((unsigned)grb[channel] * d->config.brightness / 255u);
            for (unsigned bit = 0; bit < 8u; ++bit) {
                *p++ = high;
                *p++ = (value & 0x80u) ? high : low;
                *p++ = low;
                *p++ = low;
                value = (uint8_t)(value << 1);
            }
        }
    }
}
BspStatus ws2812b_do_start(Ws2812bDo *d)
{
    BspStatus s = editable(d);
    if (s != BSP_OK) return s;
    /* The extension is a global exclusive session. Do not adopt another owner
     * after a BUSY return or stop somebody else's transfer. */
    BspTimerDmaGpioState before = {0};
    s = bsp_timer_dma_gpio_query(&before);
    if (s == BSP_OK) return BSP_BUSY;
    if (s != BSP_NOT_READY) return s;
    encode(d);
    const BspTimerDmaGpioConfig config = {
        .timer = d->config.timer, .gpio = d->config.port, .pin_mask = d->config.pin_mask,
        .tick_hz = d->config.tick_hz, .period_ticks = d->config.period_ticks,
        .sequence = d->config.words, .elements = WS2812B_DO_CELLS, .circular = true};
    d->state = (Ws2812bDoState){.borrowed = true};
    s = bsp_timer_dma_gpio_start(&config);
    if (s == BSP_OK) return BSP_OK;
    d->state.error = s;
    if (s == BSP_IO || s == BSP_TIMEOUT) {
        BspTimerDmaGpioState after = {0};
        BspStatus q = bsp_timer_dma_gpio_query(&after);
        if (q == BSP_OK) { d->state.dma = after; d->state.borrowed = after.active; }
        else if (q == BSP_NOT_READY) d->state.borrowed = false;
        /* Unknown cleanup state retains all storage until a successful stop. */
    } else d->state.borrowed = false;
    return s;
}
BspStatus ws2812b_do_query(Ws2812bDo *d, Ws2812bDoState *out)
{
    if (!d || !out) return BSP_INVALID_ARG;
    if (!d->initialized) return BSP_NOT_READY;
    if (d->state.borrowed) {
        BspTimerDmaGpioState dma = {0};
        BspStatus s = bsp_timer_dma_gpio_query(&dma);
        if (s != BSP_OK) { *out = d->state; return s; }
        d->state.dma = dma;
        if (dma.failed && d->state.error == BSP_OK) d->state.error = BSP_IO;
        /* Finite completion/error never authorizes reuse before stop=OK. */
    }
    *out = d->state;
    return BSP_OK;
}
BspStatus ws2812b_do_stop(Ws2812bDo *d, Ws2812bDoState *out)
{
    if (!d || !out) return BSP_INVALID_ARG;
    if (!d->initialized) return BSP_NOT_READY;
    BspStatus s = BSP_OK;
    if (d->state.borrowed) {
        s = bsp_timer_dma_gpio_stop();
        if (s == BSP_OK) {
            d->state.borrowed = false;
            d->state.dma.active = false;
            d->state.dma.remaining = 0;
        }
    }
    *out = d->state;
    return s;
}
BspStatus ws2812b_do_deinit(Ws2812bDo *d)
{
    BspStatus s = editable(d);
    if (s == BSP_OK) d->initialized = false;
    return s;
}
