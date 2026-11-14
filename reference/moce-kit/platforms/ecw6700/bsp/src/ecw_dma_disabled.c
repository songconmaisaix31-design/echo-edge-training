#include "ecw_features.h"
#if !BSP_ECW_ENABLE_DMA
#include "ecw_spi_dma_p16.h"
#include "ecw_uart_async.h"
#include "bsp_ecw6700_dma.h"
/* Unavailable hardware never borrows caller buffers or changes storage. */
bool ecw_spi_dma_available(void) { return false; }
bool ecw_spi_dma_active(void) { return false; }
bool ecw_spi_dma_storage_valid(const void *p, size_t n)
{ (void)p; (void)n; return false; }
EcwSpiDmaState ecw_spi_dma_state(void) { return (EcwSpiDmaState){0}; }
BspStatus ecw_spi_dma_start(const uint8_t *tx, uint8_t *rx, size_t n,
                            EcwSpiDmaDone done, void *ctx, int32_t *err)
{
    (void)tx; (void)rx; (void)n; (void)done; (void)ctx; (void)err;
    return BSP_UNSUPPORTED;
}
bool ecw_uart_dma_storage_valid(uint8_t *p, size_t n)
{ (void)p; (void)n; return false; }
void ecw_uart_dma_enable(unsigned instance) { (void)instance; }
EcwUartDmaState ecw_uart_dma_state(unsigned instance)
{ (void)instance; return (EcwUartDmaState){0}; }
BspStatus ecw_uart_dma_start(unsigned instance, uint8_t *p, size_t n, bool rx,
                             EcwUartDmaDone done, void *ctx, int32_t *err)
{
    (void)instance; (void)p; (void)n; (void)rx; (void)done; (void)ctx; (void)err;
    return BSP_UNSUPPORTED;
}
BspStatus bsp_ecw6700_dma_copy_begin(Ecw6700DmaStorage *s, const void *src,
                                    void *dst, size_t n)
{ (void)s; (void)src; (void)dst; (void)n; return BSP_UNSUPPORTED; }
BspStatus bsp_ecw6700_dma_query(Ecw6700DmaStorage *s, Ecw6700DmaState *out)
{ (void)s; (void)out; return BSP_UNSUPPORTED; }
BspStatus bsp_ecw6700_dma_finish(Ecw6700DmaStorage *s, bool cancel)
{ (void)s; (void)cancel; return BSP_UNSUPPORTED; }
#endif
