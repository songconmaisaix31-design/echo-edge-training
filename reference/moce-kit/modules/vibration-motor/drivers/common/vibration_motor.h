#ifndef VIBRATION_MOTOR_H
#define VIBRATION_MOTOR_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C" {
#endif

#define VIBRATION_MOTOR_MAX_STEPS 32u
#define VIBRATION_MOTOR_FULL_DUTY 10000u

typedef enum { VIBRATION_MOTOR_GPIO, VIBRATION_MOTOR_PWM } VibrationMotorMode;
typedef struct {
    BspPort port;
    VibrationMotorMode mode;
    size_t channel; /* BSP PWM channel index; GPIO requires zero. */
} VibrationMotorConfig;
typedef struct { uint16_t duty_bp; uint32_t duration_ms; } VibrationMotorStep;
typedef struct {
    bool initialized, faulted, output_known;
    uint16_t commanded_duty_bp;
    BspStatus error, stop_error;
} VibrationMotorState;
typedef struct {
    VibrationMotorConfig config;
    VibrationMotorState state;
} VibrationMotor;

/* Zero-initialize the object. Caller owns/configures the exclusive BSP port and
 * keeps it alive until successful deinit. Calls are serialized in one task/main;
 * no ISR, concurrent calls, callbacks, timers, dynamic allocation or port release.
 * PWM is active-high; configure its frequency in BSP before init. Duty is a
 * requested electrical ratio, not calibrated vibration strength. GPIO only
 * supports 0/10000. State describes successful commands, never motor feedback.
 * Failed init can retain initialized=true: stop/deinit must then be retried.
 * Faults latch until deinit/reinit. Stop/deinit remain available after a fault.
 */
BspStatus vibration_motor_init(VibrationMotor *motor, const VibrationMotorConfig *config);
BspStatus vibration_motor_set(VibrationMotor *motor, uint16_t duty_bp);
BspStatus vibration_motor_stop(VibrationMotor *motor);
/* Blocking pulse/pattern: duration 1..INT32_MAX; total pattern <=INT32_MAX ms.
 * Delays may be extended by scheduling. No guaranteed cutoff if the task stalls.
 * On errors attempt electrical off once; retain both primary and stop errors.
 */
BspStatus vibration_motor_pulse(VibrationMotor *motor, uint16_t duty_bp, uint32_t duration_ms);
BspStatus vibration_motor_pattern(VibrationMotor *motor, const VibrationMotorStep *steps,
                                  size_t count, uint16_t repeats);
BspStatus vibration_motor_query(const VibrationMotor *motor, VibrationMotorState *state);
BspStatus vibration_motor_deinit(VibrationMotor *motor);

#ifdef __cplusplus
}
#endif
#endif
