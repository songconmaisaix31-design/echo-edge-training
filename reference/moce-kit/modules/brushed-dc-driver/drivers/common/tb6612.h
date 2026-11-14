#ifndef MOCE_TB6612_H
#define MOCE_TB6612_H
#include "bsp_gpio.h"
#include "bsp_pwm.h"
#define TB6612_DUTY_MAX_BP 10000u
typedef enum
{
    TB6612_BRAKE = 0,
    TB6612_COAST
} Tb6612StopMode;
typedef struct
{
    BspPort pwm_port;
    size_t pwm_channel;
    BspPort in1_port;
    BspPort in2_port;
} Tb6612Config;
typedef struct
{
    Tb6612Config config;
    bool configured;
    bool initialized;
    bool faulted;
    int16_t commanded_duty_bp;
    BspStatus cleanup_status;
} Tb6612;
/* Zero-initialize one instance per H bridge. Application owns/serializes BSP
 * ports. Configuration is copied; no heap, tasks, timers, Board or HAL calls.
 * Requires externally enabled STBY; there is no STBY port in this binding.
 * Signed duty -10000..10000 selects electrical polarity, not measured RPM.
 * Direct nonzero polarity reversal is rejected and brakes/latches a fault.
 * Explicitly stop and establish physical standstill before reversing.
 * Invalid commands/I/O errors attempt all brake outputs and retain the original
 * error separately from cleanup_status. No return proves physical standstill.
 * Faulted instances allow brake/deinit attempts but never new drive commands.
 * Resolve BSP/hardware fault and initialize fresh storage explicitly; no retry.
 * deinit brakes this device; it does not release application-owned BSP ports.
 * Releasing GPIO/PWM may float signals: disable STBY/power before doing so.
 */
BspStatus tb6612_init(Tb6612 *device, const Tb6612Config *config);
BspStatus tb6612_drive(Tb6612 *device, int16_t duty_bp);
BspStatus tb6612_stop(Tb6612 *device, Tb6612StopMode mode);
BspStatus tb6612_deinit(Tb6612 *device);
#endif
