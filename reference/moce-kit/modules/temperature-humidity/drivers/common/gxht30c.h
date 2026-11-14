#ifndef GXHT30C_H
#define GXHT30C_H
#include "bsp_i2c.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GXHT30C_ADDRESS_LOW 0x44u
#define GXHT30C_ADDRESS_HIGH 0x45u
/* 3.3 V module; task/main context, one caller per instance/device.
 * Caller owns power and initialized blocking I2C port. Zero-init instances.
 * No heap, hidden task, bus scanning, general-call reset or GPIO access.
 * timeout_ms applies to each BSP transfer, not an end-to-end deadline. */
typedef struct {
    BspPort port;
    uint8_t address;             /* Unshifted 7-bit address: 0x44 or 0x45. */
    uint32_t timeout_ms;         /* 1..INT32_MAX. */
} Gxht30cConfig;
typedef enum {
    GXHT30C_ERROR_NONE,
    GXHT30C_ERROR_ARGUMENT,
    GXHT30C_ERROR_STATE,
    GXHT30C_ERROR_CAPABILITY,
    GXHT30C_ERROR_DELAY,
    GXHT30C_ERROR_WRITE,
    GXHT30C_ERROR_READ,
    GXHT30C_ERROR_TEMPERATURE_CRC,
    GXHT30C_ERROR_HUMIDITY_CRC
} Gxht30cError;
typedef struct {
    bool valid;
    uint16_t raw_temperature, raw_humidity;
    int32_t temperature_milli_c;
    uint32_t humidity_milli_percent;
    uint32_t sampled_at_ms;
} Gxht30cReading;
typedef struct {
    Gxht30cConfig config;
    bool initialized;
    Gxht30cError last_error;
} Gxht30c;
/* Wait for startup, stop previous periodic mode, then soft-reset this address.
 * Does not configure/release the I2C port. Failure leaves instance uninitialized. */
BspStatus gxht30c_init(Gxht30c *sensor, const Gxht30cConfig *config);
/* High repeatability, no clock stretching (0x2400), 20 ms conversion wait.
 * Clear output on every failure. CRC errors return BSP_IO with last_error;
 * a new measurement is permitted after CRC errors. Transport/delay failure
 * invalidates initialization; caller must resolve port fault before re-init. */
BspStatus gxht30c_measure(Gxht30c *sensor, Gxht30cReading *out);
/* Software teardown only; caller must release its port separately. */
void gxht30c_deinit(Gxht30c *sensor);
#ifdef __cplusplus
}
#endif
#endif

