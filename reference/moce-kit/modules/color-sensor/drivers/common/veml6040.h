#ifndef MOCE_VEML6040_H
#define MOCE_VEML6040_H
#include "bsp_i2c.h"
#ifdef __cplusplus
extern "C" {
#endif
#define VEML6040_DRIVER_VERSION "1.1.0"
#define VEML6040_ADDRESS 0x10u
#define VEML6040_REG_CONF 0x00u
#define VEML6040_REG_RED 0x08u
#define VEML6040_REG_GREEN 0x09u
#define VEML6040_REG_BLUE 0x0au
#define VEML6040_REG_WHITE 0x0bu

typedef enum {
    VEML6040_IT_40_MS = 0, VEML6040_IT_80_MS, VEML6040_IT_160_MS,
    VEML6040_IT_320_MS, VEML6040_IT_640_MS, VEML6040_IT_1280_MS
} Veml6040Integration;
typedef enum { VEML6040_MODE_FORCE = 0, VEML6040_MODE_AUTO = 1 } Veml6040Mode;
typedef enum {
    VEML6040_CHANNEL_RED = 0, VEML6040_CHANNEL_GREEN,
    VEML6040_CHANNEL_BLUE, VEML6040_CHANNEL_WHITE
} Veml6040Channel;

/* Existing three-field initializer remains supported. init defaults to FORCE,
 * enabled. Bus must already be initialized, blocking + repeated START capable.
 * Caller zero-initializes device and serializes access (including shared bus).
 * Task context only; no allocation. timeout_ms is per transfer, 1..INT32_MAX.
 * Configuration copied; caller may release config after init. */
typedef struct {
    BspPort port;
    uint32_t timeout_ms;
    uint8_t integration;
} Veml6040Config;
typedef struct {
    uint8_t integration; /* Veml6040Integration: 0..5 */
    Veml6040Mode mode;
    bool shutdown;
} Veml6040Settings;
typedef struct {
    Veml6040Settings settings;
    bool trigger; /* Raw TRIG bit; NOT a documented data-ready/busy indication. */
    uint16_t raw;
} Veml6040ConfigReadback;
typedef struct {
    Veml6040Config config;
    bool configured, initialized;
    Veml6040Settings settings; /* Private cached state; do not edit directly. */
} Veml6040;
typedef struct { uint16_t r, g, b, w; } Veml6040Sample;

/* On configuration/trigger write failure hardware state is uncertain:
 * initialized becomes false. After explicit application bus recovery if needed,
 * configure may restore a known state; deinit may always attempt shutdown.
 * Driver never resets/recovers/releases the shared bus automatically.
 * Failed init after write attempt retains configured=true for cleanup.
 * Init is NOT chip identification: no chip-ID register is specified. */
BspStatus veml6040_init(Veml6040 *dev, const Veml6040Config *config);
BspStatus veml6040_configure(Veml6040 *dev, const Veml6040Settings *settings);
BspStatus veml6040_set_integration(Veml6040 *dev, Veml6040Integration integration);
BspStatus veml6040_set_mode(Veml6040 *dev, Veml6040Mode mode);
BspStatus veml6040_set_shutdown(Veml6040 *dev, bool shutdown);
BspStatus veml6040_sleep(Veml6040 *dev);
BspStatus veml6040_wake(Veml6040 *dev);
/* Reads actual CONF, validates reserved bits/IT. Does not silently synchronize
 * the cache; configure is the explicit way to restore known state. */
BspStatus veml6040_read_config(Veml6040 *dev, Veml6040ConfigReadback *out);
/* FORCE + enabled only. Writes TRIG=1 without waiting. Caller must finish the
 * conversion before another trigger/config change. No asynchronous BSP transfer. */
BspStatus veml6040_trigger(Veml6040 *dev);
/* Conservative software wait: 2 * IT + 10ms; no hardware-ready bit exists.
 * Valid only while enabled. Caller must have triggered FORCE mode first. */
BspStatus veml6040_wait_conversion(Veml6040 *dev);
/* Reads retained registers without triggering/waiting; allowed during shutdown.
 * May return OLD data if conversion is incomplete. Four transactions in AUTO
 * mode can span an update boundary; use FORCE + measure for coherent RGBW.
 * Outputs unchanged on any error. No undocumented register burst assumption. */
BspStatus veml6040_read_channel(Veml6040 *dev, Veml6040Channel channel, uint16_t *out);
BspStatus veml6040_read_rgbw(Veml6040 *dev, Veml6040Sample *out);
/* FORCE: trigger + wait + RGBW. AUTO: wait a full guard interval + RGBW.
 * Shutdown rejected, mode not changed. Successful read is not a hardware
 * data-ready assertion. No other actor may change CONF during operation. */
BspStatus veml6040_measure(Veml6040 *dev, Veml6040Sample *out);
BspStatus veml6040_deinit(Veml6040 *dev);

/* Pure helpers: output unchanged on error; no bus access. */
BspStatus veml6040_integration_ms(Veml6040Integration integration, uint32_t *out);
BspStatus veml6040_conversion_wait_ms(Veml6040Integration integration, uint32_t *out);
BspStatus veml6040_green_sensitivity(Veml6040Integration integration, float *lux_per_count);
BspStatus veml6040_full_scale_lux(Veml6040Integration integration, float *out);
/* Nominal G sensitivity, no cover-glass correction. G==65535 is clipped:
 * returns BSP_NOT_READY rather than a falsely precise lux result. */
BspStatus veml6040_lux(uint16_t green, Veml6040Integration integration, float *out);
/* Caller-selected clipping threshold, e.g. 65000; this is not a hardware flag. */
BspStatus veml6040_saturation_mask(const Veml6040Sample *sample, uint16_t threshold,
                                 uint8_t *mask); /* bits0..3 = R,G,B,W */
#define VEML6040_CCT_OPEN_AIR_OFFSET 0.5f
/* CCT math helper; link the math library (GCC: -lm).
 * Vishay application note 84331 (06-Mar-2025), simplified CCT estimate.
 * Raw RGB required, NOT white-balanced/display RGB. offset is optical calibration.
 * For broad-spectrum near-white illumination; no validity guarantee for arbitrary
 * colored objects/monochromatic LEDs. Not a measured temperature register.
 * NULL/nonfinite offset -> INVALID_ARG; G=0, clipped RGB, index<=0 or
 * nonfinite result -> NOT_READY. Output unchanged on all failures.
 * W is unused. Numeric validity alone does NOT imply physical CCT validity. */
BspStatus veml6040_cct_estimate(const Veml6040Sample *sample, float offset, float *kelvin);

#ifdef __cplusplus
}
#endif
#endif
