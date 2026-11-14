#ifndef ECW_CLOCK_PROFILE_H
#define ECW_CLOCK_PROFILE_H
#include <stdbool.h>
#include "chip_clk_ctrl.h"
/* Single reviewed startup profile. SDK generated config is the sole source.
 * Peripherals below have distinct inputs; none is inferred from a literal CPU MHz.
 * This header does not configure clocks or enable runtime frequency scaling. */
_Static_assert(CHIP_CLOCK_SOURCE == 160000000 && CHIP_CLOCK_CPU == 160000000 &&
                   CHIP_CLOCK_AHB == 80000000 && CHIP_CLOCK_APB == 40000000,
               "Unvalidated ECW startup profile; CPU80 remains a candidate");
#define ECW_SPI_INPUT_HZ (CHIP_CLOCK_AHB / 2u)
#define ECW_PIT_INPUT_HZ CHIP_CLOCK_APB
/* Dedicated SDK 20MHz I2C and explicit CCU6 PCLK40 selector, not CPU clocks. */
#define ECW_I2C_INPUT_HZ 20000000u
#define ECW_CCU6_INPUT_HZ 40000000u
static inline bool ecw_clock_profile_ready(void)
{
    return chip_cpu_clk_get_MHz() == CHIP_CLOCK_CPU / 1000000u;
}
#endif
