#include "fan_pwm.h"

BspStatus fan_pwm_init(FanPwm *fan, const FanPwmConfig *config, uint16_t duty_bp)
{
    if (!fan || !config || (config->port >> 8) != BSP_PWM || duty_bp > 10000u)
        return BSP_INVALID_ARG;
    if (fan->initialized) return BSP_INVALID_STATE;
    uint32_t missing;
    BspStatus status = bsp_port_require(config->port, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) return status;
    fan->config = *config;
    fan->initialized = true;
    fan->faulted = false;
    fan->status = fan->cleanup_status = BSP_OK;
    return fan_pwm_set_duty(fan, duty_bp);
}

BspStatus fan_pwm_set_duty(FanPwm *fan, uint16_t duty_bp)
{
    if (!fan || duty_bp > 10000u) return BSP_INVALID_ARG;
    if (!fan->initialized) return BSP_NOT_READY;
    if (fan->faulted) return fan->status;
    BspStatus status = bsp_pwm_set(fan->config.port, fan->config.channel, duty_bp);
    fan->status = status;
    if (status == BSP_OK) fan->duty_bp = duty_bp;
    else fan->faulted = true;
    return status;
}

BspStatus fan_pwm_deinit(FanPwm *fan)
{
    if (!fan) return BSP_INVALID_ARG;
    if (!fan->initialized) return BSP_NOT_READY;
    BspStatus status = bsp_pwm_stop(fan->config.port, fan->config.channel);
    fan->cleanup_status = status;
    if (status == BSP_OK) {
        fan->initialized = false;
        fan->faulted = false;
    }
    return status;
}
