#ifndef SC01B_H
#define SC01B_H
#include "bsp_gpio.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Fixed liquid-mode SC01B: OUT low = liquid at the electrode.
 * No mode/sensitivity/register/ADC interface exists on this module.
 * Zero-initialize instances. Application owns port init/deinit and power.
 * One caller per instance, task context only. No allocation or hidden task.
 * now_ms: wrapping monotonic milliseconds; call gaps must be <2^31 ms.
 * Startup wait starts at init, after application has established power. */
#define SC01B_MIN_STARTUP_MS 120u
typedef enum { SC01B_UNKNOWN, SC01B_DRY, SC01B_WET } Sc01bState;
enum {
    SC01B_EVENT_INITIAL = 1u, SC01B_EVENT_WET = 2u, SC01B_EVENT_DRY = 4u,
    SC01B_EVENT_READ_ERROR = 8u, SC01B_EVENT_SAMPLE_GAP = 16u
};
typedef struct {
    BspPort port;               /* Already initialized input port. */
    uint32_t startup_ms;        /* >=120; datasheet typical, not maximum. */
    uint32_t wet_confirm_ms;    /* Software stability filter, zero allowed. */
    uint32_t dry_confirm_ms;
    uint32_t max_sample_gap_ms; /* >0; larger gaps invalidate continuity. */
} Sc01bConfig;
typedef struct {
    Sc01bState state;
    bool raw_valid, raw_high;
    uint32_t sampled_at_ms, stable_ms; /* Sampled stable duration, saturating. */
    uint32_t events; /* Most recent update only; not a queue. */
    uint32_t samples, read_errors, gaps, wet_entries, dry_entries;
    uint64_t observed_wet_ms, observed_dry_ms; /* Estimates, not metrology. */
    BspStatus last_status;
} Sc01bReading;
typedef struct {
    Sc01bConfig config;
    Sc01bReading reading;
    bool initialized, ready, candidate_valid;
    Sc01bState candidate;
    uint32_t started_ms, last_update_ms, candidate_since_ms;
} Sc01b;
BspStatus sc01b_init(Sc01b *sensor, const Sc01bConfig *config, uint32_t now_ms);
BspStatus sc01b_update(Sc01b *sensor, uint32_t now_ms);
/* NOT_READY on unknown/stale data. Stale snapshots hide raw/state/events. */
BspStatus sc01b_get(const Sc01b *sensor, uint32_t now_ms, Sc01bReading *out);
/* Software teardown only: does not switch power, calibrate IC or free port. */
void sc01b_deinit(Sc01b *sensor);
const char *sc01b_state_name(Sc01bState state);
/* Optional duration reminders, merged at user request; thresholds supplied by caller. */
typedef struct {
    bool valid, wet_active, dry_active, wet_latched, dry_latched;
} WaterAlarm;
/* Product policy: unknown clears active conditions, preserves history. */
void water_alarm_update(WaterAlarm *alarm, const Sc01bReading *reading,
                        uint32_t wet_ms, uint32_t dry_ms);
/* Clear only inactive alarms with currently valid data. */
void water_alarm_acknowledge(WaterAlarm *alarm);
#ifdef __cplusplus
}
#endif
#endif
