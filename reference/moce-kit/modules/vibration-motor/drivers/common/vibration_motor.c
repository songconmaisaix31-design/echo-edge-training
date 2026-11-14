#include "vibration_motor.h"
#include "bsp_gpio.h"
#include "bsp_pwm.h"
#include <limits.h>
#include <string.h>

static bool duty_valid(const VibrationMotor *motor, uint16_t duty)
{
    return duty <= VIBRATION_MOTOR_FULL_DUTY &&
        (motor->config.mode == VIBRATION_MOTOR_PWM || duty == 0u ||
         duty == VIBRATION_MOTOR_FULL_DUTY);
}

static BspStatus write_output(VibrationMotor *motor, uint16_t duty)
{
    BspStatus status;
    if (motor->config.mode == VIBRATION_MOTOR_GPIO)
        status = bsp_gpio_write(motor->config.port, duty != 0u);
    else
        /* Keep PWM enabled at 0%: stopping a timer alone does not prove pin low. */
        status = bsp_pwm_set(motor->config.port, motor->config.channel, duty);
    motor->state.output_known = status == BSP_OK;
    if (status == BSP_OK) motor->state.commanded_duty_bp = duty;
    return status;
}

static BspStatus fail_safe(VibrationMotor *motor, BspStatus primary)
{
    motor->state.faulted = true;
    if (motor->state.error == BSP_OK) motor->state.error = primary;
    motor->state.stop_error = write_output(motor, 0u);
    return primary;
}

BspStatus vibration_motor_init(VibrationMotor *motor, const VibrationMotorConfig *config)
{
    uint32_t missing = 0u;
    BspStatus status;
    if (!motor || !config) return BSP_INVALID_ARG;
    if (motor->state.initialized) return BSP_INVALID_STATE;
    if ((config->mode != VIBRATION_MOTOR_GPIO && config->mode != VIBRATION_MOTOR_PWM) ||
        (config->mode == VIBRATION_MOTOR_GPIO && config->channel != 0u) ||
        (config->port >> 8) != (unsigned)(config->mode == VIBRATION_MOTOR_GPIO ? BSP_GPIO : BSP_PWM))
        return BSP_INVALID_ARG;
    status = bsp_port_require(config->port, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) return status;
    memset(motor, 0, sizeof(*motor));
    motor->config = *config;
    motor->state.initialized = true;
    status = write_output(motor, 0u);
    if (status != BSP_OK) {
        motor->state.faulted = true;
        motor->state.error = status;
        motor->state.stop_error = status;
    }
    return status;
}

BspStatus vibration_motor_stop(VibrationMotor *motor)
{
    BspStatus status;
    if (!motor) return BSP_INVALID_ARG;
    if (!motor->state.initialized) return BSP_NOT_READY;
    status = write_output(motor, 0u);
    motor->state.stop_error = status;
    if (status != BSP_OK) {
        motor->state.faulted = true;
        if (motor->state.error == BSP_OK) motor->state.error = status;
    }
    return status;
}

BspStatus vibration_motor_set(VibrationMotor *motor, uint16_t duty_bp)
{
    BspStatus status;
    if (!motor) return BSP_INVALID_ARG;
    if (!motor->state.initialized) return BSP_NOT_READY;
    if (duty_bp == 0u) return vibration_motor_stop(motor);
    if (motor->state.faulted) return BSP_INVALID_STATE;
    if (!duty_valid(motor, duty_bp)) return fail_safe(motor, BSP_INVALID_ARG);
    status = write_output(motor, duty_bp);
    return status == BSP_OK ? BSP_OK : fail_safe(motor, status);
}

BspStatus vibration_motor_pattern(VibrationMotor *motor, const VibrationMotorStep *steps,
                                  size_t count, uint16_t repeats)
{
    uint64_t total = 0u;
    BspStatus status;
    if (!motor) return BSP_INVALID_ARG;
    if (!motor->state.initialized) return BSP_NOT_READY;
    if (motor->state.faulted) return BSP_INVALID_STATE;
    if (!steps || count == 0u || count > VIBRATION_MOTOR_MAX_STEPS || repeats == 0u)
        return fail_safe(motor, BSP_INVALID_ARG);
    for (size_t i = 0u; i < count; ++i) {
        if (!duty_valid(motor, steps[i].duty_bp) || steps[i].duration_ms == 0u ||
            steps[i].duration_ms > INT32_MAX)
            return fail_safe(motor, BSP_INVALID_ARG);
        total += steps[i].duration_ms;
    }
    if (total * repeats > INT32_MAX) return fail_safe(motor, BSP_INVALID_ARG);
    for (uint16_t repeat = 0u; repeat < repeats; ++repeat) {
        for (size_t i = 0u; i < count; ++i) {
            status = vibration_motor_set(motor, steps[i].duty_bp);
            if (status != BSP_OK) return status;
            status = bsp_delay_ms(steps[i].duration_ms);
            if (status != BSP_OK) return fail_safe(motor, status);
        }
    }
    return vibration_motor_stop(motor);
}

BspStatus vibration_motor_pulse(VibrationMotor *motor, uint16_t duty_bp, uint32_t duration_ms)
{
    const VibrationMotorStep step = {duty_bp, duration_ms};
    return vibration_motor_pattern(motor, &step, 1u, 1u);
}

BspStatus vibration_motor_query(const VibrationMotor *motor, VibrationMotorState *state)
{
    if (!motor || !state) return BSP_INVALID_ARG;
    *state = motor->state;
    return BSP_OK;
}

BspStatus vibration_motor_deinit(VibrationMotor *motor)
{
    BspStatus status;
    if (!motor) return BSP_INVALID_ARG;
    if (!motor->state.initialized) return BSP_OK;
    status = vibration_motor_stop(motor);
    if (status == BSP_OK) memset(motor, 0, sizeof(*motor));
    return status;
}
