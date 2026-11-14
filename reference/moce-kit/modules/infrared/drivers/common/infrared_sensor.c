#include "infrared_sensor.h"
BspStatus infrared_sensor_init(InfraredSensor *self, const InfraredSensorConfig *config)
{
    if (!self || !config || config->input == BSP_PORT_NONE ||
        (config->input >> 8) != BSP_GPIO) { return BSP_INVALID_ARG; }
    if (self->bound) { return BSP_INVALID_STATE; }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->input, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) { return status; }
    *self = (InfraredSensor){.config = *config, .bound = true};
    return BSP_OK;
}
BspStatus infrared_sensor_read(InfraredSensor *self, InfraredSensorSample *out)
{
    if (!self || !out) { return BSP_INVALID_ARG; }
    if (!self->bound) { return BSP_INVALID_STATE; }
    bool high = false;
    BspStatus status = bsp_gpio_read(self->config.input, &high);
    if (status != BSP_OK) { self->previous_valid = false; return status; }
    const bool detected = self->config.active_high ? high : !high;
    const InfraredSensorSample sample = {
        .raw_high = high, .detected = detected,
        .changed = self->previous_valid && detected != self->previous_detected,
        .sampled_ms = bsp_time_ms(),
    };
    self->previous_valid = true;
    self->previous_detected = detected;
    *out = sample;
    return BSP_OK;
}
BspStatus infrared_sensor_deinit(InfraredSensor *self)
{
    if (!self) { return BSP_INVALID_ARG; }
    if (!self->bound) { return BSP_INVALID_STATE; }
    *self = (InfraredSensor){0};
    return BSP_OK;
}
