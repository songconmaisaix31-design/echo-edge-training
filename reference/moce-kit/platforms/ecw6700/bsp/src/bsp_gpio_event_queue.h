#ifndef BSP_GPIO_EVENT_QUEUE_H
#define BSP_GPIO_EVENT_QUEUE_H
#include "bsp_gpio_event_storage.h"
BspStatus bsp_gpio_queue_start(BspGpioEventQueue *q, uint64_t *serial, uint64_t *generation);
void bsp_gpio_queue_push(BspGpioEventQueue *q, uint64_t generation, uint32_t ms, bool high);
BspStatus bsp_gpio_queue_read(BspGpioEventQueue *q, BspGpioEvent *out, size_t capacity,
                              size_t *count);
BspStatus bsp_gpio_queue_query(const BspGpioEventQueue *q, BspGpioEventState *out);
/* pause prevents event publication while hardware stop proceeds; does not free ownership. */
void bsp_gpio_queue_pause(BspGpioEventQueue *q);
/* Only call after hardware IRQ/callback retirement is proven; failed stop keeps held. */
void bsp_gpio_queue_retire(BspGpioEventQueue *q);
#endif
