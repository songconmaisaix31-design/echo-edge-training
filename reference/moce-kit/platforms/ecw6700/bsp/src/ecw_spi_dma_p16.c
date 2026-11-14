/* P16 hardware binding: independent TX/RX DMA + SPI END join.
 * Reuses fixed Board SPI initialization; never calls spi_master's queue/pinmux.
 * This is BSP-owned register adaptation with vendor DMA/IRQ primitives, not
 * an unchanged vendor spi_master wrapper. Requires the DMA event SDK patch.
 */
#include "ecw_spi_dma_p16.h"
#include "ecw_sdk.h"
#include "ecw_spi_registers.h"
#include "dma.h"
#include "arch_irq.h"

_Static_assert(BSP_ECW_SPI_END_STATUS_READ_LIMIT >= 1u &&
                   BSP_ECW_SPI_END_STATUS_READ_LIMIT <= 1024u,
               "SPI post-END status read limit must be 1..1024");

bool ecw_spi_dma_storage_valid(const void *storage, size_t bytes)
{
    uintptr_t address = (uintptr_t)storage;
    return storage && bytes && bytes <= 512 && !(bytes % 4u) && !(address % 4u) &&
           address >= MEM_BASE_RAM0 && address < IRAM_END_ADDR && bytes <= IRAM_END_ADDR - address;
}

bool ecw_spi_dma_available(void)
{
#if defined(CONFIG_CPU_DCACHE_ENABLE) && CONFIG_CPU_DCACHE_ENABLE
    return false;
#else
    return true;
#endif
}

#if defined(CONFIG_CPU_DCACHE_ENABLE) && CONFIG_CPU_DCACHE_ENABLE
bool ecw_spi_dma_active(void)
{
    return false;
}
EcwSpiDmaState ecw_spi_dma_state(void)
{
    return (EcwSpiDmaState){0};
}
BspStatus ecw_spi_dma_start(const uint8_t *tx, uint8_t *rx, size_t count, EcwSpiDmaDone done,
                            void *context, int32_t *native_error)
{
    (void)rx;
    (void)context;
    if (!native_error || !tx || !done || !count)
    {
        return BSP_INVALID_ARG;
    }
    *native_error = 0;
    return BSP_UNSUPPORTED;
}
#else
enum
{
    TRANS = 0x20,
    CMD = 0x24,
    DATA = 0x2c,
    CTRL = 0x30,
    STATUS = 0x34,
    IRQ_EN = 0x38,
    IRQ_STATUS = 0x3c,
    MUX = 0x78,
    FIFO_ERROR = 3,
    MAX_COUNT = 512
};

typedef struct
{
    bool active, tx_done, rx_done, end, faulted;
    int tx_channel, rx_channel;
    uint32_t errors;
    uint32_t dma_status;
    EcwSpiDmaDone done;
    void *context;
} SpiDma;
static SpiDma transfer;

static uint32_t read_register(unsigned offset)
{
    return ecw_spi_mmio_read((void *)(uintptr_t)MEM_BASE_SPI1, offset);
}

static void write_register(unsigned offset, uint32_t value)
{
    ecw_spi_mmio_write((void *)(uintptr_t)MEM_BASE_SPI1, offset, value);
}

static bool storage_valid(const void *storage, size_t bytes)
{
    uintptr_t address = (uintptr_t)storage;
    return storage && !(address % 4u) && address >= MEM_BASE_RAM0 && address < IRAM_END_ADDR &&
           bytes <= IRAM_END_ADDR - address;
}

static void release_channel(int channel)
{
    if (channel >= 0)
    {
        drv_dma_event_register(channel, NULL, NULL);
        drv_dma_ch_release(channel);
    }
}

static void try_complete(void)
{
    uintptr_t key = ecw_sdk_lock();
    if (!transfer.active || transfer.faulted)
    {
        ecw_sdk_unlock(key);
        return;
    }
    uint32_t flags = read_register(IRQ_STATUS);
    transfer.errors |= flags & FIFO_ERROR;
    transfer.end |= (flags & SPI_INTR_STS_END) != 0;
    /* END alone never proves that DMA stopped accessing staging. */
    if (!transfer.tx_done || !transfer.rx_done || (!transfer.end && !transfer.errors))
    {
        ecw_sdk_unlock(key);
        return;
    }
    uint32_t status = read_register(STATUS);
    /* HIL on B02P03 observed END + DMA TC while BUSY was still set.
     * Recheck only after the actual hardware completion events. Do not ignore
     * BUSY, replay CMD, delay, or introduce a timer. A stuck controller keeps
     * the existing IO result after a finite number of register reads.
     * FIFO/DMA errors are never cleared by this synchronization window.
     */
    if (transfer.end && !transfer.errors)
    {
        for (unsigned reads = 1;
             (status & SPI_STATUS_BUSY) && reads < BSP_ECW_SPI_END_STATUS_READ_LIMIT;
             ++reads)
        {
            status = read_register(STATUS);
        }
    }
    /* Include errors which appeared during the bounded status window. */
    flags |= read_register(IRQ_STATUS);
    transfer.errors |= flags & FIFO_ERROR;
    transfer.errors |= status & (SPI_STATUS_BUSY | SPI_STATUS_RXNUM | SPI_STATUS_TXNUM);
    uint32_t error = transfer.errors;
    EcwSpiDmaDone done = transfer.done;
    void *context = transfer.context;
    write_register(IRQ_EN, 0);
    arch_irq_mask(VECTOR_NUM_SPI2);
    write_register(CTRL, read_register(CTRL) & ~(SPI_CONTROL_RXDMA_EN | SPI_CONTROL_TXDMA_EN));
    write_register(IRQ_STATUS, flags);
    arch_irq_clean(VECTOR_NUM_SPI2);
    /* Both requested DMA transfers have delivered TC. No stop/abort inference. */
    release_channel(transfer.tx_channel);
    release_channel(transfer.rx_channel);
    transfer = (SpiDma){0};
    ecw_sdk_unlock(key);
    done(context, error ? BSP_IO : BSP_OK, error);
}

static void dma_event(bool receive, unsigned int events)
{
    uintptr_t key = ecw_sdk_lock();
    if (transfer.active)
    {
        int channel = receive ? transfer.rx_channel : transfer.tx_channel;
        if (channel >= 0)
        {
            uint32_t bit = 1u << (unsigned int)channel;
            transfer.dma_status |= (events & DRV_DMA_EVENT_TC) ? bit << 16 : 0;
            transfer.dma_status |= (events & DRV_DMA_EVENT_ABORT) ? bit << 8 : 0;
            transfer.dma_status |= (events & DRV_DMA_EVENT_ERROR) ? bit : 0;
        }
        if (events & (DRV_DMA_EVENT_ERROR | DRV_DMA_EVENT_ABORT))
        {
            /* Even TC combined with an error is not proof of safe retirement.
             * Latch this state: a later TC cannot silently recover the channel.
             * Masking SPI notifications does not claim to stop DMA or SCK.
             */
            transfer.faulted = true;
            write_register(IRQ_EN, 0);
            arch_irq_mask(VECTOR_NUM_SPI2);
        }
        else if (events == DRV_DMA_EVENT_TC && !transfer.faulted)
        {
            if (receive)
            {
                transfer.rx_done = true;
            }
            else
            {
                transfer.tx_done = true;
            }
        }
    }
    ecw_sdk_unlock(key);
    try_complete();
}

static void tx_completed(void *context, unsigned int events)
{
    (void)context;
    dma_event(false, events);
}

static void rx_completed(void *context, unsigned int events)
{
    (void)context;
    dma_event(true, events);
}

static void spi_interrupt(void)
{
    uintptr_t key = ecw_sdk_lock();
    if (transfer.active)
    {
        uint32_t flags = read_register(IRQ_STATUS);
        transfer.errors |= flags & FIFO_ERROR;
        transfer.end |= (flags & SPI_INTR_STS_END) != 0;
        write_register(IRQ_STATUS, flags);
        if (transfer.end || transfer.errors)
        {
            /* DMA callbacks will finish the join; don't level-trigger a storm. */
            write_register(IRQ_EN, 0);
            arch_irq_mask(VECTOR_NUM_SPI2);
        }
    }
    ecw_sdk_unlock(key);
    try_complete();
}

bool ecw_spi_dma_active(void)
{
    uintptr_t key = ecw_sdk_lock();
    bool active = transfer.active;
    ecw_sdk_unlock(key);
    return active;
}

EcwSpiDmaState ecw_spi_dma_state(void)
{
    uintptr_t key = ecw_sdk_lock();
    EcwSpiDmaState state = {
        .active = transfer.active, .faulted = transfer.faulted, .dma_status = transfer.dma_status};
    ecw_sdk_unlock(key);
    return state;
}

BspStatus ecw_spi_dma_start(const uint8_t *tx, uint8_t *rx, size_t count, EcwSpiDmaDone done,
                            void *context, int32_t *native_error)
{
    if (!native_error || !tx || !done || !count)
    {
        return BSP_INVALID_ARG;
    }
    *native_error = 0;
    if (count > MAX_COUNT)
    {
        return BSP_UNSUPPORTED;
    }
    size_t padded = (count + 3u) & ~(size_t)3u;
    if (!storage_valid(tx, padded) || (rx && !storage_valid(rx, padded)))
    {
        return BSP_INVALID_ARG;
    }
    if (rx)
    {
        uintptr_t a = (uintptr_t)tx, b = (uintptr_t)rx;
        if ((a < b ? b - a : a - b) < padded)
        {
            return BSP_INVALID_ARG;
        }
    }
    uintptr_t key = ecw_sdk_lock();
    if (transfer.active || (read_register(STATUS) & SPI_STATUS_BUSY))
    {
        ecw_sdk_unlock(key);
        return BSP_BUSY;
    }
    if (read_register(CTRL) & (SPI_CONTROL_SPIRST | SPI_CONTROL_RXFIFORST | SPI_CONTROL_TXFIFORST |
                               SPI_CONTROL_RXDMA_EN | SPI_CONTROL_TXDMA_EN))
    {
        ecw_sdk_unlock(key);
        return BSP_NOT_READY;
    }
    uint32_t stale = read_register(STATUS) & (SPI_STATUS_RXNUM | SPI_STATUS_TXNUM);
    if (stale)
    {
        *native_error = (int32_t)stale;
        ecw_sdk_unlock(key);
        return BSP_IO;
    }
    int tx_channel = drv_dma_ch_alloc();
    if (tx_channel < 0)
    {
        *native_error = tx_channel;
        ecw_sdk_unlock(key);
        return BSP_NO_RESOURCE;
    }
    int rx_channel = rx ? drv_dma_ch_alloc() : -1;
    if (rx && rx_channel < 0)
    {
        *native_error = rx_channel;
        release_channel(tx_channel);
        ecw_sdk_unlock(key);
        return BSP_NO_RESOURCE;
    }
    /* Only fresh, allocated, not-started channels: cleaning status isn't stop. */
    drv_dma_status_clean(tx_channel);
    if (rx)
    {
        drv_dma_status_clean(rx_channel);
    }
    T_DMA_CFG_INFO config = {.src = (unsigned int)(uintptr_t)tx,
                             .dst = MEM_BASE_SPI1 + DATA,
                             .len = (unsigned int)count,
                             .mode = DMA_CHN_SPI_TX};
    int result = drv_dma_cfg(tx_channel, &config);
    if (!result && rx)
    {
        config = (T_DMA_CFG_INFO){.src = MEM_BASE_SPI1 + DATA,
                                  .dst = (unsigned int)(uintptr_t)rx,
                                  .len = (unsigned int)count,
                                  .mode = DMA_CHN_SPI_RX};
        result = drv_dma_cfg(rx_channel, &config);
    }
    if (result)
    {
        *native_error = result;
        release_channel(tx_channel);
        release_channel(rx_channel);
        ecw_sdk_unlock(key);
        return BSP_IO;
    }
    transfer = (SpiDma){.active = true,
                        .rx_done = rx == NULL,
                        .tx_channel = tx_channel,
                        .rx_channel = rx_channel,
                        .done = done,
                        .context = context};
    arch_irq_mask(VECTOR_NUM_SPI2);
    arch_irq_register(VECTOR_NUM_SPI2, spi_interrupt);
    arch_irq_clean(VECTOR_NUM_SPI2);
    write_register(IRQ_STATUS, 0xffffffffu);
    write_register(MUX, 0); /* Exactly 1..512 bytes, no extended transfer counter. */
    write_register(TRANS, SPI_TRANSCTRL_WCNT(count - 1) |
                              (rx ? SPI_TRANSCTRL_RCNT(count - 1) : SPI_TRANSCTRL_TRAMODE_WO));
    write_register(CTRL,
                   SPI_CONTROL_TXDMA_EN | SPI_CONTROL_TXTHRES(1) | (rx ? SPI_CONTROL_RXDMA_EN : 0));
    drv_dma_event_register(tx_channel, tx_completed, NULL);
    if (rx)
    {
        drv_dma_event_register(rx_channel, rx_completed, NULL);
        drv_dma_start(rx_channel);
    }
    drv_dma_start(tx_channel);
    write_register(IRQ_EN, SPI_INTERRUPT_ENDINT_EN | FIFO_ERROR);
    arch_irq_unmask(VECTOR_NUM_SPI2);
    write_register(CMD, 0);
    ecw_sdk_unlock(key);
    return BSP_OK;
}
#endif
