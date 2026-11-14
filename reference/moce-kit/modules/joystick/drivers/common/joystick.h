#ifndef MOCE_JOYSTICK_H
#define MOCE_JOYSTICK_H
#include "bsp_adc.h"
#include "bsp_gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    BspPort adc, button;
    size_t axis_a_channel, axis_b_channel;
    uint32_t conversion_timeout_ms, debounce_ms;
    bool button_active_low;
} JoystickConfig;
typedef struct {
    uint16_t axis_a_raw, axis_b_raw;
    bool button_raw_pressed, button_pressed, button_valid;
    uint32_t sampled_ms;
} JoystickSample;
typedef struct {
    JoystickConfig config;
    uint32_t candidate_since;
    bool bound, tracking, candidate, stable, stable_valid;
} Joystick;
/* Zero-initialize before first init. Caller owns initialized BSP ports and serializes
 * calls. Config is copied. No allocations or port lifecycle changes.
 * Fixed unsigned 12-bit RAW contract; two sequential conversions, not simultaneous.
 * Read failure leaves output unchanged and invalidates debounce continuity.
 * Poll at intervals <2^31 ms. First button state becomes valid after debounce_ms.
 */
BspStatus joystick_init(Joystick *self, const JoystickConfig *config);
BspStatus joystick_read(Joystick *self, JoystickSample *out);
#ifdef __cplusplus
}
#endif
#endif

