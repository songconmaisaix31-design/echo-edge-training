#ifndef MOCE_RC_SERVO_H
#define MOCE_RC_SERVO_H
#include "bsp_pwm.h"
typedef struct {
    BspPort pwm_port;
    size_t pwm_channel;
    uint32_t frame_us;
    uint16_t min_pulse_us, max_pulse_us;
} RcServoConfig;
typedef struct {
    RcServoConfig config;
    bool initialized, faulted;
    uint32_t actual_pulse_us;
    BspStatus cleanup_status;
} RcServo;
/* Caller zero-initializes and serializes each instance. Application owns a PWM
 * port initialized to a frequency matching frame_us. Config copied by value.
 * BSP basis-point duty quantizes pulses; actual_pulse_us reports that command.
 * No mechanical angle/calibration, power cutoff, or physical-stop guarantee.
 * Init leaves output stopped; I/O faults latch, release remains available. */
BspStatus rc_servo_init(RcServo *device, const RcServoConfig *config);
BspStatus rc_servo_set_pulse_us(RcServo *device, uint16_t pulse_us);
BspStatus rc_servo_release(RcServo *device);
/* Stop pulses; application-owned BSP port remains held. Fault stays latched. */
BspStatus rc_servo_deinit(RcServo *device);
#endif
