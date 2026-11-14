#ifndef MOCE_INFRARED_SENSOR_H
#define MOCE_INFRARED_SENSOR_H
#include "bsp_gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Digital reflection module (ITR9909 + LM393LVDR), NOT a distance sensor.
 * Caller initializes/owns an input GPIO with BSP_CAP_BLOCKING, zero-initializes
 * the instance and serializes calls. No allocation, retries or pin configuration.
 * Config is copied; active_high is an application choice to verify on hardware.
 * GPIO errors leave the output unchanged and break transition continuity.
 * deinit releases only this binding, never the caller-owned BSP resource.
 */
typedef struct { BspPort input; bool active_high; } InfraredSensorConfig;
typedef struct {
    bool raw_high, detected, changed;
    uint32_t sampled_ms;
} InfraredSensorSample;
typedef struct {
    InfraredSensorConfig config;
    bool bound, previous_valid, previous_detected;
} InfraredSensor;
BspStatus infrared_sensor_init(InfraredSensor *self, const InfraredSensorConfig *config);
BspStatus infrared_sensor_read(InfraredSensor *self, InfraredSensorSample *out);
BspStatus infrared_sensor_deinit(InfraredSensor *self);
#ifdef __cplusplus
}
#endif
#endif
