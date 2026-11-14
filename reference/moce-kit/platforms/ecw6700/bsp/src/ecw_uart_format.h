#ifndef ECW_UART_FORMAT_H
#define ECW_UART_FORMAT_H
#include "bsp_ecw6700.h"
/* Encodings verified against B02P03 include/drivers/uart/uart.h. */
static inline BspStatus ecw_uart_format(const Ecw6700PortConfig *c, uint32_t *lcr)
{
    unsigned bits = c->uart_data_bits ? c->uart_data_bits : 8;
    if (bits < 5 || bits > 8 || c->uart_parity > 2 || c->uart_stop_bits > 2)
        return BSP_INVALID_ARG;
    if (bits == 5 && c->uart_stop_bits == 2)
        return BSP_UNSUPPORTED;
    *lcr = (bits - 5) | (c->uart_stop_bits == 2 ? 4u : 0u) |
           (c->uart_parity == 1 ? 8u : c->uart_parity == 2 ? 24u : 0u);
    return BSP_OK;
}
#endif
