#ifndef ECW_GPIO_IRQ_H
#define ECW_GPIO_IRQ_H
#include <stdint.h>
/* Private P16 IRQ adapter. Called under ecw_sdk_lock by a task owner; no pinmux
 * changes. Callback+context static until stop succeeds. Pin already BSP-owned input.
 * SDK GPIO allocator and global vector must have a single composition owner. */
int ecw_gpio_irq_start(unsigned pin, uint32_t edges, void (*callback)(void *), void *context);
int ecw_gpio_irq_stop(unsigned pin);
#endif
