#ifndef FAN_PWM_H
#define FAN_PWM_H
#include "bsp_pwm.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { BspPort port; size_t channel; } FanPwmConfig;
typedef struct {
    FanPwmConfig config;
    uint16_t duty_bp;
    BspStatus status, cleanup_status;
    bool initialized, faulted;
} FanPwm;
/* Exclusive task owner; zero-initialize. Composition must initialize active-high
 * hardware PWM at 10..25 kHz first. Driver cannot query configured frequency.
 * An init command failure can leave initialized=true for explicit cleanup. */
BspStatus fan_pwm_init(FanPwm *fan, const FanPwmConfig *config, uint16_t duty_bp);
/* 0..10000 basis points of HIGH time, not measured RPM. Zero may not stop rotor.
 * A failed BSP command latches faulted; explicit deinit/reinit is required. */
BspStatus fan_pwm_set_duty(FanPwm *fan, uint16_t duty_bp);
/* Stops PWM output only; does not disconnect 12V or guarantee stopped rotor.
 * Failure retains initialized=true. Platform port lifetime is application-owned. */
BspStatus fan_pwm_deinit(FanPwm *fan);
#ifdef __cplusplus
}
#endif
#endif
