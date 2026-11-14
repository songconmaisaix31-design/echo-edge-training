#include "bsp_ecw6700_system.h"
#include "ecw_clock_profile.h"
#include "rtc.h"
#ifdef CONFIG_PSM_SURPORT
#include "psm_system.h"
#endif
static BspStatus context(void)
{
    return bsp_delay_ms(0);
}
BspStatus bsp_ecw6700_clock_query(Ecw6700ClockState *out)
{
    if (!out) { return BSP_INVALID_ARG; }
    BspStatus e = context();
    if (e != BSP_OK) { return e; }
    unsigned mhz = chip_cpu_clk_get_MHz();
    if (!mhz || mhz > UINT32_MAX / 1000000u) { return BSP_IO; }
    *out = (Ecw6700ClockState){mhz * 1000000u, CHIP_CLOCK_AHB, CHIP_CLOCK_APB,
                              mhz == CHIP_CLOCK_CPU / 1000000u};
    return BSP_OK;
}
BspStatus bsp_ecw6700_rtc_read(Ecw6700RtcTime *out)
{
    if (!out) { return BSP_INVALID_ARG; }
    BspStatus e = context();
    if (e != BSP_OK) { return e; }
#ifdef CONFIG_RTC
    TIME_RTC value = {0};
    drv_rtc_get_rtctime(&value);
    if (value.hour >= 24u || value.min >= 60u || value.sec >= 60u || value.ms >= 1000u)
    { return BSP_IO; }
    *out = (Ecw6700RtcTime){value.hour, value.min, value.sec, value.ms, drv_rtc_get_32K_cnt()};
    return BSP_OK;
#else
    return BSP_UNSUPPORTED;
#endif
}
BspStatus bsp_ecw6700_rtc_write_utc(int64_t seconds)
{
    if (seconds < 0 || seconds > 4102444799LL) { return BSP_INVALID_ARG; }
    BspStatus e = context();
    if (e != BSP_OK) { return e; }
#ifdef CONFIG_RTC
    time_t native = (time_t)seconds;
    if ((int64_t)native != seconds) { return BSP_UNSUPPORTED; }
    return drv_rtc_set_utctime(native) == 0 ? BSP_OK : BSP_IO;
#else
    return BSP_UNSUPPORTED;
#endif
}
BspStatus bsp_ecw6700_power_query(int *out)
{
    if (!out) { return BSP_INVALID_ARG; }
    BspStatus e = context();
    if (e != BSP_OK) { return e; }
#ifdef CONFIG_PSM_SURPORT
    int value = psm_get_sleep_mode();
    if (value < 0) { return BSP_IO; }
    *out = value;
    return BSP_OK;
#else
    return BSP_UNSUPPORTED;
#endif
}
