/* Fixed P16 production binding; no SDK demo pinmux, no module-visible MMIO.
 * Exclude SDK UART owners/console for selected UART, dynamic clock/sleep and
 * other I2C0 users. PIT allocation is shared through SDK's channel allocator. */
#include "ecw_sdk.h"
#include "ecw_uart_format.h"
#include "ecw_uart_rx.h"
#include "ecw_pwm_ccu6.h"
#include "i2c_reg_struct.h"
#include "uart_reg_struct.h"
#include "pit_common.h"
#include "ecw_clock_profile.h"
#include "ecw_i2c_timing.h"
#include "chip_pinmux.h"
#include "chip_smupd.h"
#include "oshal.h"
_Static_assert(offsetof(T_I2C_REG_MAP, Setup) == 0x2c && offsetof(T_I2C_REG_MAP, Ctrl) == 0x24 &&
               offsetof(T_I2C_REG_MAP, TPM) == 0x30,
               "P16 I2C");
_Static_assert(I2C_FLAG_ACK == 0x400 && I2C_FLAG_Cmpl == 0x200, "P16 status");
_Static_assert(offsetof(T_UART_REG_MAP, LSR) == 0x34 && DRV_UART_LSR_TEMT == 0x40, "P16 UART");
static bool i2c_opened, uart_opened[2], pwm_owned[4];
static uint32_t pwm_rate[4];
static T_I2C_REG_MAP *i2c(void)
{
    return (T_I2C_REG_MAP *)(uintptr_t)MEM_BASE_I2C;
}
static T_UART_REG_MAP *uart(unsigned n)
{
    return (T_UART_REG_MAP *)(uintptr_t)(MEM_BASE_UART0 + (n << 12));
}
static uint32_t clock_ms(void *ctx)
{
    (void)ctx;
    return ecw_sdk_time_ms();
}
static int detach(const Ecw6700Port *p)
{
    for (size_t j = 0; j < p->pin_count; ++j)
    {
        uintptr_t k = ecw_sdk_lock();
        int r = ecw_sdk_gpio_close(p->pins[j]);
        ecw_sdk_unlock(k);
        if (r)
        {
            return r;
        }
    }
    return 0;
}
int ecw_basic_open(const Ecw6700Port *p, uint32_t rate)
{
    const Ecw6700PortConfig config = {.rate_hz = rate};
    return ecw_basic_open_config(p, &config);
}
int ecw_basic_open_config(const Ecw6700Port *p, const Ecw6700PortConfig *config)
{
    uint32_t rate = config->rate_hz, lcr = 3;
    if ((p->id >> 8) == BSP_UART && ecw_uart_format(config, &lcr) != BSP_OK)
        return -2;
    if ((p->id >> 8) == BSP_PWM && p->instance == ECW6700_PWM_CCU6)
    {
        return ecw_ccu6_open(p, rate);
    }
    if (!ecw_clock_profile_ready())
    {
        return -3;
    }
    unsigned kind = p->id >> 8;
    if (kind == BSP_I2C)
    {
        EcwI2cTiming timing = ecw_i2c_timing(rate);
        uint32_t setup = timing.setup;
        if (i2c_opened || p->instance || p->pin_count != 2 || !setup)
        {
            return -2;
        }
        chip_clk_enable(CLK_I2C);
        i2c_opened = true;
        i2c()->IntEn = 0;
        i2c()->Setup = 0;
        i2c()->Cmd = I2C_CMD_Reset;
        if (os_usdelay(10))
        {
            return -4;
        }
        i2c()->Cmd = I2C_CMD_NoAction;
        i2c()->Status = 0x3f8;
        i2c()->TPM = timing.tpm;
        if (i2c()->TPM != timing.tpm) { return -4; }
        i2c()->Setup = setup;
        uintptr_t k = ecw_sdk_lock();
        chip_io_mode_pinmux_cfg(p->pins[0], FUNC_I2C0_SCL);
        chip_io_mode_pinmux_cfg(p->pins[1], FUNC_I2C0_SDA);
        ecw_sdk_unlock(k);
        return 0;
    }
    if (kind == BSP_UART)
    {
        unsigned n = p->instance;
        if (n > 1 || uart_opened[n] || p->pin_count != 2 || !rate)
        {
            return -2;
        }
        uint32_t clk = chip_clk_get_uart_clk(n),
                 div = (uint32_t)(((uint64_t)clk + 4u * rate) / (8u * rate));
        if (!div || div > 65535)
        {
            return -2;
        }
        uint32_t actual = clk / (8u * div), delta = actual > rate ? actual - rate : rate - actual;
        if ((uint64_t)delta * 100u > (uint64_t)rate * 2u)
        {
            return -2;
        }
        chip_clk_enable(CLK_UART0_UCLK + 2 * n);
        uart_opened[n] = true;
        uintptr_t k = ecw_sdk_lock();
        WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0,
                  READ_REG(CHIP_SMU_PD_SOFTWARE_RESET0) & ~(1u << (17 + n)));
        ecw_sdk_unlock(k);
        if (os_usdelay(2))
        {
            return -4;
        }
        k = ecw_sdk_lock();
        WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0,
                  READ_REG(CHIP_SMU_PD_SOFTWARE_RESET0) | (1u << (17 + n)));
        T_UART_REG_MAP *u = uart(n);
        u->LCR = 0;
        u->Mux1.IER = 0;
        u->Mux2.FCR = 7;
        u->OSCR = 8;
        u->LCR = DRV_UART_LCR_DLAB | lcr;
        u->Mux0.DLL = div & 255;
        u->Mux1.DLM = div >> 8;
        u->LCR = lcr;
        u->MCR = 0; /* No hardware flow control. */
        chip_io_mode_pinmux_cfg(p->pins[0], n ? FUNC_UART1_TXD : FUNC_UART0_TXD);
        chip_io_mode_pinmux_cfg(p->pins[1], n ? FUNC_UART1_RXD : FUNC_UART0_RXD);
        ecw_sdk_unlock(k);
        return 0;
    }
    if (kind == BSP_PWM)
    {
        for (size_t j = 0; j < p->pin_count; ++j)
        {
            unsigned ch = p->pins[j] % 4;
            if (pwm_owned[ch] || drv_pit_ioctrl(ch, DRV_PIT_CTRL_CH_ALLOC))
            {
                return -5;
            }
            pwm_owned[ch] = true;
            pwm_rate[ch] = rate;
            uintptr_t k = ecw_sdk_lock();
            drv_pit_reg_write_ch_En(ch, 0);
            drv_pit_reg_write_ch_IntEn(ch, 0);
            drv_pit_reg_write_ch_ChCtrl(ch, DRV_PIT_CH_MODE_SET_PWM);
            chip_io_mode_pinmux_cfg(p->pins[j], FUNC_GPIOx_PWM_CTRLx);
            ecw_sdk_unlock(k);
        }
        return 0;
    }
    return -2;
}
int ecw_basic_close(const Ecw6700Port *p)
{
    if ((p->id >> 8) == BSP_PWM && p->instance == ECW6700_PWM_CCU6)
    {
        return ecw_ccu6_close(p);
    }
    unsigned kind = p->id >> 8;
    if (kind == BSP_I2C)
    {
        if (!i2c_opened)
        {
            return 0;
        }
        i2c()->IntEn = 0;
        i2c()->Setup = 0;
        i2c()->Cmd = I2C_CMD_Reset;
        if (os_usdelay(10))
        {
            return -4;
        }
        i2c()->Cmd = I2C_CMD_NoAction;
        /* External low lines are not repaired by reset. Keep reservation on failure. */
        uint32_t start = ecw_sdk_time_ms();
        while ((i2c()->Status & (I2C_FLAG_LineSCL | I2C_FLAG_LineSDA)) !=
               (I2C_FLAG_LineSCL | I2C_FLAG_LineSDA))
        {
            if ((uint32_t)(ecw_sdk_time_ms() - start) >= 10)
            {
                return -4;
            }
        }
        i2c()->TPM = 0u;
        int rc = detach(p);
        if (rc)
        {
            return rc;
        }
        i2c_opened = false;
        return 0;
    }
    if (kind == BSP_UART)
    {
        unsigned n = p->instance;
        if (!uart_opened[n])
        {
            return 0;
        }
        T_UART_REG_MAP *u = uart(n);
        u->Mux1.IER = 0;
        u->Mux2.FCR = 7;
        uint32_t start = ecw_sdk_time_ms();
        while (!(u->LSR & DRV_UART_LSR_TEMT))
        {
            if ((uint32_t)(ecw_sdk_time_ms() - start) >= 10)
            {
                return -4;
            }
        }
        int rc = detach(p);
        if (rc)
        {
            return rc;
        }
        chip_clk_disable(CLK_UART0_UCLK + 2 * n);
        uart_opened[n] = false;
        return 0;
    }
    if (kind == BSP_PWM)
    {
        for (size_t j = 0; j < p->pin_count; ++j)
        {
            unsigned ch = p->pins[j] % 4;
            if (!pwm_owned[ch])
            {
                continue;
            }
            uintptr_t k = ecw_sdk_lock();
            drv_pit_reg_write_ch_En(ch, 0);
            drv_pit_reg_write_ch_ChCtrl(ch, DRV_PIT_CH_MODE_SET_PWM);
            int rc = ecw_sdk_gpio_close(p->pins[j]);
            ecw_sdk_unlock(k);
            if (rc)
            {
                return rc;
            }
            if (drv_pit_ioctrl(ch, DRV_PIT_CTRL_CH_RELEASE))
            {
                return -5;
            }
            pwm_owned[ch] = false;
        }
        return 0;
    }
    return -2;
}
EcwIoResult ecw_basic_i2c(uint8_t a, const uint8_t *p, size_t np, const uint8_t *tx, size_t nt,
                          uint8_t *rx, size_t nr, uint32_t ms)
{
    if (!ecw_clock_profile_ready())
    {
        return (EcwIoResult){BSP_IO, 3, true};
    }
    uint32_t setup = i2c()->Setup, saved_tpm = i2c()->TPM;
    EcwRegIo io = {i2c(), ecw_spi_mmio_read, ecw_spi_mmio_write, clock_ms};
    EcwIoResult result = ecw_i2c_poll(&io, a, p, np, tx, nt, rx, nr, ms);
    if (result.fault)
    {
        /* Separate cleanup, never retry the failed command. P16 reset sequence:
         * reset -> 10us -> NoAction; only restore config with FIFO empty and
         * idle high bus. Otherwise keep fault/reservation for explicit recover. */
        if (!os_usdelay(10))
        {
            i2c()->Cmd = I2C_CMD_NoAction;
            uint32_t status = i2c()->Status;
            if ((status &
                 (I2C_FLAG_BusBusy | I2C_FLAG_LineSCL | I2C_FLAG_LineSDA | I2C_FLAG_FIFOEmpty)) ==
                (I2C_FLAG_LineSCL | I2C_FLAG_LineSDA | I2C_FLAG_FIFOEmpty))
            {
                i2c()->Status = 0x3f8;
                i2c()->TPM = saved_tpm;
                i2c()->Setup = setup;
                result.fault = false;
            }
        }
    }
    return result;
}
static uint32_t uart_read(void *context, unsigned offset)
{
    if (offset == 0x34u)
    {
        unsigned instance = (unsigned)(((uintptr_t)context - MEM_BASE_UART0) >> 12);
        return ecw_uart_line_status(instance);
    }
    return ecw_spi_mmio_read(context, offset);
}
EcwIoResult ecw_basic_uart(unsigned n, const uint8_t *tx, uint8_t *rx, size_t count, uint32_t ms)
{
    if (!ecw_clock_profile_ready())
    {
        return (EcwIoResult){BSP_IO, 3, true};
    }
    EcwRegIo io = {uart(n), uart_read, ecw_spi_mmio_write, clock_ms};
    return ecw_uart_poll(&io, tx, rx, count, ms);
}
BspStatus ecw_basic_pwm(const Ecw6700Port *p, size_t index, uint16_t duty, bool stop)
{
    unsigned ch = p->pins[index] % 4;
    uint32_t reload;
    if (!ecw_clock_profile_ready())
    {
        return BSP_IO;
    }
    BspStatus s = ecw_pwm_counts(pwm_rate[ch], duty, &reload);
    if (s != BSP_OK)
    {
        return s;
    }
    uintptr_t k = ecw_sdk_lock();
    drv_pit_reg_write_ch_En(ch, 0);
    drv_pit_reg_write_ch_ChCtrl(ch, DRV_PIT_CH_MODE_SET_PWM |
                                        (!stop && duty == 10000 ? DRV_PIT_PWM_PARK : 0));
    if (!stop && duty && duty < 10000)
    {
        drv_pit_reg_write_ch_Reload(ch, reload);
        drv_pit_reg_write_ch_En(ch, DRV_PIT_CH_MODE_ENABLE_PWM);
    }
    ecw_sdk_unlock(k);
    return BSP_OK;
}
