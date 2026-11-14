/* Hardware PIT expiry only. IRQ callbacks produce events; poll never creates
 * deadlines or re-arms periodic timers. Single-core P16 task/IRQ ownership. */
#include "ecw_timer_native.h"
#include "ecw_sdk.h"
#include <limits.h>
#include <string.h>

typedef struct
{
    bool held, armed, fault, pending;
    int handle;
    uint32_t period, due;
    uint64_t generation;
    BspTimerEvent event;
} TimerState;
static TimerState timers[4];
static uint64_t serial;

static BspStatus binding(BspTimer id, size_t *index)
{
    const BspTimerInventory *inventory = bsp_board_get_timer_inventory();
    if (!inventory || (inventory->count && !inventory->timers))
    {
        return BSP_INVALID_STATE;
    }
    bool found = false;
    for (size_t i = 0; i < inventory->count; ++i)
    {
        if (inventory->timers[i].id == id)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = bsp_delay_ms(0);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!ecw_timer_board || !ecw_timer_board_valid(ecw_timer_board))
    {
        return BSP_UNSUPPORTED;
    }
    for (size_t i = 0; i < ecw_timer_board->timer_count; ++i)
    {
        if (ecw_timer_board->timers[i].id == id)
        {
            *index = i;
            return BSP_OK;
        }
    }
    return BSP_UNSUPPORTED;
}

BspStatus bsp_timer_capabilities(BspTimer id, BspTimerCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    size_t index;
    BspStatus status = binding(id, &index);
    if (status != BSP_OK && status != BSP_UNSUPPORTED)
    {
        return status;
    }
    *out = status == BSP_OK ? (BspTimerCapabilities){1u, 1u, ecw_timer_max_period_ms(), true}
                            : (BspTimerCapabilities){0};
    return BSP_OK;
}

static void expired(void *context)
{
    TimerState *timer = context;
    if (!timer->held || !timer->armed || timer->fault)
    {
        return;
    }
    if (!timer->pending)
    {
        timer->event = (BspTimerEvent){.generation = timer->generation,
                                       .due_ms = timer->due,
                                       .observed_ms = ecw_sdk_time_ms(),
                                       .expirations = 1u};
        timer->pending = true;
    }
    else if (timer->event.expirations == UINT32_MAX)
    {
        timer->event.saturated = true;
    }
    else
    {
        ++timer->event.expirations;
    }
    if (timer->period)
    {
        timer->due += timer->period;
    }
    else
    {
        /* P16 ISR disables the counter after return, before task code can run. */
        timer->armed = false;
    }
}

BspStatus bsp_timer_start(BspTimer id, uint32_t first_ms, uint32_t period_ms, uint64_t *generation)
{
    if (!generation || !first_ms || first_ms > INT32_MAX || period_ms > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    size_t index;
    BspStatus status = binding(id, &index);
    if (status != BSP_OK)
    {
        return status;
    }
    if (first_ms > ecw_timer_max_period_ms() || period_ms > ecw_timer_max_period_ms() ||
        (period_ms && period_ms != first_ms))
    {
        /* No verified preload for unequal first/period: no callback rearm substitute. */
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = ecw_sdk_lock();
    TimerState *timer = &timers[index];
    if (timer->held)
    {
        status = timer->fault ? BSP_IO : BSP_BUSY;
        ecw_sdk_unlock(key);
        return status;
    }
    if (serial == UINT64_MAX)
    {
        ecw_sdk_unlock(key);
        return BSP_NO_RESOURCE;
    }
    int handle = ecw_timer_create(ecw_timer_board->timers[index].channel, first_ms * 1000u,
                                  period_ms != 0, expired, timer);
    if (handle < 0)
    {
        ecw_sdk_unlock(key);
        /* Fixed P16 -1 means allocation failed before callback installation. */
        return handle == -1 ? BSP_NO_RESOURCE : BSP_IO;
    }
    memset(timer, 0, sizeof(*timer));
    timer->held = true;
    timer->armed = true;
    timer->handle = handle;
    timer->period = period_ms;
    timer->due = ecw_sdk_time_ms() + first_ms;
    timer->generation = ++serial;
    if (ecw_timer_start_native(handle) != 0)
    {
        timer->fault = true;
        timer->armed = false;
        ecw_sdk_unlock(key);
        return BSP_IO;
    }
    *generation = timer->generation;
    ecw_sdk_unlock(key);
    return BSP_OK;
}

BspStatus bsp_timer_poll(BspTimer id, BspTimerEvent *out, bool *present)
{
    if (!out || !present)
    {
        return BSP_INVALID_ARG;
    }
    size_t index;
    BspStatus status = binding(id, &index);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = ecw_sdk_lock();
    TimerState *timer = &timers[index];
    if (!timer->held)
    {
        status = BSP_NOT_READY;
    }
    else if (timer->fault)
    {
        status = BSP_IO;
    }
    else
    {
        *present = timer->pending;
        if (timer->pending)
        {
            *out = timer->event;
            timer->pending = false;
        }
    }
    ecw_sdk_unlock(key);
    return status;
}

BspStatus bsp_timer_stop(BspTimer id, uint32_t cleanup_timeout_ms)
{
    if (cleanup_timeout_ms > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    size_t index;
    BspStatus status = binding(id, &index);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = ecw_sdk_lock();
    TimerState *timer = &timers[index];
    if (timer->held)
    {
        timer->armed = false;
        if (ecw_timer_retire(timer->handle) != 0)
        {
            timer->fault = true;
            status = BSP_IO;
        }
        else
        {
            memset(timer, 0, sizeof(*timer));
        }
    }
    /* Reviewed P16 lifecycle has no waits/loops; zero wait budget is valid.
     * Error retains ownership. No caller buffer/callback is ever borrowed. */
    ecw_sdk_unlock(key);
    return status;
}
