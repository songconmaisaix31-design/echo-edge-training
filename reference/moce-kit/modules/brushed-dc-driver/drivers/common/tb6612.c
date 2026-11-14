#include "tb6612.h"
static BspStatus brake_outputs(Tb6612 *device)
{
    BspStatus first = bsp_pwm_stop(device->config.pwm_port, device->config.pwm_channel);
    BspStatus status = bsp_gpio_write(device->config.in1_port, true);
    if (first == BSP_OK)
    {
        first = status;
    }
    status = bsp_gpio_write(device->config.in2_port, true);
    if (first == BSP_OK)
    {
        first = status;
    }
    return first;
}
static BspStatus fail_operation(Tb6612 *device, BspStatus original)
{
    device->faulted = true;
    device->cleanup_status = brake_outputs(device);
    return original;
}
BspStatus tb6612_init(Tb6612 *device, const Tb6612Config *config)
{
    if (!device || !config || (config->pwm_port >> 8) != BSP_PWM ||
        (config->in1_port >> 8) != BSP_GPIO || (config->in2_port >> 8) != BSP_GPIO ||
        config->in1_port == config->in2_port)
    {
        return BSP_INVALID_ARG;
    }
    if (device->configured || device->initialized || device->faulted)
    {
        return BSP_INVALID_STATE;
    }
    const BspPort ports[] = {config->pwm_port, config->in1_port, config->in2_port};
    for (size_t i = 0; i < sizeof(ports) / sizeof(ports[0]); ++i)
    {
        uint32_t missing = 0;
        BspStatus status = bsp_port_require(ports[i], BSP_CAP_BLOCKING, &missing);
        if (status != BSP_OK)
        {
            return status;
        }
    }
    device->config = *config;
    device->configured = true;
    BspStatus status = brake_outputs(device);
    if (status != BSP_OK)
    {
        return fail_operation(device, status);
    }
    device->commanded_duty_bp = 0;
    device->cleanup_status = BSP_OK;
    device->initialized = true;
    return BSP_OK;
}
BspStatus tb6612_stop(Tb6612 *device, Tb6612StopMode mode)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->configured)
    {
        return BSP_NOT_READY;
    }
    if (mode != TB6612_BRAKE && mode != TB6612_COAST)
    {
        return fail_operation(device, BSP_INVALID_ARG);
    }
    if (device->faulted && mode != TB6612_BRAKE)
    {
        return fail_operation(device, BSP_INVALID_STATE);
    }
    BspStatus status = brake_outputs(device);
    if (status != BSP_OK)
    {
        return fail_operation(device, status);
    }
    if (mode == TB6612_COAST)
    {
        /* Datasheet Stop: both IN low, PWM high; PWM low alone is brake. */
        status = bsp_gpio_write(device->config.in1_port, false);
        if (status == BSP_OK)
        {
            status = bsp_gpio_write(device->config.in2_port, false);
        }
        if (status == BSP_OK)
        {
            status = bsp_pwm_set(device->config.pwm_port, device->config.pwm_channel,
                                 TB6612_DUTY_MAX_BP);
        }
        if (status != BSP_OK)
        {
            return fail_operation(device, status);
        }
    }
    device->commanded_duty_bp = 0;
    device->cleanup_status = BSP_OK;
    return BSP_OK;
}
BspStatus tb6612_drive(Tb6612 *device, int16_t duty_bp)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized)
    {
        return BSP_NOT_READY;
    }
    if (device->faulted)
    {
        return BSP_INVALID_STATE;
    }
    if (duty_bp < -(int)TB6612_DUTY_MAX_BP || duty_bp > (int)TB6612_DUTY_MAX_BP)
    {
        return fail_operation(device, BSP_INVALID_ARG);
    }
    if (duty_bp == 0)
    {
        return tb6612_stop(device, TB6612_BRAKE);
    }
    if ((duty_bp > 0 && device->commanded_duty_bp < 0) ||
        (duty_bp < 0 && device->commanded_duty_bp > 0))
    {
        return fail_operation(device, BSP_INVALID_STATE);
    }
    BspStatus status = bsp_pwm_stop(device->config.pwm_port, device->config.pwm_channel);
    if (status == BSP_OK)
    {
        status = bsp_gpio_write(device->config.in1_port, duty_bp > 0);
    }
    if (status == BSP_OK)
    {
        status = bsp_gpio_write(device->config.in2_port, duty_bp < 0);
    }
    if (status == BSP_OK)
    {
        uint16_t magnitude = (uint16_t)(duty_bp < 0 ? -(int)duty_bp : duty_bp);
        status = bsp_pwm_set(device->config.pwm_port, device->config.pwm_channel, magnitude);
    }
    if (status != BSP_OK)
    {
        return fail_operation(device, status);
    }
    device->commanded_duty_bp = duty_bp;
    return BSP_OK;
}
BspStatus tb6612_deinit(Tb6612 *device)
{
    BspStatus status = tb6612_stop(device, TB6612_BRAKE);
    if (status == BSP_OK)
    {
        device->initialized = false;
        device->configured = false;
    }
    return status;
}
