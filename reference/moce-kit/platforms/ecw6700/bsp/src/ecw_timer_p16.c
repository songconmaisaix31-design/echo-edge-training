#include "ecw_timer_native.h"
#include "ecw_clock_profile.h"
#include "pit_timer.h"
#include <limits.h>


uint32_t ecw_timer_max_period_ms(void)
{
    return (UINT32_MAX / (ECW_PIT_INPUT_HZ / 1000000u)) / 1000u;
}

int ecw_timer_create(unsigned channel, uint32_t period_us, bool periodic, void (*callback)(void *),
                     void *context)
{
    T_TIMER_CONFIG config = {.cb = {.timer_callback = callback, .timer_data = context},
                             .period_us = period_us,
                             .reload_cnt = periodic ? DRV_TIMER_RELOAD_CNT_FOVEVER : 1u,
                             .auto_start = 0,
                             .timer_chn = channel};
    return drv_pit_timer_create(&config);
}

int ecw_timer_start_native(int handle)
{
    return drv_pit_timer_start(handle);
}

int ecw_timer_retire(int handle)
{
    /* P16 delete does not clear callback/user_data. Retire it while IRQ is masked,
     * before freeing the shared channel. Never disable the whole PIT IRQ or clock:
     * other timer/PWM owners can still be using the peripheral. */
    T_TIMER_ISR_CALLBACK empty = {0};
    int result = drv_pit_timer_register_isr(handle, &empty);
    if (result == TIMER_RET_SUCCESS)
    {
        result = drv_pit_timer_delete(handle);
    }
    return result;
}
