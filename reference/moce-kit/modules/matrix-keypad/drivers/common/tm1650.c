#include "tm1650.h"
#include <string.h>

BspStatus tm1650_init(Tm1650 *d, const Tm1650Config *c)
{
    if (!d || !c || c->address != TM1650_ADDRESS ||
        (c->port >> 8) != BSP_I2C || c->timeout_ms == 0u ||
        c->timeout_ms > 100u || c->powerup_ms < 100u || c->powerup_ms > 2000u) {
        return BSP_INVALID_ARG;
    }
    if (d->initialized) {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(c->port, BSP_CAP_BLOCKING, &missing);
    if (s != BSP_OK) {
        return s;
    }
    d->config = *c;
    s = bsp_delay_ms(c->powerup_ms);
    /* Blank all segment outputs before enabling normal scanning. */
    const uint8_t blank = 0;
    for (unsigned i = 0; s == BSP_OK && i < 4u; ++i) {
        s = bsp_i2c_write(c->port, (uint8_t)(0x34u + i), &blank, 1u, c->timeout_ms);
    }
    if (s == BSP_OK) {
        const uint8_t control = 0x11u; /* lowest brightness, normal, 8 segments, ON */
        s = bsp_i2c_write(c->port, c->address, &control, 1u, c->timeout_ms);
    }
    if (s == BSP_OK) {
        s = bsp_delay_ms(c->powerup_ms); /* allow initial key scan to settle */
    }
    d->initialized = s == BSP_OK;
    return s;
}

BspStatus tm1650_read(Tm1650 *d, Tm1650Reading *out)
{
    if (!d || !out) {
        return BSP_INVALID_ARG;
    }
    if (!d->initialized) {
        return BSP_INVALID_STATE;
    }
    uint8_t raw = 0;
    BspStatus s = bsp_i2c_read(d->config.port, d->config.address, &raw, 1u,
                             d->config.timeout_ms);
    if (s != BSP_OK) {
        d->initialized = false;
        return s;
    }
    Tm1650Reading value = {0};
    value.raw = raw;
    value.pressed = (raw & 0x40u) != 0u;
    /* Datasheet permits arbitrary release bytes <0x40, including power-up 0x2e. */
    if (raw < 0x40u) {
        value.recognized = true;
    } else if ((raw & 0xc4u) == 0x44u) {
        unsigned ki = (raw >> 3) & 7u;
        value.row = (uint8_t)((raw & 3u) + 1u);
        if (ki < 4u) {
            value.column = (uint8_t)(ki + 1u);
            value.keys = (uint16_t)(1u << ((value.row - 1u) * 4u + ki));
            value.recognized = true;
        } else if (ki == 7u) {
            value.keys = (uint16_t)(3u << ((value.row - 1u) * 4u));
            value.combination = true;
            value.recognized = true;
        }
    }
    *out = value;
    return BSP_OK;
}

void tm1650_deinit(Tm1650 *d)
{
    if (d) {
        memset(d, 0, sizeof *d);
    }
}
