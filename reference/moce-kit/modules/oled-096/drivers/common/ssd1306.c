#include "ssd1306.h"
#include <string.h>
#include "ssd1306_graphics.inc"
#include "ssd1306_font.inc"

static BspStatus write_packet(Ssd1306 *display, const uint8_t *data, size_t count)
{
    return bsp_i2c_write(display->config.port, display->config.address,
                         data, count, display->config.timeout_ms);
}

BspStatus ssd1306_init(Ssd1306 *display, const Ssd1306Config *config)
{
    if (!display || !config || (config->port >> 8) != BSP_I2C ||
        (config->address != 0x3cu && config->address != 0x3du) ||
        !config->timeout_ms || config->timeout_ms > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (display->configured)
    {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->port, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK)
    {
        return status;
    }
    display->config = *config;
    display->configured = true;
    display->initialized = false;
    /* Page addressing: B0..B7 and low/high column commands are valid in this mode.
     * All command parameters stay within one command-control I2C transaction. */
    const uint8_t setup[] = {
        0x00, 0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x02,
        config->flip ? 0xa1 : 0xa0, config->flip ? 0xc8 : 0xc0,
        0xda, 0x12, 0x81, config->contrast, 0xd9, 0xf1, 0xdb, 0x20,
        0x2e, 0xa4, 0xa6
    };
    status = write_packet(display, setup, sizeof(setup));
    if (status != BSP_OK)
    {
        return status;
    }
    memset(display->framebuffer, 0, sizeof(display->framebuffer));
    display->initialized = true;
    status = ssd1306_flush(display);
    if (status == BSP_OK)
    {
        const uint8_t on[] = {0x00, 0xaf};
        status = write_packet(display, on, sizeof(on));
    }
    if (status != BSP_OK)
    {
        display->initialized = false;
    }
    return status;
}

BspStatus ssd1306_clear(Ssd1306 *display, bool on)
{
    if (!display)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    memset(display->framebuffer, on ? 0xff : 0, sizeof(display->framebuffer));
    return BSP_OK;
}

BspStatus ssd1306_pixel(Ssd1306 *display, uint8_t x, uint8_t y, bool on)
{
    if (!display || x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    size_t index = (size_t)x + (size_t)(y / 8u) * SSD1306_WIDTH;
    uint8_t mask = (uint8_t)(1u << (y % 8u));
    if (on)
    {
        display->framebuffer[index] |= mask;
    }
    else
    {
        display->framebuffer[index] &= (uint8_t)~mask;
    }
    return BSP_OK;
}

BspStatus ssd1306_flush(Ssd1306 *display)
{
    if (!display)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    for (uint8_t page = 0; page < 8u; ++page)
    {
        const uint8_t position[] = {0x00, (uint8_t)(0xb0u + page), 0x00, 0x10};
        BspStatus status = write_packet(display, position, sizeof(position));
        if (status != BSP_OK)
        {
            return status;
        }
        uint8_t packet[129];
        packet[0] = 0x40;
        memcpy(packet + 1, display->framebuffer + (size_t)page * 128u, 128u);
        status = write_packet(display, packet, sizeof(packet));
        if (status != BSP_OK)
        {
            return status;
        }
    }
    return BSP_OK;
}

static int glyph_index(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'A' && character <= 'Z')
    {
        return character - 'A' + 10;
    }
    return character == ' ' ? 36 : -1;
}

BspStatus ssd1306_text(Ssd1306 *display, uint8_t x, uint8_t y,
                       const char *text, size_t length)
{
    static const uint8_t font[37][5] = {
        {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7},
        {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,2,2,2},
        {7,5,7,5,7}, {7,5,7,1,7},
        {2,5,7,5,5}, {6,5,6,5,6}, {3,4,4,4,3}, {6,5,5,5,6},
        {7,4,6,4,7}, {7,4,6,4,4}, {3,4,5,5,3}, {5,5,7,5,5},
        {7,2,2,2,7}, {1,1,1,5,2}, {5,5,6,5,5}, {4,4,4,4,7},
        {5,7,7,5,5}, {5,7,7,7,5}, {2,5,5,5,2}, {6,5,6,4,4},
        {2,5,5,7,3}, {6,5,6,5,5}, {3,4,2,1,6}, {7,2,2,2,2},
        {5,5,5,5,7}, {5,5,5,5,2}, {5,5,7,7,5}, {5,5,2,5,5},
        {5,5,2,2,2}, {7,1,2,4,7}, {0,0,0,0,0}
    };
    if (!display || !text || !length || length > 16u ||
        x > 127u || y > 54u || length * 8u > 128u - x)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    for (size_t i = 0; i < length; ++i)
    {
        if (glyph_index(text[i]) < 0)
        {
            return BSP_INVALID_ARG;
        }
    }
    for (size_t i = 0; i < length; ++i)
    {
        int glyph = glyph_index(text[i]);
        for (uint8_t row = 0; row < 10u; ++row)
        {
            for (uint8_t col = 0; col < 8u; ++col)
            {
                bool on = col < 6u && (font[glyph][row / 2u] & (4u >> (col / 2u))) != 0;
                (void)ssd1306_pixel(display, (uint8_t)(x + i * 8u + col),
                                    (uint8_t)(y + row), on);
            }
        }
    }
    return BSP_OK;
}

BspStatus ssd1306_invert(Ssd1306 *display, bool invert)
{
    if (!display)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->initialized)
    {
        return BSP_NOT_READY;
    }
    const uint8_t command[] = {0x00, invert ? 0xa7 : 0xa6};
    return write_packet(display, command, sizeof(command));
}

BspStatus ssd1306_power_off(Ssd1306 *display)
{
    if (!display)
    {
        return BSP_INVALID_ARG;
    }
    if (!display->configured)
    {
        return BSP_NOT_READY;
    }
    const uint8_t off[] = {0x00, 0xae, 0x8d, 0x10};
    BspStatus status = write_packet(display, off, sizeof(off));
    if (status == BSP_OK)
    {
        display->configured = false;
        display->initialized = false;
    }
    return status;
}
