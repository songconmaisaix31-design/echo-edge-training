#ifndef MOCE_ROTARY_ENCODER_H
#define MOCE_ROTARY_ENCODER_H
#include "bsp_gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ROTARY_ENCODER_DEFAULT_DEBOUNCE_MS 3u
#define ROTARY_ENCODER_PULSES_PER_REVOLUTION 24u
/* Zero-initialize. One caller, task/main context. Caller owns two distinct,
 * initialized input ports and power. No Board IDs, HAL, heap, IRQ or timers.
 * Poll faster than the shortest phase dwell; <=1ms is a starting recommendation,
 * not a guaranteed speed rating. Both sequential GPIO reads must finish within
 * that dwell. Lost complete cycles cannot be detected by software polling. */
typedef enum { ROTARY_ENCODER_DIRECTION_NONE=0,
    ROTARY_ENCODER_DIRECTION_CW=1, ROTARY_ENCODER_DIRECTION_CCW=-1
} RotaryEncoderDirection;
typedef enum { ROTARY_ENCODER_ERROR_NONE, ROTARY_ENCODER_ERROR_READ,
    ROTARY_ENCODER_ERROR_GAP, ROTARY_ENCODER_ERROR_TRANSITION
} RotaryEncoderError;
typedef struct {
    BspPort phase_a_port, phase_b_port;
    uint32_t debounce_ms; /* Stable detent dwell before publishing a cycle. */
    uint32_t max_sample_gap_ms; /* 1..INT32_MAX; excessive gap loses continuity. */
    uint8_t detent_state; /* A=bit1, B=bit0, 0..3; confirm on actual hardware. */
    bool invert_direction; /* Positive convention: 00->10->11->01->00. */
} RotaryEncoderConfig;
typedef struct {
    bool valid, position_certain; /* Uncertainty is sticky until set/reset. */
    bool phase_a, phase_b;
    int32_t position, delta; /* Full electrical cycles, 24/rev for supplied A2. */
    int32_t turns, angle_millidegrees; /* Relative: quotient/remainder of position. */
    RotaryEncoderDirection direction; /* This update only, not persistent speed. */
    uint32_t sampled_at_ms, samples, invalid_transitions, read_errors, gaps;
    bool overflow; /* Sticky position saturation; delta is actual position change. */
} RotaryEncoderReading;
typedef struct {
    RotaryEncoderConfig config;
    RotaryEncoderReading reading;
    uint8_t state;
    int8_t partial, pending;
    uint32_t last_update_ms, settled_since_ms;
    bool initialized, synced, have_sample;
    RotaryEncoderError last_error;
} RotaryEncoder;
BspStatus rotary_encoder_init(RotaryEncoder *, const RotaryEncoderConfig *, uint32_t);
BspStatus rotary_encoder_update(RotaryEncoder *, uint32_t);
BspStatus rotary_encoder_get(const RotaryEncoder *, uint32_t, RotaryEncoderReading *);
/* Clear partial cycle, establish caller's relative origin; no physical homing. */
BspStatus rotary_encoder_set_position(RotaryEncoder *, int32_t);
/* Reset relative position/statistics to zero; next update resynchronizes. */
BspStatus rotary_encoder_reset(RotaryEncoder *, uint32_t);
void rotary_encoder_deinit(RotaryEncoder *);
#ifdef __cplusplus
}
#endif
#endif
