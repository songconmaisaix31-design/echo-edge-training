#ifndef MOCE_TOUCH_KEY_H
#define MOCE_TOUCH_KEY_H
#include "bsp_gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Digital touch module with a level output. No capacitive measurement/PWM.
 * Caller initializes/owns an input GPIO with BSP_CAP_BLOCKING, zero-initializes
 * the instance and serializes calls. Config is copied. No retries or allocation.
 * active_high must match the module. No debounce, warmup or latch conversion.
 * GPIO failure leaves out unchanged and breaks transition continuity.
 * First successful read (also after error) establishes state: changed=false.
 * Polling can miss short pulses; changed is not a hardware interrupt/event count.
 * deinit releases only the driver binding, not the application-owned BSP port.
 */
typedef struct {
    BspPort input;
    bool active_high;
} TouchKeyConfig;
typedef struct {
    bool raw_high;
    bool touched; /* Interprets asserted output. Physical touch only in momentary mode. */
    bool changed;
    uint32_t sampled_ms;
} TouchKeySample;
typedef struct {
    TouchKeyConfig config;
    bool bound;
    bool previous_valid;
    bool previous_touched;
} TouchKey;
BspStatus touch_key_init(TouchKey *self, const TouchKeyConfig *config);
BspStatus touch_key_read(TouchKey *self, TouchKeySample *out);
BspStatus touch_key_deinit(TouchKey *self);
#ifdef __cplusplus
}
#endif
#endif

