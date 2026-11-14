#include "gxht30c.h"
#include <limits.h>
#include <string.h>

static BspStatus fail(Gxht30c *sensor, Gxht30cError error, BspStatus status)
{
    sensor->last_error = error;
    if (error != GXHT30C_ERROR_TEMPERATURE_CRC && error != GXHT30C_ERROR_HUMIDITY_CRC) {
        sensor->initialized = false;
    }
    return status;
}
static BspStatus wait_ms(Gxht30c *sensor, uint32_t milliseconds)
{
    BspStatus status = bsp_delay_ms(milliseconds);
    return status == BSP_OK ? BSP_OK : fail(sensor, GXHT30C_ERROR_DELAY, status);
}
static BspStatus command(Gxht30c *sensor, uint16_t value)
{
    const uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    BspStatus status = bsp_i2c_write(sensor->config.port, sensor->config.address,
                                   bytes, sizeof(bytes), sensor->config.timeout_ms);
    return status == BSP_OK ? BSP_OK : fail(sensor, GXHT30C_ERROR_WRITE, status);
}
static uint8_t crc8(const uint8_t *data)
{
    uint8_t crc = 0xffu;
    for (unsigned i = 0; i < 2u; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (uint8_t)((crc & 0x80u) ? ((unsigned)crc << 1) ^ 0x31u :
                                                        (unsigned)crc << 1);
        }
    }
    return crc;
}
BspStatus gxht30c_init(Gxht30c *sensor, const Gxht30cConfig *config)
{
    if (!sensor || !config) {
        return BSP_INVALID_ARG;
    }
    /* Reject duplicate init before mutating the live instance. */
    if (sensor->initialized) {
        return BSP_INVALID_STATE;
    }
    if ((config->port >> 8) != BSP_I2C ||
        (config->address != GXHT30C_ADDRESS_LOW && config->address != GXHT30C_ADDRESS_HIGH) ||
        config->timeout_ms == 0 || config->timeout_ms > INT32_MAX) {
        sensor->last_error = GXHT30C_ERROR_ARGUMENT;
        return BSP_INVALID_ARG;
    }
    Gxht30cConfig copy = *config;
    memset(sensor, 0, sizeof(*sensor));
    sensor->config = copy;
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(copy.port, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) {
        return fail(sensor, GXHT30C_ERROR_CAPABILITY, status);
    }
    /* Datasheet V3.7 tables 4/12/13: startup <=1 ms at 3.3 V;
     * break may finish the current conversion (<=15.5 ms) then switch (1 ms).
     * Soft reset requires idle mode and completes within 1.5 ms. */
    if ((status = wait_ms(sensor, 2)) != BSP_OK ||
        (status = command(sensor, 0x3093u)) != BSP_OK ||
        (status = wait_ms(sensor, 20)) != BSP_OK ||
        (status = command(sensor, 0x30a2u)) != BSP_OK ||
        (status = wait_ms(sensor, 2)) != BSP_OK) {
        return status;
    }
    sensor->initialized = true;
    return BSP_OK;
}
BspStatus gxht30c_measure(Gxht30c *sensor, Gxht30cReading *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!sensor || !out) {
        return BSP_INVALID_ARG;
    }
    if (!sensor->initialized) {
        sensor->last_error = GXHT30C_ERROR_STATE;
        return BSP_INVALID_STATE;
    }
    sensor->last_error = GXHT30C_ERROR_NONE;
    BspStatus status;
    /* Enforce >=1 ms between commands even for immediate consecutive callers. */
    if ((status = wait_ms(sensor, 2)) != BSP_OK ||
        (status = command(sensor, 0x2400u)) != BSP_OK ||
        (status = wait_ms(sensor, 20)) != BSP_OK) {
        return status;
    }
    uint8_t bytes[6] = {0};
    status = bsp_i2c_read(sensor->config.port, sensor->config.address,
                          bytes, sizeof(bytes), sensor->config.timeout_ms);
    if (status != BSP_OK) {
        return fail(sensor, GXHT30C_ERROR_READ, status);
    }
    if (crc8(bytes) != bytes[2]) {
        return fail(sensor, GXHT30C_ERROR_TEMPERATURE_CRC, BSP_IO);
    }
    if (crc8(bytes + 3) != bytes[5]) {
        return fail(sensor, GXHT30C_ERROR_HUMIDITY_CRC, BSP_IO);
    }
    Gxht30cReading reading = {0};
    reading.raw_temperature = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    reading.raw_humidity = (uint16_t)(((uint16_t)bytes[3] << 8) | bytes[4]);
    /* T=-45+175*ST/65535; RH=100*SRH/65535. Use 64-bit intermediate
     * and nearest 0.001-unit rounding; resolution is not sensor accuracy. */
    reading.temperature_milli_c = -45000 +
        (int32_t)(((uint64_t)175000u * reading.raw_temperature + 32767u) / 65535u);
    reading.humidity_milli_percent =
        (uint32_t)(((uint64_t)100000u * reading.raw_humidity + 32767u) / 65535u);
    reading.sampled_at_ms = bsp_time_ms();
    reading.valid = true;
    *out = reading;
    return BSP_OK;
}
void gxht30c_deinit(Gxht30c *sensor)
{
    if (sensor) {
        memset(sensor, 0, sizeof(*sensor));
    }
}

