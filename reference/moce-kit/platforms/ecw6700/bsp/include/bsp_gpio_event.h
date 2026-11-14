/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_GPIO_EVENT_H
#define BSP_GPIO_EVENT_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        BSP_EDGE_RISING = 1u,
        BSP_EDGE_FALLING = 2u
    };
    typedef struct
    {
        uint32_t edges, timestamp_resolution_us;
        size_t queue_capacity;
    } BspGpioEventCapabilities;
    typedef struct
    {
        uint64_t generation;
        uint32_t timestamp_ms;
        bool high;
    } BspGpioEvent;
    typedef struct
    {
        bool active;
        uint32_t queued, dropped;
        uint64_t generation;
    } BspGpioEventState;
    BspStatus bsp_gpio_event_capabilities(BspPort port, BspGpioEventCapabilities *out);
    /* Input Board port only, nonzero edge mask. Does not change pin direction or bind
 * arbitrary IRQ. No implicit debounce. Shared EXTI line conflict returns BUSY. */
    BspStatus bsp_gpio_event_start(BspPort port, uint32_t edges, uint64_t *generation);
    BspStatus bsp_gpio_event_read(BspPort port, BspGpioEvent *out, size_t capacity, size_t *count);
    BspStatus bsp_gpio_event_query(BspPort port, BspGpioEventState *out);
    BspStatus bsp_gpio_event_stop(BspPort port, uint32_t cleanup_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
