#include "bsp_gpio_event_queue.h"
BspStatus bsp_gpio_queue_start(BspGpioEventQueue *q, uint64_t *serial, uint64_t *generation)
{
    if (!q || !serial || !generation)
    {
        return BSP_INVALID_ARG;
    }
    if (q->held)
    {
        return BSP_BUSY;
    }
    if (*serial == UINT64_MAX)
    {
        return BSP_NO_RESOURCE;
    }
    q->read_index = 0;
    q->count = 0;
    q->dropped = 0;
    q->generation = ++*serial;
    q->held = true;
    q->active = true;
    *generation = q->generation;
    return BSP_OK;
}
void bsp_gpio_queue_push(BspGpioEventQueue *q, uint64_t generation, uint32_t ms, bool high)
{
    if (!q || !q->active || q->generation != generation)
    {
        return;
    }
    if (q->count == BSP_GPIO_EVENT_DEPTH)
    {
        if (q->dropped != UINT32_MAX)
        {
            ++q->dropped;
        }
        return;
    }
    uint32_t w = (q->read_index + q->count) % BSP_GPIO_EVENT_DEPTH;
    q->items[w] = (BspGpioEvent){generation, ms, high};
    ++q->count;
}
BspStatus bsp_gpio_queue_read(BspGpioEventQueue *q, BspGpioEvent *out, size_t capacity,
                              size_t *count)
{
    if (!q || !out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    if (!q->held)
    {
        return BSP_NOT_READY;
    }
    size_t n = capacity < q->count ? capacity : q->count;
    for (size_t i = 0; i < n; ++i)
    {
        out[i] = q->items[q->read_index];
        q->read_index = (q->read_index + 1) % BSP_GPIO_EVENT_DEPTH;
    }
    q->count -= (uint32_t)n;
    *count = n;
    return BSP_OK;
}
BspStatus bsp_gpio_queue_query(const BspGpioEventQueue *q, BspGpioEventState *out)
{
    if (!q || !out)
    {
        return BSP_INVALID_ARG;
    }
    *out = (BspGpioEventState){q->active, q->count, q->dropped, q->generation};
    return BSP_OK;
}
void bsp_gpio_queue_pause(BspGpioEventQueue *q)
{
    if (q)
    {
        q->active = false;
    }
}
void bsp_gpio_queue_retire(BspGpioEventQueue *q)
{
    if (q)
    {
        q->active = false;
        q->held = false;
        q->count = 0;
        q->read_index = 0;
    }
}
