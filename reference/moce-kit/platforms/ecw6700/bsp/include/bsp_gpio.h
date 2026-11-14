/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_GPIO_H
#define BSP_GPIO_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    BspStatus bsp_gpio_write(BspPort id, bool high);
    BspStatus bsp_gpio_read(BspPort id, bool *high);
#ifdef __cplusplus
}
#endif
#endif
