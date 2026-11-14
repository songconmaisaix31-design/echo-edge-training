#include "ecw_gpio_irq.h"
#include "gpio.h"
#include "gpio_common.h"
/* P16 gpio.c copies callback contents; NULL pointer does NOT unregister.
 * Non-NULL empty struct replaces both pointer fields without freeing SDK storage.
 * Single-core caller masks IRQ through SDK; no callback can be in flight there.
 * SDK per-pin callback allocation is retained and reused, not a per-start leak. */
int ecw_gpio_irq_stop(unsigned pin)
{
    if (pin > 24)
    {
        return -1;
    }
    int r = drv_gpio_set_intr_en(pin, 0);
    if (r)
    {
        return r;
    }
    T_GPIO_ISR_CALLBACK empty = {0};
    r = drv_gpio_isr_register(pin, &empty);
    if (r)
    {
        return r;
    }
    r = drv_gpio_set_intr_mode(pin, DRV_GPIO_ARG_INTR_MODE_NOP);
    if (r)
    {
        return r;
    }
    drv_gpio_reg_write_IntrStatus(1u << pin);
    return 0;
}
int ecw_gpio_irq_start(unsigned pin, uint32_t edges, void (*callback)(void *), void *context)
{
    if (pin > 24 || !callback || !edges || (edges & ~3u))
    {
        return -1;
    }
    int r = drv_gpio_set_intr_en(pin, 0);
    if (r)
    {
        return r;
    }
    /* Remove pending old-generation edge before installing/enabling new generation. */
    drv_gpio_reg_write_IntrStatus(1u << pin);
    T_GPIO_ISR_CALLBACK cb = {.gpio_callback = callback, .gpio_data = context};
    r = drv_gpio_isr_register(pin, &cb);
    if (r)
    {
        return r;
    }
    unsigned char mode = edges == 3   ? DRV_GPIO_ARG_INTR_MODE_DUAL_EDGE
                         : edges == 1 ? DRV_GPIO_ARG_INTR_MODE_P_EDGE
                                      : DRV_GPIO_ARG_INTR_MODE_N_EDGE;
    r = drv_gpio_set_intr_mode(pin, mode);
    if (r)
    {
        return r;
    }
    return drv_gpio_set_intr_en(pin, 1);
}
