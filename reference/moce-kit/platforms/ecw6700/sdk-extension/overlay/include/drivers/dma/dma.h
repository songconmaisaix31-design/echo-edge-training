
/**
 * @file drv_dma.h
 * @brief This is a brief description
 * @details This is the detail description
 * @author your_name
 * @date today
 * @version V0.1
 * @Copyright © 2025 BEIJING ESWIN COMPUTING TECHINOLOGY CO., LTD and its affiliates (“ESWIN”).
 *            All rights reserved. Any modification, reproduction, adaption, translation,
 *            distribution is prohibited without consent.
 * @par History 1:
 *      Date:
 *      Version:
 *      Author:
 *      Modification:
 *
 * @par History 2:
 */

#ifndef DRV_DMA_H
#define DRV_DMA_H

/*--------------------------------------------------------------------------
 *                                 Include files
 *--------------------------------------------------------------------------*/
#include "chip_configuration.h"
#include "chip_dma_idx.h"

/*--------------------------------------------------------------------------
 *                                 Macros
 *--------------------------------------------------------------------------*/
/** Description of the macro */
#define CHN_WIDTH_BYTE          (0)
#define CHN_WIDTH_HALF_WORLD    (1)
#define CHN_WIDTH_WORLD         (2)

/*--------------------------------------------------------------------------
 *                                 Types structs menus
 *--------------------------------------------------------------------------*/
/**
 * @brief The brief description
 * @details The detail description
 */
typedef struct 
{
    void (* callback)(void *);
    void *data;
} T_DRV_DMA_CALLBACK;


/**
 * @brief The brief description
 * @details The detail description
 */
typedef struct 
{
    unsigned int   src;
    unsigned int   dst;
    unsigned int   len;
    E_DMA_CHN_MODE mode;
    unsigned int   dst_trans_byte_size;
    unsigned int   src_trans_byte_size;
} T_DMA_CFG_INFO;

/*--------------------------------------------------------------------------
 *                                 Constants
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 *                                 Global  Variables
 *--------------------------------------------------------------------------*/
/**  Description of global variable  */

/*--------------------------------------------------------------------------
 *                                 Function Prototypes
 *--------------------------------------------------------------------------*/
void drv_dma_status_clean(int num);
int drv_dma_cfg(int num, T_DMA_CFG_INFO * cfg);
int drv_dma_cfg_loop(int num, T_DMA_CFG_INFO *cfg);
void drv_dma_start(int num);
unsigned int drv_dma_stop(int num);
void drv_dma_isr_register(int num, void (* callback)(void *), void  *data);
int drv_dma_ch_alloc(void);
int drv_dma_close(void);
void drv_dma_ch_release(int chn);

/* MOCE candidate extension. Events describe the interrupt, not quiescence.
 * ERROR/ABORT do not authorize reuse/free or a new descriptor on that channel.
 * Choose either this registration or the legacy TC callback, not both.
 */
#define DRV_DMA_EVENT_TC (1u)
#define DRV_DMA_EVENT_ERROR (2u)
#define DRV_DMA_EVENT_ABORT (4u)
typedef void (*DrvDmaEventCallback)(void *context, unsigned int events);
void drv_dma_event_register(int num, DrvDmaEventCallback callback, void *context);

#endif /* DRV_DMA_H */
