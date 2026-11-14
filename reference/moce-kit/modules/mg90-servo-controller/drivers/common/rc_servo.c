#include "rc_servo.h"
BspStatus rc_servo_init(RcServo *device, const RcServoConfig *config)
{
    if (!device || !config || (config->pwm_port >> 8) != BSP_PWM ||
        !config->frame_us || !config->min_pulse_us ||
        config->min_pulse_us > config->max_pulse_us ||
        config->max_pulse_us >= config->frame_us) { return BSP_INVALID_ARG; }
    if (device->initialized || device->faulted) { return BSP_INVALID_STATE; }
    uint32_t missing = 0;
    BspStatus result = bsp_port_require(config->pwm_port, BSP_CAP_BLOCKING, &missing);
    if (result != BSP_OK) { return result; }
    device->config = *config;
    result = bsp_pwm_stop(config->pwm_port, config->pwm_channel);
    device->cleanup_status = result;
    if (result != BSP_OK) { device->faulted = true; return result; }
    device->initialized = true;
    device->actual_pulse_us = 0;
    return BSP_OK;
}
BspStatus rc_servo_set_pulse_us(RcServo *device, uint16_t pulse_us)
{
    if (!device) { return BSP_INVALID_ARG; }
    if (!device->initialized) { return BSP_NOT_READY; }
    if (device->faulted) { return BSP_INVALID_STATE; }
    if (pulse_us < device->config.min_pulse_us || pulse_us > device->config.max_pulse_us) {
        device->faulted = true;
        device->cleanup_status = bsp_pwm_stop(device->config.pwm_port, device->config.pwm_channel);
        return BSP_INVALID_ARG;
    }
    uint32_t duty = (uint32_t)(((uint64_t)pulse_us * 10000u + device->config.frame_us / 2u) /
                             device->config.frame_us);
    if (!duty || duty >= 10000u) { return BSP_UNSUPPORTED; }
    uint32_t actual = (uint32_t)(((uint64_t)duty * device->config.frame_us + 5000u) / 10000u);
    if (actual < device->config.min_pulse_us || actual > device->config.max_pulse_us) {
        return BSP_UNSUPPORTED;
    }
    BspStatus result = bsp_pwm_set(device->config.pwm_port, device->config.pwm_channel, (uint16_t)duty);
    if (result != BSP_OK) {
        device->faulted = true;
        device->cleanup_status = bsp_pwm_stop(device->config.pwm_port, device->config.pwm_channel);
        return result;
    }
    device->actual_pulse_us = actual;
    return BSP_OK;
}
BspStatus rc_servo_release(RcServo *device)
{
    if (!device) { return BSP_INVALID_ARG; }
    if (!device->initialized && !device->faulted) { return BSP_NOT_READY; }
    BspStatus result = bsp_pwm_stop(device->config.pwm_port, device->config.pwm_channel);
    device->cleanup_status = result;
    if (result != BSP_OK) { device->faulted = true; return result; }
    device->actual_pulse_us = 0;
    return BSP_OK;
}

BspStatus rc_servo_deinit(RcServo *device)
{
    BspStatus status=rc_servo_release(device);
    if(status==BSP_OK) { device->initialized=false; }
    return status;
}
