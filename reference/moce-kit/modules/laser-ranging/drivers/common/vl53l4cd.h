#ifndef MOCE_VL53L4CD_H
#define MOCE_VL53L4CD_H
#include "bsp_i2c.h"
#ifdef __cplusplus
extern "C" {
#endif
#define VL53L4CD_DEFAULT_ADDRESS 0x29u /* 7-bit; ST 0x52 is shifted notation. */
#define VL53L4CD_MODEL_ID 0xEBAAu
typedef struct
{
    BspPort port;
    uint8_t address;
    uint32_t io_timeout_ms; /* 1..INT32_MAX, each BSP transaction. */
    uint32_t boot_timeout_ms; /* 1..10000; polling budget, not whole-call deadline. */
    uint32_t timing_budget_ms; /* 10..200, continuous ranging only. */
} Vl53l4cdConfig;
typedef enum { VL53L4CD_UNINITIALIZED, VL53L4CD_IDLE, VL53L4CD_RANGING,
               VL53L4CD_FAULT } Vl53l4cdState;
typedef struct
{
    Vl53l4cdConfig config;
    Vl53l4cdState state;
    BspStatus last_error;
    BspStatus cleanup_error;
} Vl53l4cd;
typedef struct
{
    uint16_t distance_mm;
    uint8_t range_status; /* ST ULD mapping: only 0 means valid distance. */
    uint8_t raw_status;
    bool valid;
    uint16_t spad_count;
    uint32_t signal_rate_kcps;
    uint32_t ambient_rate_kcps;
    uint16_t sigma_quarter_mm;
} Vl53l4cdReading;
/* Caller owns initialized synchronous I2C bus, hardware reset, power, scheduling.
 * No Board/SDK dependency. Zero-initialize instance; do not modify active instance.
 * Init checks ID before writes; successful init leaves sensor stopped. Config copied.
 * FAULT requires sensor power-cycle/XSHUT reset, then zero instance and re-init.
 * Task/main only. Serialize all calls and shared-bus lifetime; no internal locks.
 * Errors propagate, no retries. Mutating/reading bus errors latch FAULT; stop may
 * still be attempted in FAULT, but never clears the need for hardware reset.
 * Output is unchanged on error/NOT_READY. BSP_OK read may contain invalid range
 * status; use distance ONLY when reading.valid is true.
 */
BspStatus vl53l4cd_init(Vl53l4cd *device, const Vl53l4cdConfig *config);
BspStatus vl53l4cd_start(Vl53l4cd *device);
BspStatus vl53l4cd_stop(Vl53l4cd *device);
BspStatus vl53l4cd_data_ready(Vl53l4cd *device, bool *ready);
BspStatus vl53l4cd_read(Vl53l4cd *device, Vl53l4cdReading *out);
/* Poll once for wait_ms=0; otherwise finite poll budget <=INT32_MAX.
 * A timeout leaves RANGING active: caller may read later or stop explicitly. */
BspStatus vl53l4cd_wait_read(Vl53l4cd *device, uint32_t wait_ms, Vl53l4cdReading *out);
#ifdef __cplusplus
}
#endif
#endif
