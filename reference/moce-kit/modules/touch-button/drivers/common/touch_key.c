#include "touch_key.h"
BspStatus touch_key_init(TouchKey *self, const TouchKeyConfig *config)
{
    if (!self || !config || (config->input >> 8) != BSP_GPIO) {
        return BSP_INVALID_ARG;
    }
    if (self->bound) {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->input, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) {
        return status;
    }
    *self = (TouchKey){.config = *config, .bound = true};
    return BSP_OK;
}
BspStatus touch_key_read(TouchKey *self, TouchKeySample *out)
{
    if (!self || !out) {
        return BSP_INVALID_ARG;
    }
    if (!self->bound) {
        return BSP_INVALID_STATE;
    }
    bool high = false;
    BspStatus status = bsp_gpio_read(self->config.input, &high);
    if (status != BSP_OK) {
        self->previous_valid = false;
        return status;
    }
    const bool touched = self->config.active_high ? high : !high;
    const TouchKeySample sample = {
        .raw_high = high, .touched = touched,
        .changed = self->previous_valid && touched != self->previous_touched,
        .sampled_ms = bsp_time_ms(),
    };
    self->previous_valid = true;
    self->previous_touched = touched;
    *out = sample;
    return BSP_OK;
}
BspStatus touch_key_deinit(TouchKey *self)
{
    if (!self) {
        return BSP_INVALID_ARG;
    }
    if (!self->bound) {
        return BSP_INVALID_STATE;
    }
    *self = (TouchKey){0};
    return BSP_OK;
}
