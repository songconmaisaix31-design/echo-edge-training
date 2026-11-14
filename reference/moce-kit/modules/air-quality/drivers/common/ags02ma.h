#ifndef MOCE_AGS02MA_H
#define MOCE_AGS02MA_H
#include "bsp_i2c.h"
#include <stdbool.h>
#include <stdint.h>

#define AGS02MA_DEFAULT_ADDRESS 0x1au
#define AGS02MA_MAX_I2C_HZ 30000u
#define AGS02MA_PREHEAT_MS 120000u
#define AGS02MA_MEASURE_WAIT_MS 30u

/* Zero-initialize instance; single task, exclusive sensor ownership.
 * Caller initializes BSP I2C at config.i2c_hz, waits >=120s after power-up,
 * and spaces measurement starts >=2s. Default power-up ppb mode required.
 * Driver does not change address, unit or calibration; errors preserve outputs.
 * Per-transfer timeout is not a whole-operation deadline. No automatic retries.
 * Version and TVOC use STOP + 30ms + read; measurement starts stay >=2s apart.
 * Diagnostic frame is untrusted until CRC passes. No peripheral deinit ownership.
 */
typedef enum { AGS02MA_OK, AGS02MA_INVALID_ARG, AGS02MA_BSP_ERROR,
    AGS02MA_CRC_ERROR, AGS02MA_NOT_READY, AGS02MA_UNSUPPORTED_DATA } Ags02maStatus;
typedef struct { BspPort port; uint8_t address; uint32_t i2c_hz, timeout_ms; } Ags02maConfig;
typedef struct { uint8_t status, frame[5], calculated_crc, reg; bool frame_received; } Ags02maDiagnostic;
typedef struct { Ags02maConfig config; BspStatus last_bsp_status; Ags02maDiagnostic diagnostic; bool initialized; } Ags02ma;
typedef struct { uint8_t status; uint32_t tvoc_ppb; } Ags02maSample;

Ags02maStatus ags02ma_init(Ags02ma *device, const Ags02maConfig *config);
Ags02maStatus ags02ma_read_version(Ags02ma *device, uint8_t *version);
Ags02maStatus ags02ma_read_tvoc(Ags02ma *device, Ags02maSample *sample);
#endif

