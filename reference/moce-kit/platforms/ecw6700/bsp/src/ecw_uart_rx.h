#ifndef ECW_UART_RX_H
#define ECW_UART_RX_H
#include "ecw_internal.h"
/* Two native UARTs, fixed public BSP_UART_RX_DEPTH bytes each. No heap/task/DMA. */
void ecw_uart_rx_bind(const Ecw6700Port *port);
void ecw_uart_rx_detach(unsigned instance);
void ecw_uart_rx_forget(unsigned instance);
bool ecw_uart_rx_active(unsigned instance);
void ecw_uart_rx_observe(unsigned instance, uint32_t status);
EcwIoResult ecw_uart_rx_result(unsigned instance, EcwIoResult result);
/* Production SDK LL/IRQ binding, replaced by observable doubles in host tests. */
BspStatus ecw_uart_rx_hw_start(unsigned instance, void (*handler)(void));
void ecw_uart_rx_hw_stop(unsigned instance);
uint32_t ecw_uart_line_status(unsigned instance);
uint8_t ecw_uart_rx_hw_byte(unsigned instance);
#endif
