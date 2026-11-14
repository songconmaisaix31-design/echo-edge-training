/* MOCE-SYNC: native DMA | ../DMA.md
 * B02P03 finite IRAM-to-IRAM DMA copy, bytes, nonoverlapping buffers.
 * Zero-init and serialize thread calls. Keep both buffers and this storage alive
 * until finish returns BSP_OK, including begin/finish failures with active=true.
 * No hidden allocation. No cache maintenance: unsupported with DCache enabled.
 * Pure TC reports completion, not buffer return. finish(cancel=true) requests
 * abort and checks the native channel-enable register before returning memory.
 */
#ifndef BSP_ECW6700_DMA_H
#define BSP_ECW6700_DMA_H
#include "bsp_core.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool active, completed, buffers_returned, cancelled;
    BspStatus error;
    uint32_t events;
} Ecw6700DmaState;
typedef struct {
    Ecw6700DmaState state; /* BSP-owned mutable fields. */
    int channel;
} Ecw6700DmaStorage;
BspStatus bsp_ecw6700_dma_copy_begin(Ecw6700DmaStorage *,const void *,void *,size_t bytes);
BspStatus bsp_ecw6700_dma_query(Ecw6700DmaStorage *,Ecw6700DmaState *);
BspStatus bsp_ecw6700_dma_finish(Ecw6700DmaStorage *,bool cancel);
#ifdef __cplusplus
}
#endif
#endif
