#include "ecw_sdk.h"
#include "gpio.h"
#include "chip_pinmux.h"
#include "oshal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "ecw_clock_profile.h"
extern volatile uint32_t ulCriticalNesting;
_Static_assert(configTICK_RATE_HZ == 500, "P16 adapter requires reviewed 500 Hz timebase");
bool ecw_sdk_clock_ready(void)
{
    /* SDK reads the clock mux and divider. This verifies configuration, not
     * oscillator accuracy, external waveforms, or opaque radio library timing. */
    return ecw_clock_profile_ready();
}
uintptr_t ecw_sdk_lock(void)
{
    return system_irq_save();
}
void ecw_sdk_unlock(uintptr_t key)
{
    system_irq_restore((unsigned int)key);
}
bool ecw_sdk_in_isr(void)
{
    return arch_irq_context() != 0;
}
bool ecw_sdk_thread_ready(void)
{
    return !arch_irq_context() && ulCriticalNesting == 0 &&
           os_get_scheduler_run_state() == taskSCHEDULER_RUNNING;
}
uint32_t ecw_sdk_time_ms(void)
{
    return (uint32_t)os_time_get();
}
int ecw_sdk_delay_ms(uint32_t ms)
{
    /* Add one tick so tick phase/truncation cannot produce an undersized wait. */
    return os_msleep((size_t)ms + 2u);
}
int ecw_sdk_gpio_open(unsigned pin, bool output, bool high)
{
    return ecw_sdk_gpio_open_pull(pin, output, high, ECW_GPIO_PULL_NONE);
}
int ecw_sdk_gpio_open_pull(unsigned pin, bool output, bool high, unsigned pull)
{
    if (pin > 24u || pull > ECW_GPIO_PULL_DOWN)
    {
        return -2;
    }
    unsigned char native_pull = pull == ECW_GPIO_PULL_UP ? DRV_GPIO_ARG_PULL_UP
                                : pull == ECW_GPIO_PULL_DOWN ? DRV_GPIO_ARG_PULL_DOWN
                                                           : DRV_GPIO_ARG_PULL_CLOSE;
    T_GPIO_CONFIG c = {.num = (unsigned char)pin,
                       .dir = DRV_GPIO_ARG_DIR_IN,
                       .intr_type = DRV_GPIO_ARG_INTR_MODE_NOP,
                       .pull_type = native_pull};
    int r = drv_gpio_set_intr_en(pin, 0);
    if (r)
    {
        return r;
    }
    r = drv_gpio_open(&c);
    if (r)
    {
        return r;
    }
    if (output)
    {
        r = drv_gpio_write(pin, high ? 1u : 0u);
        if (r)
        {
            return r;
        }
    }
    chip_io_mode_pinmux_cfg((int)pin, FUNC_GPIO);
    return output ? drv_gpio_set_dir(pin, DRV_GPIO_ARG_DIR_OUT) : 0;
}
int ecw_sdk_gpio_read(unsigned pin, unsigned *v)
{
    return drv_gpio_read(pin, v);
}
int ecw_sdk_gpio_write(unsigned pin, bool high)
{
    return drv_gpio_write(pin, high ? 1u : 0u);
}
int ecw_sdk_gpio_close(unsigned pin)
{
    int r = drv_gpio_set_intr_en(pin, 0);
    if (r)
    {
        return r;
    }
    r = drv_gpio_set_dir(pin, DRV_GPIO_ARG_DIR_IN);
    if (r)
    {
        return r;
    }
    return drv_gpio_set_pull_type(pin, DRV_GPIO_ARG_PULL_CLOSE);
}
