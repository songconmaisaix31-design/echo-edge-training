#ifndef MOCE_NTC_THERMISTOR_H
#define MOCE_NTC_THERMISTOR_H

#include "bsp_adc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTC_THERMISTOR_VERSION "1.0.0"
#define NTC_THERMISTOR_MAX_SAMPLES 32u
#define NTC_THERMISTOR_MAX_TABLE_POINTS 256u

/* Module evidence: MF52A103F3950(A1), 10k fixed pull-up, NTC to GND;
 * divider -> 1k series -> AO, AO -> 100nF -> GND; CN1 1=3V3,2=AO,3=GND.
 * Shiheng exact-part approval C0 (2026-05-28): R25=10k +/-1%, B25/50=3950K
 * +/-1%, operating -40..125C. Config supplies parameters/curve, never defaults.
 * Public BSP ADC only. Caller initializes ports, owns zero-initialized instance,
 * and serializes all calls in task/main context. No allocation or automatic retries.
 */
typedef enum { NTC_TO_GND = 0, NTC_TO_SUPPLY = 1 } NtcTopology;
typedef enum {
    NTC_RESISTANCE_ONLY = 0, NTC_BETA = 1, NTC_STEINHART_HART = 2, NTC_TABLE = 3
} NtcModel;
typedef enum { NTC_MEAN = 0, NTC_TRIMMED_MEAN = 1, NTC_MEDIAN = 2 } NtcFilter;
typedef struct { double temperature_c, resistance_ohm; } NtcThermistorPoint;
enum {
    NTC_LOW_RAIL = 1u,
    NTC_HIGH_RAIL = 2u,
    NTC_TEMPERATURE_OUTSIDE_RANGE = 4u,
    NTC_ADC_OUTSIDE_RANGE = 8u
};

typedef struct {
    BspPort port;
    size_t channel; /* Compact composed index, never a silicon channel number. */
    BspAdcReadOptions read;
    uint8_t samples;
    NtcFilter filter;
    double ema_alpha; /* (0,1], 1 disables temporal smoothing. */
    /* divider ratio = (ADC value * input_gain + input_offset) / full_scale.
     * UV mode: full_scale is measured/nominal excitation at module VCC, UV.
     * RAW mode: full_scale is converter max code ONLY IF VREF and excitation
     * track (ratiometric, e.g. shared F407 VDDA/3V3). Not valid for arbitrary ADCs.
     * gain/offset can compensate a known analog transfer; no hidden auto-calibration. */
    double full_scale, input_gain, input_offset;
    double valid_input_min, valid_input_max; /* Verified ADC range, native units. */
    double fixed_resistance_ohm;
    double rail_margin; /* (0,0.5), rail diagnostics, NOT proof of open/short. */
    NtcTopology topology;
    NtcModel model;
    double r0_ohm, t0_c, beta_kelvin; /* BETA model, log(R/R0). */
    double sh_a, sh_b, sh_c; /* SH model: 1/T = A+B*ln(R)+C*ln(R)^3; R in ohms. */
    /* TABLE: 2..256 points, strictly increasing C and decreasing positive ohms.
     * Caller keeps immutable table alive through driver deinit; pointer is borrowed.
     * Piecewise linear 1/K vs ln(ohms), no extrapolation. */
    const NtcThermistorPoint *table;
    size_t table_count;
    double temperature_offset_c; /* Explicit one-point temperature correction. */
    double minimum_c, maximum_c; /* Application validity window, not sensor rating. */
} NtcThermistorConfig;

typedef struct {
    BspAdcResult last;
    double minimum, maximum, average, filtered; /* ADC units. */
    double ratio, resistance_ohm, temperature_c, temperature_k, temperature_f;
    uint32_t flags;
    bool resistance_valid, temperature_valid;
} NtcThermistorReading;

typedef struct {
    NtcThermistorConfig config;
    double filtered;
    bool initialized, have_filter;
} NtcThermistor;

/* Capabilities checked at init. Repeated init -> INVALID_STATE. Port lifecycle
 * stays with config/application; deinit clears driver only. Failed operations
 * preserve device/result. Sync buffers return on every call, no async borrowing.
 * N samples consume N native waits; SDK_NATIVE may be unbounded. No total
 * hard deadline or sample-spacing/accuracy guarantee is provided by this driver.
 */
BspStatus ntc_thermistor_init(NtcThermistor *device, const NtcThermistorConfig *config);
BspStatus ntc_thermistor_read(NtcThermistor *device, NtcThermistorReading *out);
BspStatus ntc_thermistor_reset_filter(NtcThermistor *device);
BspStatus ntc_thermistor_deinit(NtcThermistor *device);
/* Pure computations, no hardware. Rail samples return OK + diagnostic flags,
 * both validity flags false. Never interpret the zeroed temperature as 0 C.
 * Outside configured temperature window: resistance valid, temperature invalid.
 * Disconnected AO cannot reliably be detected without additional hardware. */
BspStatus ntc_thermistor_convert(const NtcThermistorConfig *config, double input,
                                NtcThermistorReading *out);
BspStatus ntc_thermistor_resistance_to_c(const NtcThermistorConfig *config,
                                       double resistance_ohm, double *out_c);
/* BETA/TABLE inverse; other models return UNSUPPORTED, no guessed solver.
 * TABLE inputs outside its curve return BSP_IO and preserve the output. */
BspStatus ntc_thermistor_c_to_resistance(const NtcThermistorConfig *config,
                                       double temperature_c, double *out_ohm);

#ifdef __cplusplus
}
#endif
#endif
