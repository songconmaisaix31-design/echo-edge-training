#ifndef BSP_GPIO_EVENT_STORAGE_H
#define BSP_GPIO_EVENT_STORAGE_H
#include "bsp_gpio_event.h"
#define BSP_GPIO_EVENT_DEPTH 16u
/* Private state. Platform serializes EVERY access against IRQ, including start/
 * stop/query. ISR receives immutable generation captured when handler installed;
 * do not substitute current generation for a delayed old interrupt callback.
 * State/context remains allocated until hardware retirement is confirmed. */
typedef struct
{
    BspGpioEvent items[BSP_GPIO_EVENT_DEPTH];
    uint32_t read_index, count, dropped;
    uint64_t generation;
    bool active, held;
} BspGpioEventQueue;
#endif
