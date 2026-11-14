#include "ecw_ble_pump.h"
#include "bsp_request_internal.h"
#include "nimble/nimble_port.h"
#include <string.h>

static void exit_marker(struct ble_npl_event *event)
{
    (void)event;
}
BspStatus ecw_ble_pump_prepare(EcwBlePump *p)
{
    if (!p)
    {
        return BSP_INVALID_ARG;
    }
    /* Owner serializes prepare against run/exit; no SDK call under BSP lock. */
    uintptr_t key = bsp_request_lock();
    bool busy = p->prepared && (!p->returned || p->running || p->posting);
    bsp_request_unlock(key);
    if (busy)
    {
        return BSP_BUSY;
    }
    struct ble_npl_eventq *q = nimble_port_get_dflt_eventq();
    /* P16 public NPL queue handle; no register or internal Host state access. */
    if (!q || !q->q)
    {
        return BSP_NOT_READY;
    }
    memset(p, 0, sizeof(*p));
    p->queue = q;
    ble_npl_event_init(&p->exit_event, exit_marker, NULL);
    p->prepared = true;
    return BSP_OK;
}
static BspStatus finish(EcwBlePump *p, BspStatus result)
{
    uintptr_t key = bsp_request_lock();
    p->result = result;
    p->running = false;
    p->returned = true;
    bsp_request_unlock(key);
    return result; /* No SDK or pump access after publication. */
}
BspStatus ecw_ble_pump_run(EcwBlePump *p)
{
    if (!p)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (!p->prepared || p->running || p->returned)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    p->running = true;
    bsp_request_unlock(key);
    for (;;)
    {
        struct ble_npl_event *event = ble_npl_eventq_get(p->queue, BLE_NPL_TIME_FOREVER);
        if (!event)
        {
            return finish(p, BSP_IO);
        }
        ble_npl_event_run(event);
        if (event == &p->exit_event)
        {
            return finish(p, BSP_OK);
        }
        /* Preserve P16 native Host scheduling behavior for ordinary events. */
        ble_npl_hs_delay();
    }
}
BspStatus ecw_ble_pump_request_exit(EcwBlePump *p)
{
    if (!p)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (!p->prepared || !p->running || p->returned)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    if (p->requested)
    {
        BspStatus result = p->posting ? BSP_BUSY : BSP_OK;
        bsp_request_unlock(key);
        return result;
    }
    p->requested = true;
    p->posting = true;
    bsp_request_unlock(key);
    ble_npl_eventq_put(p->queue, &p->exit_event);
    key = bsp_request_lock();
    p->posting = false;
    bsp_request_unlock(key);
    return BSP_OK;
}
bool ecw_ble_pump_quiet(EcwBlePump *p, BspStatus *result)
{
    if (!p || !result)
    {
        return false;
    }
    uintptr_t key = bsp_request_lock();
    bool quiet = p->prepared && p->returned && !p->running && !p->posting;
    if (quiet)
    {
        *result = p->result;
    }
    bsp_request_unlock(key);
    return quiet;
}
