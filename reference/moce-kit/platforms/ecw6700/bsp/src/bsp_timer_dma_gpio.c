/* PIT and memory DMA are separately adapted for ECW6700.  B02P12 does not
 * currently provide reviewed evidence for a generic PIT→GPIO-register DMA
 * request route, so this optional composition is explicitly unsupported. */
#include "bsp_timer_dma_gpio.h"

BspStatus bsp_timer_dma_gpio_start(const BspTimerDmaGpioConfig *config)
{
    (void)config;
    return BSP_UNSUPPORTED;
}
BspStatus bsp_timer_dma_gpio_query(BspTimerDmaGpioState *state)
{
    (void)state;
    return BSP_UNSUPPORTED;
}
BspStatus bsp_timer_dma_gpio_stop(void)
{
    return BSP_UNSUPPORTED;
}
