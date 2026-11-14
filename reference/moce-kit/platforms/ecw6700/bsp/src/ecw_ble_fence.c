#include "ecw_ble_fence.h"
#include "bsp_request_internal.h"
#include "nimble/nimble_port.h"

static void reached_marker(struct ble_npl_event *event)
{
    EcwBleFence *fence = ble_npl_event_get_arg(event);
    uintptr_t key = bsp_request_lock();
    if (fence && fence->held && event == &fence->marker)
    {
        fence->reached = true;
    }
    bsp_request_unlock(key);
}

BspStatus ecw_ble_fence_prepare(EcwBleFence *fence)
{
    if (!fence)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    bool held = fence->held;
    bool prepared = fence->prepared;
    bsp_request_unlock(key);
    if (held)
    {
        return BSP_BUSY;
    }
    if (prepared)
    {
        return BSP_OK;
    }
    struct ble_npl_eventq *queue = nimble_port_get_dflt_eventq();
    if (!queue || !queue->q)
    {
        return BSP_NOT_READY;
    }
    ble_npl_event_init(&fence->marker, reached_marker, fence);
    fence->queue = queue;
    fence->prepared = true;
    return BSP_OK;
}

BspStatus ecw_ble_fence_post(EcwBleFence *fence, uint64_t operation)
{
    if (!fence || !operation)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus status = !fence->prepared                     ? BSP_NOT_READY
                       : fence->held                        ? BSP_BUSY
                       : operation <= fence->last_operation ? BSP_INVALID_STATE
                                                            : BSP_OK;
    if (status != BSP_OK)
    {
        bsp_request_unlock(key);
        return status;
    }
    fence->held = true;
    fence->posting = true;
    fence->reached = false;
    fence->operation = operation;
    fence->last_operation = operation;
    bsp_request_unlock(key);

    /* Real P16 NPL sets queued before sending; do not repost a live marker or
     * erase it merely because the consumer ran before this call returned. */
    ble_npl_eventq_put(fence->queue, &fence->marker);

    key = bsp_request_lock();
    fence->posting = false;
    bsp_request_unlock(key);
    return BSP_OK;
}

BspStatus ecw_ble_fence_query(EcwBleFence *fence, uint64_t operation, bool *reached)
{
    if (!fence || !operation || !reached)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus status = !fence->prepared                                ? BSP_NOT_READY
                       : !fence->held || operation != fence->operation ? BSP_INVALID_STATE
                                                                       : BSP_OK;
    if (status == BSP_OK)
    {
        *reached = fence->reached && !fence->posting;
    }
    bsp_request_unlock(key);
    return status;
}

BspStatus ecw_ble_fence_release(EcwBleFence *fence, uint64_t operation)
{
    if (!fence || !operation)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus status = !fence->prepared                                ? BSP_NOT_READY
                       : !fence->held || operation != fence->operation ? BSP_INVALID_STATE
                       : fence->posting || !fence->reached             ? BSP_BUSY
                                                                       : BSP_OK;
    if (status == BSP_OK)
    {
        fence->held = false;
    }
    bsp_request_unlock(key);
    return status;
}
