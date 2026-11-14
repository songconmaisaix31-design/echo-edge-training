/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_PWM_H
#define BSP_PWM_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Fixed board timer channel index; duty 0..10000 basis points. All channels share period. */
    BspStatus bsp_pwm_set(BspPort id, size_t channel_index, uint16_t duty_bp);
    BspStatus bsp_pwm_stop(BspPort id, size_t channel_index);
#ifdef __cplusplus
}
#endif
#endif
