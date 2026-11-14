/* Copyright (c) 2023 STMicroelectronics. All rights reserved.
 * MOCE adaptation 2026: public BSP IO, fail-fast errors, explicit lifecycle,
 * bounded polls, continuous-only timing, valid-range reporting.
 * Derived from ST VL53L4CD ULD 2.2.2. See LICENSE and UPSTREAM.md.
 */
#include "vl53l4cd.h"

static const uint8_t default_configuration[] = {
#include "vl53l4cd_defaults.inc"
};
_Static_assert(sizeof(default_configuration) == 91u, "ST configuration length");

static BspStatus rd(Vl53l4cd *d, uint16_t reg, uint8_t *data, size_t n)
{
    return bsp_i2c_mem_read(d->config.port, d->config.address, reg, 2, data, n,
                            d->config.io_timeout_ms);
}
static BspStatus wr(Vl53l4cd *d, uint16_t reg, const uint8_t *data, size_t n)
{
    return bsp_i2c_mem_write(d->config.port, d->config.address, reg, 2, data, n,
                             d->config.io_timeout_ms);
}
static BspStatus wr8(Vl53l4cd *d, uint16_t reg, uint8_t value)
{
    return wr(d, reg, &value, 1);
}
static BspStatus wr16(Vl53l4cd *d, uint16_t reg, uint16_t value)
{
    uint8_t bytes[] = {(uint8_t)(value >> 8), (uint8_t)value};
    return wr(d, reg, bytes, 2);
}
static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static BspStatus fault(Vl53l4cd *d, BspStatus status)
{
    d->last_error = status;
    d->state = VL53L4CD_FAULT;
    return status;
}
static BspStatus ready_raw(Vl53l4cd *d, bool *ready)
{
    uint8_t polarity = 0, value = 0;
    BspStatus s = rd(d, 0x0030, &polarity, 1);
    if (s != BSP_OK) return s;
    s = rd(d, 0x0031, &value, 1);
    if (s == BSP_OK) *ready = (value & 1u) == ((polarity & 0x10u) ? 0u : 1u);
    return s;
}
static BspStatus poll(Vl53l4cd *d, bool boot, uint32_t budget)
{
    uint32_t start = bsp_time_ms();
    /* Count bound also prevents a stalled BSP timebase causing an infinite loop. */
    for (uint32_t i = 0; i <= budget; ++i)
    {
        bool ready = false;
        BspStatus s;
        if (boot)
        {
            uint8_t value = 0;
            s = rd(d, 0x00e5, &value, 1);
            ready = value == 3u;
        }
        else s = ready_raw(d, &ready);
        if (s != BSP_OK) return s;
        if (ready) return BSP_OK;
        if (i == budget || (uint32_t)(bsp_time_ms() - start) >= budget) return BSP_TIMEOUT;
        s = bsp_delay_ms(1);
        if (s != BSP_OK) return s;
    }
    return BSP_TIMEOUT;
}
static BspStatus timing(Vl53l4cd *d)
{
    uint8_t bytes[2];
    BspStatus s = rd(d, 0x0006, bytes, 2);
    if (s != BSP_OK) return s;
    uint16_t osc = be16(bytes);
    if (!osc) return BSP_IO;
    uint32_t macro = (uint32_t)((uint64_t)2304u * (0x40000000u / osc) >> 6);
    uint32_t budget = (d->config.timing_budget_ms * 1000u - 2500u) << 12;
    const uint8_t zero[4] = {0};
    s = wr(d, 0x006c, zero, sizeof(zero));
    if (s != BSP_OK) return s;
    const uint32_t factors[] = {16, 12};
    const uint16_t regs[] = {0x005e, 0x0061};
    for (unsigned i = 0; i < 2; ++i)
    {
        uint64_t denominator = ((uint64_t)macro * factors[i]) >> 6;
        if (!denominator) return BSP_IO;
        uint64_t ticks = ((uint64_t)budget + denominator / 2) / denominator;
        if (!ticks) return BSP_IO;
        --ticks;
        uint16_t exponent = 0;
        while (ticks > 255u) { ticks >>= 1; ++exponent; }
        s = wr16(d, regs[i], (uint16_t)((exponent << 8) | (uint16_t)ticks));
        if (s != BSP_OK) return s;
    }
    return BSP_OK;
}
static BspStatus initialize(Vl53l4cd *d)
{
    BspStatus s = poll(d, true, d->config.boot_timeout_ms);
    if (s != BSP_OK) return s;
    for (size_t i = 0; i < sizeof(default_configuration); ++i)
    {
        s = wr8(d, (uint16_t)(0x2du + i), default_configuration[i]);
        if (s != BSP_OK) return s;
    }
    s = wr8(d, 0x0087, 0x40); /* Initial VHV calibration. */
    if (s != BSP_OK) return s;
    s = poll(d, false, d->config.boot_timeout_ms);
    if (s != BSP_OK) return s;
    s = wr8(d, 0x0086, 1);
    if (s != BSP_OK) return s;
    s = wr8(d, 0x0087, 0x80);
    if (s != BSP_OK) return s;
    s = wr8(d, 0x0008, 9);
    if (s != BSP_OK) return s;
    s = wr8(d, 0x000b, 0);
    if (s != BSP_OK) return s;
    s = wr16(d, 0x0024, 0x0500);
    if (s != BSP_OK) return s;
    return timing(d);
}
BspStatus vl53l4cd_init(Vl53l4cd *d, const Vl53l4cdConfig *c)
{
    if (!d || !c || c->address < 8 || c->address > 0x77 || !c->io_timeout_ms ||
        c->io_timeout_ms > INT32_MAX || !c->boot_timeout_ms || c->boot_timeout_ms > 10000 ||
        c->timing_budget_ms < 10 || c->timing_budget_ms > 200) return BSP_INVALID_ARG;
    if (d->state != VL53L4CD_UNINITIALIZED) return BSP_INVALID_STATE;
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(c->port, BSP_CAP_BLOCKING | BSP_CAP_RESTART, &missing);
    if (s != BSP_OK) return s;
    Vl53l4cd next = {.config = *c};
    uint8_t id[2];
    s = rd(&next, 0x010f, id, 2);
    if (s != BSP_OK) return s;
    if (be16(id) != VL53L4CD_MODEL_ID) return BSP_UNSUPPORTED; /* No writes to wrong model. */
    *d = next;
    s = initialize(d);
    if (s != BSP_OK)
    {
        d->cleanup_error = wr8(d, 0x0087, 0x80); /* Best-effort stop, retain primary error. */
        return fault(d, s);
    }
    d->state = VL53L4CD_IDLE;
    return BSP_OK;
}
BspStatus vl53l4cd_start(Vl53l4cd *d)
{
    if (!d) return BSP_INVALID_ARG;
    if (d->state != VL53L4CD_IDLE) return BSP_INVALID_STATE;
    BspStatus s = wr8(d, 0x0086, 1);
    if (s == BSP_OK) s = wr8(d, 0x0087, 0x21);
    if (s != BSP_OK) return fault(d, s);
    d->state = VL53L4CD_RANGING;
    return BSP_OK;
}
BspStatus vl53l4cd_stop(Vl53l4cd *d)
{
    if (!d) return BSP_INVALID_ARG;
    if (d->state == VL53L4CD_UNINITIALIZED) return BSP_INVALID_STATE;
    if (d->state == VL53L4CD_IDLE) return BSP_OK;
    bool was_fault = d->state == VL53L4CD_FAULT;
    BspStatus s = wr8(d, 0x0087, 0x80);
    if (was_fault) { d->cleanup_error = s; return s; }
    if (s != BSP_OK) return fault(d, s);
    d->state = VL53L4CD_IDLE;
    return BSP_OK;
}
BspStatus vl53l4cd_data_ready(Vl53l4cd *d, bool *ready)
{
    if (!d || !ready) return BSP_INVALID_ARG;
    if (d->state != VL53L4CD_RANGING) return BSP_INVALID_STATE;
    BspStatus s = ready_raw(d, ready);
    return s == BSP_OK ? s : fault(d, s);
}
BspStatus vl53l4cd_read(Vl53l4cd *d, Vl53l4cdReading *out)
{
    if (!d || !out) return BSP_INVALID_ARG;
    bool ready = false;
    BspStatus s = vl53l4cd_data_ready(d, &ready);
    if (s != BSP_OK) return s;
    if (!ready) return BSP_NOT_READY;
    uint8_t bytes[15];
    s = rd(d, 0x0089, bytes, sizeof(bytes));
    if (s != BSP_OK) return fault(d, s);
    static const uint8_t mapping[24] = {255,255,255,5,2,4,1,7,3,0,255,255,9,13,255,255,255,255,10,6,255,255,11,12};
    Vl53l4cdReading result = {0};
    result.raw_status = bytes[0] & 0x1fu;
    result.range_status = result.raw_status < 24 ? mapping[result.raw_status] : 255;
    result.distance_mm = be16(&bytes[13]);
    result.spad_count = be16(&bytes[3]) / 256u;
    result.signal_rate_kcps = (uint32_t)be16(&bytes[5]) * 8u;
    result.ambient_rate_kcps = (uint32_t)be16(&bytes[7]) * 8u;
    result.sigma_quarter_mm = be16(&bytes[9]);
    result.valid = result.range_status == 0 && result.spad_count != 0;
    s = wr8(d, 0x0086, 1); /* Acknowledge only after complete result fetch. */
    if (s != BSP_OK) return fault(d, s);
    *out = result;
    return BSP_OK;
}
BspStatus vl53l4cd_wait_read(Vl53l4cd *d, uint32_t wait_ms, Vl53l4cdReading *out)
{
    if (!d || !out || wait_ms > INT32_MAX) return BSP_INVALID_ARG;
    uint32_t start = bsp_time_ms();
    for (uint32_t i = 0; i <= wait_ms; ++i)
    {
        BspStatus s = vl53l4cd_read(d, out);
        if (s != BSP_NOT_READY) return s;
        if (!wait_ms) return BSP_NOT_READY;
        if (i == wait_ms || (uint32_t)(bsp_time_ms() - start) >= wait_ms) return BSP_TIMEOUT;
        s = bsp_delay_ms(1);
        if (s != BSP_OK) return fault(d, s);
    }
    return BSP_TIMEOUT;
}
