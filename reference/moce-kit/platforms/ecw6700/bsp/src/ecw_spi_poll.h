#ifndef ECW_SPI_POLL_H
#define ECW_SPI_POLL_H
#include "bsp_ecw6700.h"
/* BSP-private B02P03/P16 register transport. Never exported to modules or Board.
 * Register offsets/bits follow B02P03/P16 include/drivers/spi/spi.h.
 * Caller owns an initialized, exclusive controller with IRQ/DMA disabled,
 * 8-bit data with configured wire bit order, DATA_MERGE=1 and no hardware CS pin muxed.
 * The clock must run throughout polling. Not an ISR/critical-section API.
 */
typedef struct
{
    void *context;
    uint32_t (*read)(void *, unsigned offset);
    void (*write)(void *, unsigned offset, uint32_t value);
    uint32_t (*time_ms)(void *);
} EcwSpiPollIo;
typedef Ecw6700IoResult EcwSpiPollResult;
/* One transaction, 1..512 bytes; no implicit CS or retry. Timeout covers
 * initial idle, FIFO reset and the entire transfer, never restarts per byte.
 * Hardware always uses simultaneous W/R; rx==NULL discards received bytes.
 * Completion requires all RX bytes plus idle/empty FIFOs, not the IRQ END latch.
 * On error RX may contain a prefix. No pointer is retained on return.
 * Fault cleanup issues controller reset; this does NOT prove wire drain.
 */
EcwSpiPollResult ecw_spi_poll_transfer(const EcwSpiPollIo *io, const uint8_t *tx, uint8_t *rx,
                                       size_t n, uint32_t timeout_ms);
/* Actual volatile MMIO accessors; production initializer supplies SPI base.
 * Tests inject a stateful FIFO model instead; no test symbols in firmware. */
uint32_t ecw_spi_mmio_read(void *base, unsigned offset);
void ecw_spi_mmio_write(void *base, unsigned offset, uint32_t value);
#endif
