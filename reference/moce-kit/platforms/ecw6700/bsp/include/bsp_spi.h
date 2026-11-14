/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_SPI_H
#define BSP_SPI_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* SPI is 8-bit master. tx required; rx NULL means transmit only. No implicit chip select. */
    BspStatus bsp_spi_transfer(BspPort id, const uint8_t *tx, uint8_t *rx, size_t n,
                               uint32_t timeout_ms);
    BspStatus bsp_spi_transfer_async(BspPort id, const uint8_t *tx, uint8_t *rx, size_t n,
                                     BspRequest *request);
#ifdef __cplusplus
}
#endif
#endif
