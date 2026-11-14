#include "bsp_request_internal.h"
#include <string.h>

static BspRequestSync runtime_sync;
static bool runtime_ready;

BspStatus bsp_request_runtime_init(const BspRequestSync *sync)
{
    if (sync == NULL || sync->enter == NULL || sync->leave == NULL)
    {
        return BSP_INVALID_ARG;
    }
    if (runtime_ready)
    {
        return BSP_INVALID_STATE;
    }
    runtime_sync = *sync;
    runtime_ready = true;
    return BSP_OK;
}

uintptr_t bsp_request_lock(void)
{
    return runtime_sync.enter(runtime_sync.context);
}

void bsp_request_unlock(uintptr_t previous)
{
    runtime_sync.leave(runtime_sync.context, previous);
}

static bool valid_notify(const BspNotify *notify)
{
    if (notify == NULL)
    {
        return false;
    }
    switch (notify->mode)
    {
    case BSP_NOTIFY_NONE:
        return notify->done == NULL;
    case BSP_NOTIFY_NATIVE:
    case BSP_NOTIFY_DISPATCH:
        return notify->done != NULL;
    default:
        return false;
    }
}

BspStatus bsp_request_init(BspRequest *r, const BspNotify *notify)
{
    uintptr_t key;
    if (r == NULL || !valid_notify(notify))
    {
        return BSP_INVALID_ARG;
    }
    if (!runtime_ready)
    {
        return BSP_NOT_READY;
    }
    key = bsp_request_lock();
    if (r->initialized)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    memset(r, 0, sizeof(*r));
    r->initialized = true;
    r->notify = *notify;
    r->snapshot.state = BSP_REQ_READY;
    r->snapshot.result.buffers_returned = true;
    bsp_request_unlock(key);
    return BSP_OK;
}

BspStatus bsp_request_query(const BspRequest *r, BspRequestSnapshot *out)
{
    uintptr_t key;
    if (r == NULL || out == NULL)
    {
        return BSP_INVALID_ARG;
    }
    if (!runtime_ready)
    {
        return BSP_NOT_READY;
    }
    key = bsp_request_lock();
    if (!r->initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    *out = r->snapshot;
    bsp_request_unlock(key);
    return BSP_OK;
}

BspStatus bsp_request_begin(BspRequest *r, void *owner, void (*retire)(void *),
                            uint64_t *generation)
{
    uintptr_t key;
    if (r == NULL || owner == NULL || retire == NULL || generation == NULL)
    {
        return BSP_INVALID_ARG;
    }
    if (!runtime_ready)
    {
        return BSP_NOT_READY;
    }
    key = bsp_request_lock();
    if (!r->initialized || r->snapshot.state != BSP_REQ_READY || r->submit_active)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    if (r->snapshot.generation == UINT64_MAX)
    {
        bsp_request_unlock(key);
        return BSP_NO_RESOURCE;
    }
    ++r->snapshot.generation;
    *generation = r->snapshot.generation;
    memset(&r->snapshot.result, 0, sizeof(r->snapshot.result));
    r->snapshot.state = BSP_REQ_RUNNING;
    r->submit_active = true;
    r->owner = owner;
    r->retire = retire;
    bsp_request_unlock(key);
    return BSP_OK;
}

void bsp_request_end_submit(BspRequest *r)
{
    uintptr_t key = bsp_request_lock();
    r->submit_active = false;
    bsp_request_unlock(key);
}

BspStatus bsp_request_reject(BspRequest *r, uint64_t generation)
{
    uintptr_t key = bsp_request_lock();
    if (r->snapshot.generation != generation || !r->submit_active ||
        r->snapshot.state != BSP_REQ_RUNNING)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    r->retire(r->owner); /* Internal metadata cleanup only, under state lock. */
    r->owner = NULL;
    r->retire = NULL;
    r->snapshot.state = BSP_REQ_READY;
    r->snapshot.result.buffers_returned = true;
    bsp_request_unlock(key);
    return BSP_OK;
}

static void invoke_callback(BspRequest *r)
{
    uintptr_t key;
    r->notify.done(r, r->notify.user); /* No internal lock held. */
    key = bsp_request_lock();
    r->callback_running = false;
    bsp_request_unlock(key);
}

BspStatus bsp_request_publish(BspRequest *r, uint64_t generation, const BspResult *result)
{
    bool notify_now = false;
    uintptr_t key;
    if (r == NULL || result == NULL)
    {
        return BSP_INVALID_ARG;
    }
    key = bsp_request_lock();
    if (!r->initialized || r->snapshot.generation != generation ||
        (r->snapshot.state != BSP_REQ_RUNNING && r->snapshot.state != BSP_REQ_FAULT_HELD))
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    r->snapshot.result = *result;
    r->snapshot.state = result->buffers_returned ? BSP_REQ_TERMINAL : BSP_REQ_FAULT_HELD;
    if (result->buffers_returned && r->notify.mode == BSP_NOTIFY_NATIVE)
    {
        r->callback_running = true;
        notify_now = true;
    }
    else if (result->buffers_returned && r->notify.mode == BSP_NOTIFY_DISPATCH)
    {
        r->callback_pending = true;
    }
    bsp_request_unlock(key);
    if (notify_now)
    {
        invoke_callback(r);
    }
    return BSP_OK;
}

BspStatus bsp_request_dispatch_one(BspRequest *r, bool *delivered)
{
    uintptr_t key;
    if (r == NULL || delivered == NULL)
    {
        return BSP_INVALID_ARG;
    }
    *delivered = false;
    if (!runtime_ready)
    {
        return BSP_NOT_READY;
    }
    key = bsp_request_lock();
    if (!r->initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (r->callback_pending && !r->callback_running)
    {
        r->callback_pending = false;
        r->callback_running = true;
        *delivered = true;
    }
    bsp_request_unlock(key);
    if (*delivered)
    {
        invoke_callback(r);
    }
    return BSP_OK;
}

BspStatus bsp_request_reset(BspRequest *r)
{
    uintptr_t key;
    if (r == NULL)
    {
        return BSP_INVALID_ARG;
    }
    if (!runtime_ready)
    {
        return BSP_NOT_READY;
    }
    key = bsp_request_lock();
    if (!r->initialized)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (r->snapshot.state != BSP_REQ_TERMINAL || !r->snapshot.result.buffers_returned ||
        r->submit_active || r->callback_pending || r->callback_running)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    r->retire(r->owner);
    r->owner = NULL;
    r->retire = NULL;
    r->snapshot.state = BSP_REQ_READY;
    bsp_request_unlock(key);
    return BSP_OK;
}

BspStatus bsp_request_cancel(BspRequest *r)
{
    BspRequestSnapshot snapshot;
    BspStatus status = bsp_request_query(r, &snapshot);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}
