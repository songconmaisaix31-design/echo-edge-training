/* MOCE-SYNC: explicit timer DMA GPIO | ../TIMER_DMA_GPIO.md */
#ifndef BSP_TIMER_DMA_GPIO_H
#define BSP_TIMER_DMA_GPIO_H
#include "bsp_timer.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    BspTimer timer;
    BspPort gpio;
    uint16_t pin_mask;
    uint32_t tick_hz;
    uint32_t period_ticks;
    /* SRAM words: low16=set, high16=reset; immutable until stop succeeds. */
    const uint32_t *sequence;
    size_t elements;
    bool circular;
} BspTimerDmaGpioConfig;
typedef struct {
    bool active, complete, failed;
    uint32_t halves, cycles;
    size_t remaining;
} BspTimerDmaGpioState;
BspStatus bsp_timer_dma_gpio_start(const BspTimerDmaGpioConfig *config);
BspStatus bsp_timer_dma_gpio_query(BspTimerDmaGpioState *state);
BspStatus bsp_timer_dma_gpio_stop(void);
#ifdef __cplusplus
}
#endif
#endif
