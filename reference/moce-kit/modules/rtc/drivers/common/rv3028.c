#include "rv3028.h"
#include <string.h>

#define REG_STATUS 0x0eu
#define REG_CONTROL1 0x0fu
#define REG_CONTROL2 0x10u
#define REG_ID 0x28u
#define REG_PASSWORD 0x30u
#define REG_CLKOUT 0x35u
#define REG_BACKUP 0x37u
#define STATUS_BUSY 0x80u
#define CONTROL_EERD 0x08u

static BspStatus read_regs(const Rv3028 *d, uint8_t reg, uint8_t *data, size_t n)
{
    return bsp_i2c_mem_read(d->config.port, d->config.address, reg, 1u,
                            data, n, d->config.timeout_ms);
}

static BspStatus write_byte(const Rv3028 *d, uint8_t reg, uint8_t value)
{
    return bsp_i2c_mem_write(d->config.port, d->config.address, reg, 1u,
                             &value, 1u, d->config.timeout_ms);
}

static BspStatus update_byte(const Rv3028 *d, uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t old, actual;
    BspStatus s = read_regs(d, reg, &old, 1u);
    if (s != BSP_OK) {
        return s;
    }
    uint8_t value = (uint8_t)((old & (uint8_t)~mask) | bits);
    if (value != old) {
        s = write_byte(d, reg, value);
        if (s != BSP_OK) {
            return s;
        }
    }
    s = read_regs(d, reg, &actual, 1u);
    if (s != BSP_OK) {
        return s;
    }
    return (actual & mask) == bits ? BSP_OK : BSP_IO;
}

static BspStatus wait_refresh(const Rv3028 *d)
{
    /* Bounded 20 reads and 19 task delays, no busy spin or EEPROM command. */
    for (unsigned i = 0; i < 20u; ++i) {
        uint8_t status;
        BspStatus s = read_regs(d, REG_STATUS, &status, 1u);
        if (s != BSP_OK || !(status & STATUS_BUSY)) {
            return s;
        }
        if (i + 1u < 20u) {
            s = bsp_delay_ms(10u);
            if (s != BSP_OK) {
                return s;
            }
        }
    }
    return BSP_TIMEOUT;
}

static bool valid_time(const Rv3028DateTime *t)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (t->year < 2000u || t->year > 2099u || t->month < 1u || t->month > 12u ||
        t->weekday > 6u || t->hour > 23u || t->minute > 59u || t->second > 59u) {
        return false;
    }
    unsigned limit = days[t->month - 1u];
    if (t->month == 2u && t->year % 4u == 0u) {
        ++limit;
    }
    return t->day >= 1u && t->day <= limit;
}

static bool bcd_valid(uint8_t b)
{
    return (b & 15u) < 10u && (b >> 4) < 10u;
}

static uint8_t from_bcd(uint8_t b)
{
    return (uint8_t)((b >> 4) * 10u + (b & 15u));
}

static uint8_t to_bcd(unsigned v)
{
    return (uint8_t)(((v / 10u) << 4) | (v % 10u));
}

BspStatus rv3028_init(Rv3028 *d, const Rv3028Config *c)
{
    if (!d || !c || c->address != RV3028_ADDRESS || c->timeout_ms == 0u ||
        c->timeout_ms > 100u || (c->port >> 8) != BSP_I2C) {
        return BSP_INVALID_ARG;
    }
    if (d->initialized) {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(c->port, BSP_CAP_BLOCKING | BSP_CAP_RESTART, &missing);
    if (s != BSP_OK) {
        return s;
    }
    d->config = *c;
    s = wait_refresh(d);
    uint8_t protected = 0;
    if (s == BSP_OK) {
        s = read_regs(d, REG_PASSWORD, &protected, 1u);
    }
    if (s == BSP_OK && protected == 0xffu) {
        return BSP_UNSUPPORTED;
    }
    if (s == BSP_OK) {
        s = read_regs(d, REG_ID, &d->device_id, 1u);
    }
    if (s == BSP_OK) {
        s = update_byte(d, REG_CONTROL1, CONTROL_EERD, CONTROL_EERD);
    }
    if (s == BSP_OK) {
        s = wait_refresh(d);
    }
    /* Preserve factory offset bit7 and resistor bits1:0. Clear TCE/BSIE,
     * keep fast-edge detection and select LSM for this 3V3/CR1220 module. */
    if (s == BSP_OK) {
        s = update_byte(d, REG_BACKUP, 0x7cu, 0x1cu);
    }
    if (s == BSP_OK) {
        s = update_byte(d, REG_CLKOUT, 0x87u, 0x07u);
    }
    if (s == BSP_OK) {
        s = update_byte(d, REG_CONTROL2, 0xffu, 0u);
    }
    d->initialized = s == BSP_OK;
    return s;
}

BspStatus rv3028_read(Rv3028 *d, Rv3028Reading *out)
{
    if (!d || !out) {
        return BSP_INVALID_ARG;
    }
    if (!d->initialized) {
        return BSP_INVALID_STATE;
    }
    uint8_t raw[17];
    /* Calendar, status and mode in ONE transfer, coherently latched by device. */
    BspStatus s = read_regs(d, 0u, raw, sizeof(raw));
    if (s != BSP_OK) {
        return s; /* Caller output remains unchanged on I/O failure. */
    }
    Rv3028Reading r = {0};
    r.status = raw[REG_STATUS];
    r.time = (Rv3028DateTime){
        .year = (uint16_t)(2000u + from_bcd(raw[6])),
        .month = from_bcd(raw[5]), .day = from_bcd(raw[4]), .weekday = raw[3],
        .hour = from_bcd(raw[2]), .minute = from_bcd(raw[1]), .second = from_bcd(raw[0])
    };
    r.calendar_valid = !(raw[REG_CONTROL2] & 0x02u) && valid_time(&r.time);
    for (unsigned i = 0; i < 7u; ++i) {
        if (i != 3u && !bcd_valid(raw[i])) {
            r.calendar_valid = false;
        }
    }
    r.time_valid = r.calendar_valid && !(r.status & RV3028_STATUS_POR);
    *out = r;
    return BSP_OK;
}

BspStatus rv3028_set_datetime(Rv3028 *d, const Rv3028DateTime *t)
{
    if (!d || !t || !valid_time(t)) {
        return BSP_INVALID_ARG;
    }
    if (!d->initialized) {
        return BSP_INVALID_STATE;
    }
    uint8_t data[7] = {to_bcd(t->second), to_bcd(t->minute), to_bcd(t->hour),
                      t->weekday, to_bcd(t->day), to_bcd(t->month), to_bcd(t->year - 2000u)};
    BspStatus s = bsp_i2c_mem_write(d->config.port, d->config.address, 0u, 1u,
                                    data, sizeof(data), d->config.timeout_ms);
    if (s == BSP_OK) {
        /* Flags are cleared by writing zero. Ones preserve concurrent events. */
        s = write_byte(d, REG_STATUS, 0xfeu);
    }
    if (s == BSP_OK) {
        uint8_t status;
        s = read_regs(d, REG_STATUS, &status, 1u);
        if (s == BSP_OK && (status & RV3028_STATUS_POR)) {
            s = BSP_IO;
        }
    }
    if (s != BSP_OK) {
        d->initialized = false;
    }
    return s;
}

void rv3028_deinit(Rv3028 *d)
{
    if (d) {
        memset(d, 0, sizeof(*d));
    }
}
