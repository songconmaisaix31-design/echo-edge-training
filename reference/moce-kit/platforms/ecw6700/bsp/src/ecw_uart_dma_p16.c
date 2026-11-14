/* Native finite DMA only. No high-level UART queue, timer, allocation or polling. */
#include "ecw_uart_async.h"
#include "ecw_uart_rx.h"
#include "ecw_sdk.h"
#include "chip_memmap.h"
#include "dma.h"
#include "uart_ll.h"
#include "uart_common.h"

typedef struct
{
    bool active, receive, faulted;
    uint32_t dma_status;
    unsigned instance;
    int channel;
    EcwUartDmaDone done;
    void *context;
} DmaSlot;
static DmaSlot slots[2];

static T_UART_REG_MAP *uart_base(unsigned instance)
{
    return (T_UART_REG_MAP *)(uintptr_t)(MEM_BASE_UART0 + (instance << 12));
}

bool ecw_uart_dma_storage_valid(uint8_t *buffer, size_t capacity)
{
    uintptr_t address = (uintptr_t)buffer;
    return buffer && capacity && capacity <= UINT16_MAX && !(address % 4u) &&
           address >= MEM_BASE_RAM0 && address <= IRAM_END_ADDR &&
           capacity <= IRAM_END_ADDR - address;
}

void ecw_uart_dma_enable(unsigned instance)
{
    /* Only at explicit port initialization, before any transfer; resets FIFO. */
    drv_uart_ll_dma(uart_base(instance), 1);
}

EcwUartDmaState ecw_uart_dma_state(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    EcwUartDmaState state = {0};
    if (instance < 2)
    {
        const DmaSlot *slot = &slots[instance];
        state = (EcwUartDmaState){
            .active = slot->active, .faulted = slot->faulted, .dma_status = slot->dma_status};
    }
    ecw_sdk_unlock(key);
    return state;
}

static void dma_event(void *context, unsigned int events)
{
    DmaSlot *slot = context;
    uintptr_t key = ecw_sdk_lock();
    if (!slot->active)
    {
        ecw_sdk_unlock(key);
        return;
    }
    uint32_t bit = 1u << (unsigned)slot->channel;
    slot->dma_status |= (events & DRV_DMA_EVENT_TC) ? bit << 16 : 0;
    slot->dma_status |= (events & DRV_DMA_EVENT_ABORT) ? bit << 8 : 0;
    slot->dma_status |= (events & DRV_DMA_EVENT_ERROR) ? bit : 0;
    if (events & (DRV_DMA_EVENT_ERROR | DRV_DMA_EVENT_ABORT))
    {
        /* Error/abort, including TC in the same IRQ, does not prove retirement.
         * Retain callback, channel and staging. A later TC cannot erase fault. */
        slot->faulted = true;
    }
    if (slot->faulted || events != DRV_DMA_EVENT_TC)
    {
        ecw_sdk_unlock(key);
        return;
    }
    uint32_t line = ecw_uart_line_status(slot->instance);
    uint32_t error = slot->receive ? line & 0x1eu : 0u;
    BspEvidence wire = slot->receive ? (error ? BSP_UNKNOWN : BSP_YES)
                                     : ((line & DRV_UART_LSR_TEMT) ? BSP_YES : BSP_NO);
    EcwUartDmaDone done = slot->done;
    void *argument = slot->context;
    /* Pure TC follows native descriptor completion. No inferred stop on error. */
    drv_dma_event_register(slot->channel, NULL, NULL);
    drv_dma_ch_release(slot->channel);
    slot->active = false;
    slot->done = NULL;
    slot->context = NULL;
    ecw_sdk_unlock(key);
    done(argument, error, wire);
}

BspStatus ecw_uart_dma_start(unsigned instance, uint8_t *buffer, size_t count, bool receive,
                             EcwUartDmaDone done, void *context, int32_t *native_error)
{
    if (instance > 1 || !done || !native_error || !ecw_uart_dma_storage_valid(buffer, count))
    {
        return BSP_INVALID_ARG;
    }
    *native_error = 0;
    uintptr_t key = ecw_sdk_lock();
    DmaSlot *slot = &slots[instance];
    if (slot->active)
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    int channel = drv_dma_ch_alloc();
    if (channel < 0)
    {
        *native_error = channel;
        ecw_sdk_unlock(key);
        return BSP_NO_RESOURCE;
    }
    unsigned int fifo = drv_uart_ll_dma_addr((unsigned int)(uintptr_t)uart_base(instance));
    T_DMA_CFG_INFO config = {.src = receive ? fifo : (unsigned int)(uintptr_t)buffer,
                             .dst = receive ? (unsigned int)(uintptr_t)buffer : fifo,
                             .len = (unsigned int)count,
                             .mode = receive ? (instance ? DMA_CHN_UART1_RX : DMA_CHN_UART0_RX)
                                             : (instance ? DMA_CHN_UART1_TX : DMA_CHN_UART0_TX)};
    int result = drv_dma_cfg(channel, &config);
    if (result != 0)
    {
        *native_error = result;
        /* Newly allocated channel, no DMA start: no caller/storage lease retained. */
        drv_dma_event_register(channel, NULL, NULL);
        drv_dma_ch_release(channel);
        ecw_sdk_unlock(key);
        return BSP_IO;
    }
    *slot = (DmaSlot){.active = true,
                      .receive = receive,
                      .instance = instance,
                      .channel = channel,
                      .done = done,
                      .context = context};
    drv_dma_event_register(channel, dma_event, slot);
    drv_dma_start(channel);
    ecw_sdk_unlock(key);
    return BSP_OK;
}
