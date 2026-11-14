/* MOCE-SYNC: SDK-owned system peripherals | ../NATIVE_SYSTEM.md */
#ifndef BSP_ECW6700_SYSTEM_H
#define BSP_ECW6700_SYSTEM_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t cpu_hz, ahb_hz, apb_hz;
    bool profile_matches; /* CPU hardware readback matches fixed SDK profile. */
} Ecw6700ClockState;
typedef struct {
    uint32_t hours, minutes, seconds, milliseconds;
    uint32_t counter_32k; /* Separate native observation; not atomic with fields. */
} Ecw6700RtcTime;
/* BSP_ECW_SYSTEM_EXTENSIONS opt-in, main/task calls. SDK chip_init retains RTC
 * initialization and all IRQ/calibration/PSM ownership. No reinit or alarm hook.
 * AHB/APB report fixed SDK profile values, not independent register readback. */
BspStatus bsp_ecw6700_clock_query(Ecw6700ClockState *out);
/* Time-of-day hardware read, no fabricated UTC. SDK getter uses mktime and is
 * timezone dependent; this extension deliberately does not expose it as UTC. */
BspStatus bsp_ecw6700_rtc_read(Ecw6700RtcTime *out);
/* Explicit shared SDK calendar write, seconds since 1970 UTC, 1970..2099.
 * Serialize with all SDK RTC writers. Does not provide backup retention proof. */
BspStatus bsp_ecw6700_rtc_write_utc(int64_t seconds);
/* Observe SDK PSM policy only; UNSUPPORTED if CONFIG_PSM_SURPORT is off.
 * No claim that configured policy means CPU is physically asleep. */
BspStatus bsp_ecw6700_power_query(int *native_sleep_mode);
#ifdef __cplusplus
}
#endif
#endif
