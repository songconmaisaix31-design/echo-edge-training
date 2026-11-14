#include "ags02ma.h"
#include "bsp_core.h"
#include <limits.h>
#include <string.h>
static uint8_t crc8(const uint8_t *data)
{
    uint8_t crc = 0xffu;
    for (unsigned i = 0; i < 4u; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (uint8_t)((crc & 0x80u) ? ((unsigned)crc << 1) ^ 0x31u : (unsigned)crc << 1);
        }
    }
    return crc;
}
static Ags02maStatus read_frame(Ags02ma *d, uint8_t reg, uint32_t wait_ms, uint8_t frame[5])
{
    d->diagnostic = (Ags02maDiagnostic){.reg = reg};
    d->last_bsp_status = bsp_i2c_write(d->config.port, d->config.address, &reg, 1u, d->config.timeout_ms);
    if (d->last_bsp_status != BSP_OK) { return AGS02MA_BSP_ERROR; }
    d->last_bsp_status = bsp_delay_ms(wait_ms);
    if (d->last_bsp_status != BSP_OK) { return AGS02MA_BSP_ERROR; }
    d->last_bsp_status = bsp_i2c_read(d->config.port, d->config.address, frame, 5u, d->config.timeout_ms);
    if (d->last_bsp_status != BSP_OK) { return AGS02MA_BSP_ERROR; }
    memcpy(d->diagnostic.frame, frame, 5u);
    d->diagnostic.calculated_crc = crc8(frame);
    d->diagnostic.frame_received = true;
    d->diagnostic.status = frame[0];
    return d->diagnostic.calculated_crc == frame[4] ? AGS02MA_OK : AGS02MA_CRC_ERROR;
}
Ags02maStatus ags02ma_init(Ags02ma *d, const Ags02maConfig *c)
{
    if (!d || !c || (c->port >> 8) != BSP_I2C || c->address == 0u || c->address > 0x7fu ||
        c->i2c_hz == 0u || c->i2c_hz > AGS02MA_MAX_I2C_HZ || c->timeout_ms == 0u || c->timeout_ms == UINT32_MAX) {
        return AGS02MA_INVALID_ARG;
    }
    if (d->initialized) { return AGS02MA_INVALID_ARG; }
    uint32_t missing = 0u;
    d->last_bsp_status = bsp_port_require(c->port, BSP_CAP_BLOCKING, &missing);
    if (d->last_bsp_status != BSP_OK) { return AGS02MA_BSP_ERROR; }
    d->config = *c;
    d->diagnostic = (Ags02maDiagnostic){0};
    d->initialized = true;
    return AGS02MA_OK;
}
Ags02maStatus ags02ma_read_version(Ags02ma *d, uint8_t *version)
{
    if (!d || !d->initialized || !version) { return AGS02MA_INVALID_ARG; }
    uint8_t frame[5]; Ags02maStatus s = read_frame(d, 0x11u, 30u, frame);
    if (s == AGS02MA_OK) { *version = frame[3]; }
    return s;
}
Ags02maStatus ags02ma_read_tvoc(Ags02ma *d, Ags02maSample *sample)
{
    if (!d || !d->initialized || !sample) { return AGS02MA_INVALID_ARG; }
    uint8_t frame[5]; Ags02maStatus s = read_frame(d, 0x00u, AGS02MA_MEASURE_WAIT_MS, frame);
    if (s != AGS02MA_OK) { return s; }
    if ((frame[0] & 1u) != 0u) { return AGS02MA_NOT_READY; }
    if ((frame[0] & 0xfeu) != 0u) { return AGS02MA_UNSUPPORTED_DATA; }
    Ags02maSample result = {.status = frame[0], .tvoc_ppb = ((uint32_t)frame[1] << 16) | ((uint32_t)frame[2] << 8) | frame[3]};
    *sample = result;
    return AGS02MA_OK;
}
