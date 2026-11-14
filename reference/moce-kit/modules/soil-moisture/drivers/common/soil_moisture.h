#ifndef SOIL_MOISTURE_H
#define SOIL_MOISTURE_H
#include "bsp_adc.h"
#ifdef __cplusplus
extern "C" {
#endif
/* TLC555 AO module: no digital identity, temperature or absolute water-content
 * measurement. One serialized task/main owns the zero-initialized instance and
 * its exclusively initialized ADC channel. No ISR, concurrent API calls or heap.
 * Driver never initializes/deinitializes BSP ports. Calibration is RAM-only,
 * measured on this module/soil/ADC configuration, in the selected ADC unit. */
typedef struct {
    BspPort port;
    size_t channel;
    BspAdcReadOptions adc;
    uint8_t samples;             /* 1..64; integer arithmetic mean */
    uint32_t sample_gap_ms;       /* 0..1000 between readings, not a sample rate */
    bool calibrated;
    int64_t dry, wet;            /* distinct measured endpoints; either polarity */
} SoilMoistureConfig;
typedef struct {
    BspAdcFormat format;
    uint8_t resolution_bits;
    bool raw_signed;
    int64_t mean, minimum, maximum;
    uint8_t samples;
    bool moisture_valid, clipped;
    uint16_t moisture_bp;        /* relative 0..10000 = 0.00..100.00%, not VWC */
} SoilMoistureSample;
typedef struct {
    bool initialized, last_valid;
    BspStatus last_error;
    uint32_t successful_reads;
} SoilMoistureState;
typedef struct {
    SoilMoistureConfig config;
    BspAdcCapabilities capabilities;
    SoilMoistureState state;
} SoilMoisture;
/* Non-OK init leaves instance unchanged; already initialized -> INVALID_STATE. */
BspStatus soil_moisture_init(SoilMoisture *device, const SoilMoistureConfig *config);
/* Non-OK read leaves *out unchanged, sets last_valid=false and last_error.
 * Whole batch discarded on first error; no automatic retry or retained pointer.
 * Mean truncates toward zero. ADC SDK_NATIVE may wait without a finite bound.
 * Finite timeout covers each ADC conversion wait only, not the whole batch. */
BspStatus soil_moisture_read(SoilMoisture *device, SoilMoistureSample *out);
/* Validates both endpoints in the active ADC domain; no persistent writes. */
BspStatus soil_moisture_calibrate(SoilMoisture *device, int64_t dry, int64_t wet);
BspStatus soil_moisture_clear_calibration(SoilMoisture *device);
BspStatus soil_moisture_query(const SoilMoisture *device, SoilMoistureState *out);
BspStatus soil_moisture_deinit(SoilMoisture *device);
#ifdef __cplusplus
}
#endif
#endif
