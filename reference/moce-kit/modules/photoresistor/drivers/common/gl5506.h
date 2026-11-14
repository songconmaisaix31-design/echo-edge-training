#ifndef MOCE_GL5506_H
#define MOCE_GL5506_H
#include "bsp_adc.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GL5506_MAX_SAMPLES 64u
typedef struct {
    BspPort port;
    size_t channel_index;
    BspAdcFormat format; /* Explicit RAW or MICROVOLTS; never auto-fallback. */
    BspAdcWait wait;
    uint32_t timeout_ms; /* Per conversion, not whole-call deadline. */
    uint8_t sample_count; /* 1..64, consecutive reads without delay/retry. */
    uint8_t resolution_bits; /* RAW unsigned 1..32; MICROVOLTS requires 0. */
    uint32_t valid_min; /* Inclusive raw code or microvolts, matching format. */
    uint32_t valid_max;
    uint32_t supply_uv; /* Explicit module supply; RAW-only configurations may use 0. */
    uint32_t pullup_ohms; /* R3 pullup; required when estimating resistance. */
    uint32_t raw_reference_uv; /* Explicit nominal/measured RAW full-scale voltage; 0 disables. */
    bool report_out_of_window; /* Report range flag instead of treating range as IO error. */
    bool lux_enabled; /* Optional uncalibrated model selected by project. */
    double r10_ohms; /* Resistance at 10 lux, positive finite. */
    double gamma; /* Positive finite exponent in R=R10*(10/lux)^gamma. */
} Gl5506Config;
typedef struct {
    Gl5506Config config;
    bool initialized;
} Gl5506;
typedef struct {
    BspAdcFormat format;
    uint8_t resolution_bits;
    uint32_t value; /* Rounded mean in explicitly tagged units. */
    bool resistance_valid; /* false if conversion disabled, at rails or outside configured window. */
    double resistance_ohms;
    bool outside_window; /* At least one sample outside configured valid_min/max. */
    uint32_t sample_min, sample_max;
    bool illuminance_valid;
    double illuminance_lux; /* Estimate only, no accuracy claim. */
} Gl5506Reading;
typedef enum {
    GL5506_DIAG_NONE, GL5506_DIAG_ADC, GL5506_DIAG_FORMAT, GL5506_DIAG_WINDOW
} Gl5506DiagnosticReason;
typedef struct {
    Gl5506DiagnosticReason reason;
    uint8_t sample_index;
    BspAdcResult sample; /* Valid only for FORMAT/WINDOW, not ADC failure. */
} Gl5506Diagnostic;
/* GL5506 module: 3V3 -> R3 10k -> GL5506 -> GND, AO through R1 1k/C2 100nF.
 * More light decreases AO. R1 is not part of the DC divider at high impedance.
 * Optional RAW estimate uses explicitly supplied raw_reference_uv and full-scale code.
 * Lux model is project-supplied; no accuracy/calibration is implied.
 * Outside-window groups never report resistance/lux. Native ADC errors still stop.
 * Link libm. Diagnostic output is reset each call and is separate from reading output.
 * Application initializes/owns the ADC, settles the circuit and serializes access.
 * Zero-initialize instance. Config is copied; successful init has no ADC side effects.
 * Reinitializing an initialized instance returns INVALID_STATE. All errors preserve
 * caller output/instance. IO means malformed/out-of-window data, not wiring diagnosis.
 * Read failure stops immediately. SDK_NATIVE is not a hard timing guarantee.
 */
BspStatus gl5506_init(Gl5506 *device, const Gl5506Config *config);
BspStatus gl5506_read(const Gl5506 *device, Gl5506Reading *out);
BspStatus gl5506_read_diagnostic(const Gl5506 *device, Gl5506Reading *out,
                                 Gl5506Diagnostic *diagnostic);
#ifdef __cplusplus
}
#endif
#endif
