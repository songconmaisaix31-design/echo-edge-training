#ifndef ECW_SPI_ASYNC_H
#define ECW_SPI_ASYNC_H
#include "bsp_ecw6700.h"
#include "ecw_spi_dma_p16.h"
#define ECW_SPI_RESOURCE_CONFLICT (-221)
int ecw_spi_open(const Ecw6700Port *port, const Ecw6700PortConfig *config);
int ecw_spi_close(const Ecw6700Port *port);
bool ecw_spi_storage_overlaps(const void *buffer, size_t capacity);
BspStatus ecw_spi_dispatch(size_t *count);
/* Access only while owning the SPI port, including a managed plan. */
void ecw_spi_workspace(uint8_t **tx, uint8_t **rx, size_t *capacity);
BspStatus ecw_spi_plan_dispatch(size_t *count);
#endif
