#include "epd0426.h"
#include "bsp_spi.h"
#include "bsp_gpio.h"
#include <string.h>
#include <limits.h>

/* SSD1677 section 6.1.3: D/C then D7..D0.
 * Table 7: 7F NOP has no display effect and terminates RAM access.
 * Prefix NOPs align whole transactions to eight 9-bit words (72 clocks).
 * NEVER append padding after a command that asserts BUSY or enters sleep.
 * BSP owns every physical clock; this is byte encoding, not GPIO bit banging.
 */
static BspStatus wait_idle(Epd0426 *d)
{
    uint32_t start = bsp_time_ms();
    for (;;) {
        bool high;
        BspStatus s = bsp_gpio_read(d->config.busy, &high);
        if (s != BSP_OK || !high) {
            return s;
        }
        if ((uint32_t)(bsp_time_ms() - start) >= d->config.busy_timeout_ms) {
            return BSP_TIMEOUT;
        }
        s = bsp_delay_ms(1);
        if (s != BSP_OK) {
            return s;
        }
    }
}
static BspStatus settle(Epd0426 *d, uint32_t delay)
{
    BspStatus s = bsp_delay_ms(delay);
    return s == BSP_OK ? wait_idle(d) : s;
}
static BspStatus command(Epd0426 *d, uint8_t cmd, const uint8_t *data,
                         size_t length, uint8_t fill)
{
    uint8_t packed[288]; /* 256 words; each flush ends on a complete 9-bit word. */
    size_t prefix = (8u - ((length + 1u) % 8u)) % 8u;
    size_t words = prefix + 1u + length;
    BspStatus s = wait_idle(d);
    if (s != BSP_OK) {
        return s;
    }
    s = bsp_gpio_write(d->config.cs, false);
    if (s != BSP_OK) {
        return s;
    }
    for (size_t offset = 0; offset < words && s == BSP_OK;) {
        size_t count = words - offset;
        if (count > 256u) {
            count = 256u;
        }
        memset(packed, 0, sizeof(packed));
        for (size_t i = 0; i < count; ++i) {
            size_t pos = offset + i;
            uint16_t word;
            if (pos < prefix) {
                word = 0x7Fu;
            } else if (pos == prefix) {
                word = cmd;
            } else {
                size_t index = pos - prefix - 1u;
                word = 0x100u | (data ? data[index] : fill);
            }
            for (unsigned b = 0; b < 9u; ++b) {
                size_t bit = i * 9u + b;
                if (word & (1u << (8u - b))) {
                    packed[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
                }
            }
        }
        s = bsp_spi_transfer(d->config.spi, packed, NULL, count * 9u / 8u,
                             d->config.transfer_timeout_ms);
        offset += count;
    }
    d->cleanup_status = bsp_gpio_write(d->config.cs, true);
    if (s == BSP_OK) {
        s = d->cleanup_status;
    }
    /* Guarantee CS high interval without relying on instruction timing. */
    if (s == BSP_OK) {
        s = bsp_delay_ms(1);
    }
    return s;
}
static BspStatus fail(Epd0426 *d, BspStatus s)
{
    if (s != BSP_OK) {
        d->fault = true;
        d->ready = false;
    }
    return s;
}
static BspStatus window(Epd0426 *d)
{
    const uint8_t x[] = {0, 0, 0x1F, 3};
    const uint8_t y[] = {0xDF, 1, 0, 0};
    const uint8_t zero[] = {0, 0};
    const uint8_t top[] = {0xDF, 1};
    BspStatus s = command(d, 0x44, x, sizeof(x), 0);
    if (s == BSP_OK) { s = command(d, 0x45, y, sizeof(y), 0); }
    if (s == BSP_OK) { s = command(d, 0x4E, zero, sizeof(zero), 0); }
    if (s == BSP_OK) { s = command(d, 0x4F, top, sizeof(top), 0); }
    return s;
}
BspStatus epd0426_init(Epd0426 *d, const Epd0426Config *c)
{
    if (!d || !c || !c->transfer_timeout_ms || !c->busy_timeout_ms ||
        c->transfer_timeout_ms > INT32_MAX || c->busy_timeout_ms > INT32_MAX ||
        (c->spi >> 8) != BSP_SPI || (c->cs >> 8) != BSP_GPIO ||
        (c->reset >> 8) != BSP_GPIO || (c->busy >> 8) != BSP_GPIO ||
        c->cs == c->reset || c->cs == c->busy || c->reset == c->busy) {
        return BSP_INVALID_ARG;
    }
    if (d->ready || d->fault) {
        return BSP_INVALID_STATE;
    }
    const BspPort ports[] = {c->spi, c->cs, c->reset, c->busy};
    for (size_t i = 0; i < 4; ++i) {
        uint32_t missing;
        BspStatus s = bsp_port_require(ports[i], BSP_CAP_BLOCKING, &missing);
        if (s != BSP_OK) { return s; }
    }
    d->config = *c;
    d->cleanup_status = BSP_OK;
    BspStatus s = bsp_gpio_write(c->cs, true);
    if (s == BSP_OK) { s = bsp_delay_ms(10); }
    if (s == BSP_OK) { s = bsp_gpio_write(c->reset, false); }
    if (s == BSP_OK) { s = bsp_delay_ms(10); }
    if (s == BSP_OK) { s = bsp_gpio_write(c->reset, true); }
    if (s == BSP_OK) { s = settle(d, 10); }
    if (s == BSP_OK) { s = command(d, 0x12, NULL, 0, 0); }
    if (s == BSP_OK) { s = settle(d, 10); }
    const uint8_t gate[] = {0xDF, 0x01, 0x02};
    const uint8_t softstart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
    const uint8_t update[] = {0x40, 0x00}; /* bypass secondary plane, single chip */
    if (s == BSP_OK) { s = command(d, 0x18, NULL, 1, 0x80); }
    if (s == BSP_OK) { s = command(d, 0x0C, softstart, sizeof(softstart), 0); }
    if (s == BSP_OK) { s = command(d, 0x01, gate, sizeof(gate), 0); }
    if (s == BSP_OK) { s = command(d, 0x11, NULL, 1, 0x01); }
    if (s == BSP_OK) { s = command(d, 0x3C, NULL, 1, 0x01); }
    if (s == BSP_OK) { s = command(d, 0x21, update, sizeof(update), 0); }
    if (s == BSP_OK) { s = window(d); }
    if (s != BSP_OK) { return fail(d, s); }
    d->ready = true;
    return BSP_OK;
}
static BspStatus display(Epd0426 *d, const uint8_t *frame, uint8_t fill)
{
    if (!d) { return BSP_INVALID_ARG; }
    if (!d->ready || d->fault) { return BSP_INVALID_STATE; }
    BspStatus s = window(d);
    if (s == BSP_OK) { s = command(d, 0x24, frame, EPD0426_FRAME_BYTES, fill); }
    if (s == BSP_OK) { s = window(d); }
    if (s == BSP_OK) { s = command(d, 0x26, NULL, EPD0426_FRAME_BYTES, 0xFF); }
    /* F7: temperature + OTP LUT load + full update + power off.
     * No forced temperature / fast-refresh waveform. */
    if (s == BSP_OK) { s = command(d, 0x22, NULL, 1, 0xF7); }
    if (s == BSP_OK) { s = command(d, 0x20, NULL, 0, 0); }
    if (s == BSP_OK) { s = settle(d, 1); }
    return fail(d, s);
}
BspStatus epd0426_show(Epd0426 *d, const uint8_t *frame, size_t length)
{
    if (!frame || length != EPD0426_FRAME_BYTES) { return BSP_INVALID_ARG; }
    return display(d, frame, 0);
}
BspStatus epd0426_clear(Epd0426 *d, bool white)
{
    return display(d, NULL, white ? 0xFF : 0x00);
}
BspStatus epd0426_sleep(Epd0426 *d)
{
    if (!d) { return BSP_INVALID_ARG; }
    if (!d->ready || d->fault) { return BSP_INVALID_STATE; }
    BspStatus s = command(d, 0x10, NULL, 1, 0x01);
    if (s == BSP_OK) { d->ready = false; }
    return fail(d, s);
}

