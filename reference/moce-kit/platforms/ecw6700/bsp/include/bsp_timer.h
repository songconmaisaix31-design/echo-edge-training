/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_TIMER_H
#define BSP_TIMER_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef uint16_t
        BspTimer; /* Board-bound hardware resource ID, not a software slot or vendor TIM number. */
    typedef struct
    {
        BspTimer id;
        const char *name;
    } BspTimerInfo;
    typedef struct
    {
        const BspTimerInfo *timers;
        size_t count;
    } BspTimerInventory;
    const BspTimerInventory *bsp_board_get_timer_inventory(void);
    /* All-zero fields mean the known hardware resource has no adapted timer modes. */
    typedef struct
    {
        uint32_t resolution_us, min_period_ms, max_period_ms;
        bool periodic;
    } BspTimerCapabilities;
    typedef struct
    {
        uint64_t generation;
        uint32_t due_ms, observed_ms, expirations;
        bool saturated;
    } BspTimerEvent;
    /* Known IDs require an already initialized platform and task context.
 * Query reads configuration only; it never allocates/enables a timer. */
    BspStatus bsp_timer_capabilities(BspTimer id, BspTimerCapabilities *out);
    /* Hardware counter/alarm and IRQ required; no software deadline emulation.
 * One owner, task context. first_ms>0; period_ms=0 one-shot. Unsupported native
 * ranges/modes reject before effects. No arbitrary user callbacks run from IRQ.
 * Events aggregate observed hardware expirations with a saturating count, never
 * guessed missed periods. IRQ coalescing limits must be disclosed per adapter.
 * due_ms is scheduled time; observed_ms is software observation, not pin timing. */
    BspStatus bsp_timer_start(BspTimer id, uint32_t first_ms, uint32_t period_ms,
                              uint64_t *generation);
    /* Consume an event already produced by hardware IRQ; never drive timing here.
 * Empty on an active supported timer => OK/present=false, out unchanged. */
    BspStatus bsp_timer_poll(BspTimer id, BspTimerEvent *out, bool *present);
    /* Retirement barrier; clears queued events only after producer stopped. Failure
 * keeps resource held; late IRQ from an old generation cannot create a new event. */
    BspStatus bsp_timer_stop(BspTimer id, uint32_t cleanup_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
