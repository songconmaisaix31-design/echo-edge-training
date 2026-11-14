#include "ecw_internal.h"
#include "ecw_sdk.h"
#include "ecw_uart_rx.h"
#include "ecw_pwm_ccu6.h"
static BspStatus i2c_transfer(BspPort id, uint8_t address, const uint8_t *prefix, size_t np,
                              const uint8_t *tx, size_t nt, uint8_t *rx, size_t nr, uint32_t ms)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    if (address > 127 || (!tx && !rx) || (!nt && !nr) || (!tx && nt) || (!rx && nr) || nt > 65535 ||
        nr > 65535 || ms == UINT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (nr > 256)
    {
        return BSP_UNSUPPORTED;
    }
    if (np && nr)
    {
        uint32_t missing;
        BspStatus required = bsp_port_require(id, BSP_CAP_RESTART, &missing);
        if (required != BSP_OK)
        {
            return required;
        }
    }
    const Ecw6700Port *p;
    BspStatus s = ecw_claim_basic(id, BSP_I2C, &p);
    if (s != BSP_OK)
    {
        return s;
    }
    return ecw_finish_basic(id, ecw_basic_i2c(address, prefix, np, tx, nt, rx, nr, ms));
}
BspStatus bsp_i2c_write(BspPort id, uint8_t a, const uint8_t *data, size_t n, uint32_t ms)
{
    return i2c_transfer(id, a, NULL, 0, data, n, NULL, 0, ms);
}
BspStatus bsp_i2c_read(BspPort id, uint8_t a, uint8_t *data, size_t n, uint32_t ms)
{
    return i2c_transfer(id, a, NULL, 0, NULL, 0, data, n, ms);
}
BspStatus bsp_i2c_mem_write(BspPort id, uint8_t a, uint16_t reg, uint8_t bytes, const uint8_t *data,
                            size_t n, uint32_t ms)
{
    if ((bytes != 1 && bytes != 2) || (bytes == 1 && reg > 255))
    {
        return BSP_INVALID_ARG;
    }
    uint8_t prefix[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_transfer(id, a, prefix + (bytes == 1), bytes, data, n, NULL, 0, ms);
}
BspStatus bsp_i2c_mem_read(BspPort id, uint8_t a, uint16_t reg, uint8_t bytes, uint8_t *data,
                           size_t n, uint32_t ms)
{
    if ((bytes != 1 && bytes != 2) || (bytes == 1 && reg > 255))
    {
        return BSP_INVALID_ARG;
    }
    uint8_t prefix[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    return i2c_transfer(id, a, prefix + (bytes == 1), bytes, NULL, 0, data, n, ms);
}
static BspStatus uart_transfer(BspPort id, const uint8_t *tx, uint8_t *rx, size_t n, uint32_t ms)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    if ((!tx && !rx) || !n || n > 65535 || ms == UINT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    const Ecw6700Port *p;
    BspStatus s = ecw_claim_basic(id, BSP_UART, &p);
    if (s != BSP_OK)
    {
        return s;
    }
    if (rx && ecw_uart_rx_active(p->instance))
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_BUSY, 0, false});
    }
    return ecw_finish_basic(id, ecw_basic_uart(p->instance, tx, rx, n, ms));
}
BspStatus bsp_uart_write(BspPort id, const uint8_t *data, size_t n, uint32_t ms)
{
    return uart_transfer(id, data, NULL, n, ms);
}
BspStatus bsp_uart_read(BspPort id, uint8_t *data, size_t n, uint32_t ms)
{
    return uart_transfer(id, NULL, data, n, ms);
}
static BspStatus pwm(BspPort id, size_t channel, uint16_t duty, bool stop)
{
    if (duty > 10000)
    {
        return BSP_INVALID_ARG;
    }
    const Ecw6700Port *p;
    BspStatus s = ecw_claim_basic(id, BSP_PWM, &p);
    if (s != BSP_OK)
    {
        return s;
    }
    if (channel >= p->pin_count)
    {
        s = BSP_INVALID_ARG;
    }
    else if (p->instance == ECW6700_PWM_CCU6)
    {
        return ecw_finish_basic(id, ecw_ccu6_set(p, channel, duty, stop));
    }
    else
    {
        s = ecw_basic_pwm(p, channel, duty, stop);
    }
    return ecw_finish_basic(id, (EcwIoResult){s, 0, s == BSP_IO});
}
BspStatus bsp_pwm_set(BspPort id, size_t channel, uint16_t duty)
{
    return pwm(id, channel, duty, false);
}
BspStatus bsp_pwm_stop(BspPort id, size_t channel)
{
    return pwm(id, channel, 0, true);
}
