/* One Board-owned CCU6 counter, two independently controlled PWM outputs.
 * No direct register access, SDK modification, PIT occupation or software PWM.
 * SDK applications/CLI must not use CCU6 concurrently with this binding. */
#include "ecw_pwm_ccu6.h"
#if CONFIG_CCU6
#include "ccu6.h"
#include "ecw_clock_profile.h"
#include "chip_pinmux.h"
#if CONFIG_PSM_SURPORT
#include "pm_core.h"
#endif
#ifndef BSP_ECW_PWM_DIAGNOSTICS
#define BSP_ECW_PWM_DIAGNOSTICS 0
#endif
#define ECW_CCU6_CLOCK_HZ ECW_CCU6_INPUT_HZ
static bool owned;
static uint32_t period;
static const Ecw6700Port *owner;
static bool active[2];

static void power_acquire(void)
{
#if CONFIG_PSM_SURPORT
    pm_lock_acquire(PM_LOCK_ACTIVE, "bsp_ccu6");
#endif
}

static void power_release(void)
{
#if CONFIG_PSM_SURPORT
    pm_lock_release(PM_LOCK_ACTIVE, "bsp_ccu6");
#endif
}

static void trace(const char *stage, int result)
{
#if BSP_ECW_PWM_DIAGNOSTICS
    T_DRV_CCU6_CFG actual = {0};
    drv_ccu6_info(&actual);
    /* Task context, outside the IRQ lock. A count snapshot is not proof of
     * pin output. No polling, hidden retry or delay is added to PWM service. */
    os_printf(LL_INFO,
              "BSP_CCU6 stage=%s rc=%d cpu_mhz=%u period=%u clk=%u div=%u "
              "mode=%u trigger=%u one_shot=%u count=%u active=%u,%u\n",
              stage, result, chip_cpu_clk_get_MHz(), actual.period_ticks, actual.clk_sel,
              actual.div_clk, actual.cnt_mode, actual.trigger_sel, actual.one_shot_en,
              drv_ccu6_get_current_count(), (unsigned)active[0], (unsigned)active[1]);
#else
    (void)stage;
    (void)result;
#endif
}

bool ecw_ccu6_available(void)
{
    return ecw_clock_profile_ready();
}

int ecw_ccu6_open(const Ecw6700Port *port, uint32_t hz)
{
    if (owned || !port || port->pin_count != 2 || (port->pins[0] != 19 && port->pins[0] != 15) || port->pins[1] != 16 ||
        !hz || hz > 20000000u || !ecw_ccu6_available())
    {
        return -1;
    }
    /* CCU6 has a 32-bit period, unlike PIT's two 16-bit PWM phases.
     * 40 MHz / 50 Hz = 800000 ticks. No global clock change is required. */
    T_DRV_CCU6_CFG config = {.trigger_sel = DRV_CCU6_RUN_TRIGGER_BY_REG,
                             .cnt_mode = DRV_CCU6_CNT_MODE_EDGE_ALIGNED,
                             .one_shot_en = DRV_CCU6_ONE_SHOT_DIS,
                             .period_ticks = ECW_CCU6_CLOCK_HZ / hz,
                             .clk_sel = DRV_CCU6_SEL_PCLK_40MHz,
                             .div_clk = DRV_CCU6_DIV_DIV0};
    /* Keep configuration, static endpoint levels and running PWM valid until
     * deinit. Forced sleep/manual clock changes remain outside this ownership.
     * P16 startup must initialize native PM before the BSP application task. */
    power_acquire();
    if (!ecw_ccu6_available())
    {
        power_release();
        return -2;
    }
    drv_ccu6_init();
    int result = drv_ccu6_cfg(&config); /* Also rejects failed native allocation/running counter. */
    if (result)
    {
        trace("configure", result);
        power_release();
        return result; /* Do not stop or reconfigure a counter we did not acquire. */
    }
    owned = true;
    owner = port;
    period = config.period_ticks;
    T_DRV_CCU6_CFG actual = {0};
    drv_ccu6_info(&actual);
    if (actual.period_ticks != period || actual.clk_sel != config.clk_sel ||
        actual.cnt_mode != config.cnt_mode || actual.trigger_sel != config.trigger_sel ||
        actual.one_shot_en != config.one_shot_en)
    {
        trace("config-readback", ECW_CCU6_CONFIG_MISMATCH);
        return ECW_CCU6_CONFIG_MISMATCH; /* Retain ownership/PM lock until explicit cleanup. */
    }
    for (unsigned channel = 0; channel < 2; ++channel)
    {
        active[channel] = false;
        T_DRV_CCU6_CFG_PWM pwm = {.dead_time_en = 0,
                                  .defautl_level = DRV_CCU6_PWM_DEF_LVL_LOW,
                                  .chn_mutual_exclusion = DRV_CCU6_PWM_CHN_SYC};
        result = drv_ccu6_chn_mode(channel, DRV_CCU6_MODE_BASE);
        if (!result)
        {
            result = drv_ccu6_pwm_set_cfg(channel, &pwm);
        }
        if (!result)
        {
            result = ecw_sdk_gpio_open(port->pins[channel], true, false);
        }
        if (result)
        {
            trace("open-output", result);
            return result; /* Port stays fault-held; explicit close cleans owned resources. */
        }
    }
    trace("open", 0);
    return 0;
}

EcwIoResult ecw_ccu6_set(const Ecw6700Port *port, size_t channel, uint16_t duty, bool stop)
{
    if (!owned || owner != port)
    {
        return (EcwIoResult){BSP_NOT_READY, 0, false};
    }
    if (channel >= 2 || duty > 10000)
    {
        return (EcwIoResult){BSP_INVALID_ARG, 0, false};
    }
    if (!ecw_ccu6_available())
    {
        return (EcwIoResult){BSP_IO, (uint32_t)-2, true};
    }
    uint32_t high = (uint32_t)((uint64_t)period * duty / 10000u);
    if (!stop && duty && duty < 10000 && (!high || high >= period))
    {
        return (EcwIoResult){BSP_UNSUPPORTED, 0, false};
    }
    uintptr_t key = ecw_sdk_lock();
    int result;
    if (stop || !duty || duty == 10000)
    {
        result = drv_ccu6_chn_mode((unsigned)channel, DRV_CCU6_MODE_BASE);
        if (!result)
        {
            result = ecw_sdk_gpio_open(port->pins[channel], true, !stop && duty == 10000);
        }
        if (!result)
        {
            active[channel] = false;
        }
    }
    else
    {
        /* Native edge-aligned default-low output rises at compare, falls at
         * period. Compare is LOW duration, not requested HIGH duration. */
        result = drv_ccu6_pwm_set_cmp_val((unsigned)channel, period - high);
        if (!result)
        {
            result = drv_ccu6_chn_mode((unsigned)channel, DRV_CCU6_MODE_PWM);
        }
        if (!result)
        {
            result = chip_ccu6_pinmux_cfg(channel ? CCU6_IO_TYPE_PWM6 : CCU6_IO_TYPE_PWM4,
                                          port->pins[channel]);
        }
        if (!result)
        {
            active[channel] = true;
        }
    }
    if (!result)
    {
        result = drv_ccu6_run(active[0] || active[1]);
    }
    ecw_sdk_unlock(key);
    trace(stop ? "stop" : "set", result);
    return (EcwIoResult){result ? BSP_IO : BSP_OK, (uint32_t)result, result != 0};
}

int ecw_ccu6_close(const Ecw6700Port *port)
{
    if (!owned)
    {
        return 0;
    }
    if (owner != port)
    {
        return -1;
    }
    uintptr_t key = ecw_sdk_lock();
    int result = drv_ccu6_run(0);
    for (unsigned channel = 0; !result && channel < 2; ++channel)
    {
        result = drv_ccu6_chn_mode(channel, DRV_CCU6_MODE_BASE);
        if (!result)
        {
            result = ecw_sdk_gpio_close(port->pins[channel]);
        }
    }
    if (!result)
    {
        owned = false;
        owner = NULL;
        active[0] = active[1] = false;
    }
    ecw_sdk_unlock(key);
    trace("close", result);
    if (!result)
    {
        power_release();
    }
    /* SDK has no CCU6 deinit; its static context/clock remain SDK-owned. */
    return result;
}
#else
bool ecw_ccu6_available(void)
{
    return false;
}
int ecw_ccu6_open(const Ecw6700Port *p, uint32_t hz)
{
    (void)p;
    (void)hz;
    return -1;
}
int ecw_ccu6_close(const Ecw6700Port *p)
{
    (void)p;
    return 0;
}
EcwIoResult ecw_ccu6_set(const Ecw6700Port *p, size_t c, uint16_t d, bool s)
{
    (void)p;
    (void)c;
    (void)d;
    (void)s;
    return (EcwIoResult){BSP_UNSUPPORTED, 0, false};
}
#endif
