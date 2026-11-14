#ifndef ECW_SPI_DMA_P16_H
#define ECW_SPI_DMA_P16_H
#include "bsp_core.h"

/* Bounded post-END status synchronization reads, not microseconds or retries. */
#ifndef BSP_ECW_SPI_END_STATUS_READ_LIMIT
#define BSP_ECW_SPI_END_STATUS_READ_LIMIT 64u
#endif


/* Internal P16 SPI DMA binding.
 * The owner must first initialize SPI1 through the fixed Board backend and
 * exclude all native SPI owners. Buffers are exclusive aligned IRAM staging,
 * padded to ceil(count/4)*4 bytes. RX staging must not overlap TX staging.
 * No timer, worker, queue or caller-buffer copy is hidden in this primitive.
 * Requires the isolated DMA event SDK extension. DMA errors/abort retain all
 * staging and channels until a separately proven recovery path exists.
 */
typedef void (*EcwSpiDmaDone)(void *context, BspStatus status, uint32_t native_error);
BspStatus ecw_spi_dma_start(const uint8_t *tx, uint8_t *rx, size_t count, EcwSpiDmaDone done,
                            void *context, int32_t *native_error);
bool ecw_spi_dma_active(void);
bool ecw_spi_dma_available(void);
bool ecw_spi_dma_storage_valid(const void *storage, size_t bytes);
typedef struct
{
    bool active;
    bool faulted;
    uint32_t dma_status; /* P16 IntStatus layout: TC[21:16], abort[13:8], error[5:0]. */
} EcwSpiDmaState;
EcwSpiDmaState ecw_spi_dma_state(void);
#endif
