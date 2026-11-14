/* Owned P16 CPU backend. Does not call/modify the vendor spi_master driver.
 * Integration MUST exclude other SPI1 owners and dynamic clocks/sleep.
 * Register definitions and reset sequence: P16 spi.h/spi_master.c.
 */
#include "chip_pinmux.h"
#include "chip_smuaon.h"
#include "ecw_clock_profile.h"
#include "ecw_sdk.h"
#include "ecw_spi_registers.h"
_Static_assert(sizeof(unsigned int) == 4, "P16 register width");
_Static_assert(offsetof(spi_reg, mux_config) == 0x78, "P16 register layout");
_Static_assert(offsetof(spi_reg, status) == 0x34 && offsetof(spi_reg, intrSt) == 0x3c,
               "P16 polling offsets");
_Static_assert(SPI_STATUS_BUSY == 1 && SPI_STATUS_TXFULL == (1u << 23) &&
                   SPI_STATUS_RXNUM == (31u << 8) && SPI_STATUS_TXNUM == (31u << 16),
               "P16 status bits");
_Static_assert(SPI_INTR_STS_END == (1u << 4) && SPI_CONTROL_SPIRST == 1,
               "P16 completion/reset bits");
_Static_assert(SPI_TRANSCTRL_TRAMODE_WO == (1u << 24) && SPI_TRANSCTRL_TRAMODE_WRCON == 0,
               "P16 transfer modes");
static bool opened, clocked;
static spi_reg *regs(void)
{
    return (spi_reg *)MEM_BASE_SPI1;
}
static uint32_t poll_time(void *ignored)
{
    (void)ignored;
    return ecw_sdk_time_ms();
}
static bool reset_done(void)
{
    uint32_t start = ecw_sdk_time_ms();
    while (regs()->ctrl & (SPI_CONTROL_SPIRST | SPI_CONTROL_RXFIFORST | SPI_CONTROL_TXFIFORST))
    {
        if ((uint32_t)(ecw_sdk_time_ms() - start) >= 10u)
        {
            return false;
        }
    }
    return !(regs()->status & SPI_STATUS_BUSY);
}
int ecw_sdk_spi_open(unsigned instance, const uint8_t *pins, uint32_t rate)
{
    const Ecw6700PortConfig config = {.rate_hz = rate};
    return ecw_sdk_spi_open_config(instance, pins, &config);
}
int ecw_sdk_spi_open_config(unsigned instance, const uint8_t *pins,
                            const Ecw6700PortConfig *config)
{
    uint32_t rate = config->rate_hz;
    if (config->spi_mode > 3 || config->spi_bit_order > 1)
        return -2;
    /* Preflight has no hardware writes. All pin choice is from fixed Board. */
    if (instance || !pins || !rate || opened)
    {
        return -1;
    }
    if (pins[0] != 14 || pins[1] != 12 || pins[2] != 13)
    {
        return -2;
    }
    uint32_t half = ECW_SPI_INPUT_HZ;
    if (rate > half || half % rate || half / rate > 255u)
    {
        return -2;
    }
    if (!ecw_clock_profile_ready())
    {
        return -3;
    }
    chip_clk_enable(CLK_SPI);
    clocked = true;
    /* SDK's default pinmux would drive GPIO13 as CS; never call it. */
    uintptr_t key = ecw_sdk_lock();
    WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0,
              READ_REG(CHIP_SMU_PD_SOFTWARE_RESET0) & ~DRV_SPI1_RESET_ON);
    WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0,
              READ_REG(CHIP_SMU_PD_SOFTWARE_RESET0) | DRV_SPI1_RESET_ON);
    WRITE_REG(MEM_BASE_SMU_PD + 0x18, READ_REG(MEM_BASE_SMU_PD + 0x18) | (1u << 4));
    ecw_sdk_unlock(key);
    regs()->intrEn = 0;
    regs()->ctrl = SPI_CONTROL_SPIRST;
    if (!reset_done())
    {
        return -4;
    }
    regs()->transFmt = SPI_FORMAT_DATA_LEN(7) | SPI_FORMAT_DATA_MERGE(1) |
                       ((config->spi_mode & 2) ? SPI_FORMAT_CPOL_HIGH : SPI_FORMAT_CPOL_LOW) |
                       ((config->spi_mode & 1) ? SPI_FORMAT_CPA_EVEN : SPI_FORMAT_CPHA_ODD) |
                       (config->spi_bit_order ? SPI_FORMAT_LSB : SPI_FORMAT_MSB);
    regs()->timing = (regs()->timing & SPI_INTERFACE_TIMING_MUSK) | (half / rate - 1u);
    regs()->transCtrl = 0;
    regs()->mux_config = 0;
    regs()->intrSt = 0xffffffffu;
    key = ecw_sdk_lock();
    /* Runtime baseline excludes GPIO wake and low-power pin remux. */
    chip_io_mode_pinmux_cfg(pins[1], FUNC_SPI1_MISO);
    chip_io_mode_pinmux_cfg(pins[2], FUNC_SPI1_MOSI);
    chip_io_mode_pinmux_cfg(pins[0], FUNC_SPI1_CLK);
    ecw_sdk_unlock(key);
    opened = true;
    return 0;
}
int ecw_sdk_spi_close(unsigned instance, const uint8_t *pins)
{
    if (instance || !pins)
    {
        return -1;
    }
    if (!clocked)
    {
        return 0;
    }
    regs()->intrEn = 0;
    regs()->ctrl = SPI_CONTROL_SPIRST;
    if (!reset_done())
    {
        return -4;
    }
    /* Only detach owned pins, never claim/control a CS pin. */
    for (size_t i = 0; i < 3; ++i)
    {
        uintptr_t key = ecw_sdk_lock();
        int rc = ecw_sdk_gpio_open(pins[i], false, false);
        ecw_sdk_unlock(key);
        if (rc)
        {
            return rc;
        }
    }
    chip_clk_disable(CLK_SPI);
    opened = false;
    clocked = false;
    return 0;
}
EcwSpiPollResult ecw_sdk_spi_transfer(unsigned instance, const uint8_t *tx, uint8_t *rx, size_t n,
                                      uint32_t timeout_ms)
{
    if (instance || !opened)
    {
        return (EcwSpiPollResult){BSP_NOT_READY, 0, false};
    }
    if (!ecw_clock_profile_ready())
    {
        return (EcwSpiPollResult){BSP_IO, 3, true};
    }
    EcwSpiPollIo io = {regs(), ecw_spi_mmio_read, ecw_spi_mmio_write, poll_time};
    return ecw_spi_poll_transfer(&io, tx, rx, n, timeout_ms);
}
