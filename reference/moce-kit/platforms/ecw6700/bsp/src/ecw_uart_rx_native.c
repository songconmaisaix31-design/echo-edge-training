#include "ecw_uart_rx.h"
#include "ecw_sdk.h"
#include "chip_memmap.h"
#include "arch_irq.h"
#include "uart_common.h"
#include "uart_ll.h"
static T_UART_REG_MAP *base(unsigned instance)
{
    return (T_UART_REG_MAP *)(uintptr_t)(MEM_BASE_UART0 + (instance << 12));
}
BspStatus ecw_uart_rx_hw_start(unsigned instance, void (*handler)(void))
{
    if (arch_irq_is_enabled(VECTOR_NUM_UART0 + instance))
    {
        return BSP_BUSY;
    }
    arch_irq_register(VECTOR_NUM_UART0 + instance, handler);
    drv_uart_ll_intr_en(base(instance), 1, UART_INTR_RX);
    drv_uart_ll_intr_en(base(instance), 1, UART_INTR_LINE);
    arch_irq_unmask(VECTOR_NUM_UART0 + instance);
    return BSP_OK;
}
void ecw_uart_rx_hw_stop(unsigned instance)
{
    drv_uart_ll_intr_en(base(instance), 0, UART_INTR_RX);
    drv_uart_ll_intr_en(base(instance), 0, UART_INTR_LINE);
    arch_irq_mask(VECTOR_NUM_UART0 + instance);
    /* arch_irq_clean is an empty SDK macro. No pending-clear claim, FIFO reset,
     * clock gate, TX abort or DMA stop. Static handler sees inactive/fault state. */
}
uint32_t ecw_uart_line_status(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    uint32_t status = drv_uart_reg_read_LSR(base(instance));
    ecw_uart_rx_observe(instance, status);
    EcwIoResult latched = ecw_uart_rx_result(instance, (EcwIoResult){BSP_OK, 0, false});
    ecw_sdk_unlock(key);
    return status | (uint32_t)latched.native_status;
}
uint8_t ecw_uart_rx_hw_byte(unsigned instance)
{
    return drv_uart_ll_rx_getc((unsigned int)(uintptr_t)base(instance));
}
