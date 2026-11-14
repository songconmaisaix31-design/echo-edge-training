#ifndef ECW_UART_ASYNC_H
#define ECW_UART_ASYNC_H
#include "bsp_ecw6700.h"
#include "ecw_serial_poll.h"
#define ECW_UART_RESOURCE_CONFLICT (-220)
typedef void (*EcwUartDmaDone)(void *context, uint32_t line_error, BspEvidence wire);
typedef struct
{
    bool active, faulted;
    uint32_t dma_status;
} EcwUartDmaState;
EcwUartDmaState ecw_uart_dma_state(unsigned instance);
bool ecw_uart_dma_storage_valid(uint8_t *buffer, size_t capacity);
void ecw_uart_dma_enable(unsigned instance);
BspStatus ecw_uart_dma_start(unsigned instance, uint8_t *buffer, size_t count, bool receive,
                             EcwUartDmaDone done, void *context, int32_t *native_error);
int ecw_uart_open(const Ecw6700Port *port, const Ecw6700PortConfig *config);
int ecw_uart_close(const Ecw6700Port *port);
BspStatus ecw_uart_dispatch(size_t *count);
void ecw_record_async(BspPort id, EcwIoResult result);
bool ecw_uart_storage_overlaps(const void *buffer, size_t capacity);
#endif
