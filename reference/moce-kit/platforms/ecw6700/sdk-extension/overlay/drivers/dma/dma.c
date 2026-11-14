
/**
 * @file dma.c
 * @brief DMA driver implementation
 * @details This file contains the implementation of the DMA driver for the ESWIN platform.
 * @author your_name * @date today
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

/*--------------------------------------------------------------------------
 *                                 Include files
 *--------------------------------------------------------------------------*/
#include <stdio.h>
#include "chip_memmap.h"
#include "chip_clk_ctrl.h"
#include "chip_smupd.h"
#include "arch_irq.h"
#include "chip_irqvector.h"
#include "dma.h"
#include "oshal.h"

/*--------------------------------------------------------------------------
 *                                 Local Macros
 *--------------------------------------------------------------------------*/
#define DRV_DMA_CH_NUM              (6)
#define DRV_DMA_DESCRIPTOR_MAX      (4)

#define DMAC_INTR_TC(X)             (((X) >> 16) & 0xFF)
#define DMAC_INTR_ABORT(X)          (((X) >> 8) & 0xFF)
#define DMAC_INTR_ERROR(X)          (((X) >> 0) & 0xFF)

#define CHN_CTRL_EN(x)              (((x) & 0x1) << 0)
#define CHN_CTRL_INT_TC_MASK(x)     (((x) & 0x1) << 1)
#define CHN_CTRL_INT_ERR_MASK(x)    (((x) & 0x1) << 2)
#define CHN_CTRL_INT_ABT_MASK(x)    (((x) & 0x1) << 3)
#define CHN_CTRL_DST_REQ_SEL(x)     (((x) & 0xF) << 4)
#define CHN_CTRL_SRC_REQ_SEL(x)     (((x) & 0xF) << 8)
#define CHN_CTRL_DST_ADDR_CTRL(x)   (((x) & 0x3) << 12)
#define CHN_CTRL_SRC_ADDR_CTRL(x)   (((x) & 0x3) << 14)
#define CHN_CTRL_DST_MODE(x)        (((x) & 0x1) << 16)
#define CHN_CTRL_SRC_MODE(x)        (((x) & 0x1) << 17)
#define CHN_CTRL_DST_WIDTH(x)       (((x) & 0x3) << 18)
#define CHN_CTRL_SRC_WIDTH(x)       (((x) & 0x3) << 20)
#define CHN_CTRL_SRC_BURST_SIZE(x)  (((x) & 0x7) << 22)
#define CHN_CTRL_PRIORITY(x)        (((x) & 0x1) << 29)

#define CHN_ENABLE                  (1)
#define CHN_DISABLE                 (0)

#define CHN_INT_MASK                (1)
#define CHN_INT_UNMASK              (0)

#define CHN_ADDR_INC                (0)
#define CHN_ADDR_DEC                (1)
#define CHN_ADDR_FIX                (2)

#define CHN_SRC_MODE_NORMAL         (0)
#define CHN_SRC_MODE_HANDSHAKE      (1)

#define CHN_BURST_SIZE_1_TRANS      (0)
#define CHN_BURST_SIZE_2_TRANS      (1)
#define CHN_BURST_SIZE_4_TRANS      (2)
#define CHN_BURST_SIZE_8_TRANS      (3)
#define CHN_BURST_SIZE_16_TRANS     (4)
#define CHN_BURST_SIZE_32_TRANS     (5)
#define CHN_BURST_SIZE_64_TRANS     (6)
#define CHN_BURST_SIZE_128_TRANS    (7)

#define CHN_PRIORITY_LOW            (0)
#define CHN_PRIORITY_HIGH           (1)

#define CHN_REQ_SEL_UART0_TX        (0)
#define CHN_REQ_SEL_UART0_RX        (1)
#define CHN_REQ_SEL_UART1_TX        (2)
#define CHN_REQ_SEL_UART1_RX        (3)
#define CHN_REQ_SEL_I2C             (4)
#define CHN_REQ_SEL_SPI_TX          (5)
#define CHN_REQ_SEL_SPI_RX          (6)
#define CHN_REQ_SEL_SPI_FLASH_TX    (7)
#define CHN_REQ_SEL_SPI_FLASH_RX    (8)
#define CHN_REQ_SEL_I2S_TX          (9)
#define CHN_REQ_SEL_I2S_RX          (10)
#define CHN_REQ_SEL_UART2_TX        (11)
#define CHN_REQ_SEL_UART2_RX        (12)
#define CHN_REQ_SEL_AES             (13)
#ifdef CONFIG_ECR6630
#define CHN_REQ_SEL_MEM             (14)
#else
#define CHN_REQ_SEL_SPI2_TX      	(14)
#define CHN_REQ_SEL_SPI2_RX         (15)
#define CHN_REQ_SEL_MEM        		(16)
#endif

#define DRV_DMA_WIDTH_IS_WORLD(x)   ((x) & 0x200000)
#define DRV_DMA_WIDTH_IS_HALF_WORLD(x) ((x) & 0x100000)

/*  dma mem & mem cfg */
#define CHN_CTRL_VALUE_MEM          CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_DST_MODE(CHN_SRC_MODE_NORMAL) | \
    CHN_CTRL_DST_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

/*  dma mem & uart cfg */
#define CHN_CTRL_VALUE_TX_BYTE(X)  CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_DST_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_DST_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define CHN_CTRL_VALUE_I2S_TX_BYTE(X, dst_byte_size, src_byte_size) \
    CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_DST_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_DST_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(dst_byte_size) | \
    CHN_CTRL_SRC_WIDTH(src_byte_size) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define CHN_CTRL_VALUE_I2S_RX_BYTE(X, dst_byte_size, src_byte_size) \
    CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_SRC_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(dst_byte_size) | \
    CHN_CTRL_SRC_WIDTH(src_byte_size) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define CHN_CTRL_VALUE_RX_BYTE(X)   CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_SRC_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_WIDTH(CHN_WIDTH_BYTE) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define CHN_CTRL_VALUE_TX_WORLD(X)  CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_DST_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_DST_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(CHN_WIDTH_WORLD) | \
    CHN_CTRL_SRC_WIDTH(CHN_WIDTH_WORLD) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define CHN_CTRL_VALUE_RX_WORLD(X)  CHN_CTRL_EN(CHN_ENABLE) | \
    CHN_CTRL_INT_TC_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ERR_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_INT_ABT_MASK(CHN_INT_UNMASK) | \
    CHN_CTRL_SRC_REQ_SEL(X) | \
    CHN_CTRL_DST_ADDR_CTRL(CHN_ADDR_INC) | \
    CHN_CTRL_SRC_ADDR_CTRL(CHN_ADDR_FIX) | \
    CHN_CTRL_SRC_MODE(CHN_SRC_MODE_HANDSHAKE) | \
    CHN_CTRL_DST_WIDTH(CHN_WIDTH_WORLD) | \
    CHN_CTRL_SRC_WIDTH(CHN_WIDTH_WORLD) | \
    CHN_CTRL_SRC_BURST_SIZE(CHN_BURST_SIZE_1_TRANS) | \
    CHN_CTRL_PRIORITY(CHN_PRIORITY_LOW)

#define DRV_DMA_CHECK_CHN( ch )                   \
    do {                                          \
        if( (ch) >= DRV_DMA_CH_NUM )              \
        {                                         \
            return;                               \
        }                                         \
    } while( 0 )


/*--------------------------------------------------------------------------
 *                                 Local Types structs menus
 *--------------------------------------------------------------------------*/
/**
 * @brief DMA linked list descriptor
 * @details This structure defines the DMA linked list descriptor
 */
typedef volatile struct 
{
    volatile unsigned int ChCtrl;               // Channel control register
    volatile unsigned int ChSrcAddr;            // Channel source address register
    volatile unsigned int ChDstAddr;            // Channel destination address register
    volatile unsigned int ChTranSize;           // Channel transfer size register
    volatile unsigned int ChLLPointer;          // Channel linked list pointer register
} DMA_LLDESCRIPTOR;


/**
 * @brief DMA register map
 * @details This structure defines the DMA register map
 */
typedef volatile struct _T_DMA_REG_MAP 
{
    unsigned int	 IdRev;                         // 0x00  -- ID and revision register
    unsigned int	 Resv0[3];                      // 0x04 ~ 0x0C  -- reserved register
    unsigned int	 DMACfg;                        // 0x10  -- DMAC configuration register
    unsigned int	 Resv1[3];                      // 0x14 ~ 0x1C  -- reserved register
    unsigned int	 DMACtrl;                       // 0x20  -- DMA control register
    unsigned int	 Resv2[3];                      // 0x24 ~ 0x2C  -- reserved register
    unsigned int	 IntStatus;                     // 0x30  -- interrupt status register
    unsigned int	 ChEN;                          // 0x34  -- Channel enable register
    unsigned int	 Resv3[2];                      // 0x38 ~ 0x3C  -- reserved register
    unsigned int	 ChAbort;                       // 0x40 -- Channel abort register
    DMA_LLDESCRIPTOR Ch[DRV_DMA_CH_NUM];            // DMA channels
} T_DMA_REG_MAP;


/**
 * @brief DMA chain structure
 * @details This structure defines the DMA chain structure
 */
typedef struct 
{
    unsigned int	 count;
    unsigned int	 dma_ctrl_cfg;
    DMA_LLDESCRIPTOR llDes[DRV_DMA_DESCRIPTOR_MAX];
} DMA_CHAIN;


/**
 * @brief DMA channel structure
 * @details This structure defines the DMA channel structure
 */
typedef struct 
{
    T_DRV_DMA_CALLBACK dma_callback;
    DMA_CHAIN		   dma_chn_lldes;
} T_DMA_CHN;


/**
 * @brief DMA device structure
 * @details This structure defines the DMA device structure
 */
typedef struct 
{
    char init;
    unsigned char alloc_flag;
    T_DMA_REG_MAP * reg_base;
    T_DMA_CHN *		p_chn;
} T_DRV_DMA_DEV;

/*--------------------------------------------------------------------------
 *                                 Local Constants
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 *                                 Local Function Prototypes
 *--------------------------------------------------------------------------*/
static unsigned int dma_get_ctrl_cfg(T_DMA_CFG_INFO *cfg);

/*--------------------------------------------------------------------------
 *                                 Global Constants
 *--------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
 *                                 Global Variables
 *--------------------------------------------------------------------------*/

static unsigned int dma_cfg[DMA_CHN_MEM + 1] = 
{
    [DMA_CHN_UART0_TX]     = CHN_CTRL_VALUE_TX_BYTE(CHN_REQ_SEL_UART0_TX),
    [DMA_CHN_UART0_RX]     = CHN_CTRL_VALUE_RX_BYTE(CHN_REQ_SEL_UART0_RX),
    [DMA_CHN_UART1_TX]     = CHN_CTRL_VALUE_TX_BYTE(CHN_REQ_SEL_UART1_TX),
    [DMA_CHN_UART1_RX]     = CHN_CTRL_VALUE_RX_BYTE(CHN_REQ_SEL_UART1_RX),
    [DMA_CHN_I2C_TX]       = CHN_CTRL_VALUE_TX_BYTE(CHN_REQ_SEL_I2C),
    [DMA_CHN_I2C_RX]       = CHN_CTRL_VALUE_RX_BYTE(CHN_REQ_SEL_I2C),
    [DMA_CHN_SPI_TX]       = CHN_CTRL_VALUE_TX_WORLD(CHN_REQ_SEL_SPI_TX),
    [DMA_CHN_SPI_RX]       = CHN_CTRL_VALUE_RX_WORLD(CHN_REQ_SEL_SPI_RX),
    [DMA_CHN_SPI_FLASH_TX] = CHN_CTRL_VALUE_TX_WORLD(CHN_REQ_SEL_SPI_FLASH_TX),
    [DMA_CHN_SPI_FLASH_RX] = CHN_CTRL_VALUE_RX_WORLD(CHN_REQ_SEL_SPI_FLASH_RX),
    [DMA_CHN_UART2_TX]     = CHN_CTRL_VALUE_TX_BYTE(CHN_REQ_SEL_UART2_TX),
    [DMA_CHN_UART2_RX]     = CHN_CTRL_VALUE_RX_BYTE(CHN_REQ_SEL_UART2_RX),
#ifdef CHN_REQ_SEL_SPI2_TX
    [DMA_CHN_SPI2_TX]      = CHN_CTRL_VALUE_TX_WORLD(CHN_REQ_SEL_SPI2_TX),
#endif
#ifdef CHN_REQ_SEL_SPI2_RX
    [DMA_CHN_SPI2_RX]      = CHN_CTRL_VALUE_RX_WORLD(CHN_REQ_SEL_SPI2_RX),
#endif
#ifdef CHN_REQ_SEL_I2C1
    [DMA_CHN_I2C1_TX]      = CHN_CTRL_VALUE_TX_BYTE(CHN_REQ_SEL_I2C1),
#endif
#ifdef CHN_REQ_SEL_I2C1
    [DMA_CHN_I2C1_RX]      = CHN_CTRL_VALUE_RX_BYTE(CHN_REQ_SEL_I2C1),
#endif
    [DMA_CHN_MEM]          = CHN_CTRL_VALUE_MEM
};

//static T_DMA_CHN dma_chn[DRV_DMA_CH_NUM] __attribute__((section(".dma.data")));
static T_DMA_CHN dma_chn[DRV_DMA_CH_NUM];
static T_DRV_DMA_DEV dma_dev;

typedef struct
{
    DrvDmaEventCallback callback;
    void *context;
    unsigned int generation;
} DmaEventRegistration;
static DmaEventRegistration dma_events[DRV_DMA_CH_NUM];



/*--------------------------------------------------------------------------
 *                                 Global Function Prototypes
 *--------------------------------------------------------------------------*/


/*--------------------------------------------------------------------------
 *                                 Function Definitions
 *--------------------------------------------------------------------------*/
unsigned int dma_get_ctrl_cfg(T_DMA_CFG_INFO *cfg) __attribute__((section(".ilm_text_drv")));
void dma_isr(void) __attribute__((section(".ilm_text_drv")));
void drv_dma_status_clean(int num) __attribute__((section(".ilm_text_drv")));
int drv_dma_cfg(int num, T_DMA_CFG_INFO *cfg) __attribute__((section(".ilm_text_drv")));
void drv_dma_start(int num) __attribute__((section(".ilm_text_drv")));

/**
 * @brief Get DMA control configuration
 * @param cfg Pointer to DMA configuration info
 * @return DMA control configuration value
 */
unsigned int dma_get_ctrl_cfg(T_DMA_CFG_INFO *cfg)
{
    unsigned int		val	 = 0;
    E_DMA_CHN_MODE		mode = cfg->mode;

    if (mode > DMA_CHN_MEM) 
    {
        return -1;
    }

    if (mode == DMA_CHN_I2S_TX) 
    {
        dma_cfg[DMA_CHN_I2S_TX] = CHN_CTRL_VALUE_I2S_TX_BYTE(CHN_REQ_SEL_I2S_TX, cfg->dst_trans_byte_size, cfg->src_trans_byte_size);
    }
    else if (mode == DMA_CHN_I2S_RX) 
    {
        dma_cfg[DMA_CHN_I2S_RX] = CHN_CTRL_VALUE_I2S_RX_BYTE(CHN_REQ_SEL_I2S_RX, cfg->dst_trans_byte_size, cfg->src_trans_byte_size);
    }
#ifdef CHN_REQ_SEL_I2C1
    else if (mode == DMA_CHN_I2C1_TX || mode == DMA_CHN_I2C1_RX )
    {
        WRITE_REG(CHIP_SMU_PD_I2C2_DMA_SEL, 2);
    }
#endif

    val = dma_cfg[mode];

    if (DRV_DMA_WIDTH_IS_WORLD(val)) 
    {
        cfg->len = (cfg->len + 3) / 4;
    } 
    else if (DRV_DMA_WIDTH_IS_HALF_WORLD(val)) 
    {
        cfg->len = (cfg->len + 1) / 2;
    }

    return val;
}


/**
 * @brief DMA interrupt service routine
 */
void dma_isr(void)
{
#if defined(CONFIG_ESWIN_ISR_DEBUG_HOOK)
    eswin_enter_isr_hook(VECTOR_NUM_DMA);
#endif
    unsigned int status = dma_dev.reg_base->IntStatus;
    dma_dev.reg_base->IntStatus = status;
    DmaEventRegistration snapshot[DRV_DMA_CH_NUM];
    unsigned int unhandled_errors = DMAC_INTR_ERROR(status);
    for (unsigned int i = 0; i < DRV_DMA_CH_NUM; ++i)
    {
        snapshot[i] = dma_events[i];
        if (snapshot[i].callback)
        {
            unhandled_errors &= ~(1u << i);
        }
    }

    /* Keep the original policy for unregistered legacy error channels.
     * Registered channels report errors; neither abort nor error becomes TC.
     */
    unsigned int start_tick = ARCH_GET_TICK();
    while (unhandled_errors)
    {
        if (ARCH_GET_TICK() - start_tick >= chip_cpu_clk_get_MHz() * 1000000)
        {
            os_printf(LL_INFO, "DMA error 0x%x\n", unhandled_errors);
            system_assert(0);
        }
    }

    /* Preserve the legacy two-pass ordering: all abort counts are retired
     * before any legacy TC callback can inspect another channel's state.
     */
    for (unsigned int i = 0; i < DRV_DMA_CH_NUM; ++i)
    {
        if (!snapshot[i].callback && dma_chn[i].dma_callback.callback &&
            ((DMAC_INTR_ABORT(status) >> i) & 1u))
        {
            dma_chn[i].dma_chn_lldes.count = 0;
        }
    }

    for (unsigned int i = 0; i < DRV_DMA_CH_NUM; ++i)
    {
        if (snapshot[i].callback)
        {
            unsigned int events = 0;
            if ((DMAC_INTR_TC(status) >> i) & 1u)
            {
                events |= DRV_DMA_EVENT_TC;
            }
            if ((DMAC_INTR_ERROR(status) >> i) & 1u)
            {
                events |= DRV_DMA_EVENT_ERROR;
            }
            if ((DMAC_INTR_ABORT(status) >> i) & 1u)
            {
                events |= DRV_DMA_EVENT_ABORT;
            }
            /* A callback may reconfigure another channel. Do not deliver the
             * already captured old IRQ into its new subscription/context.
             */
            if (events && snapshot[i].generation == dma_events[i].generation)
            {
                if (events == DRV_DMA_EVENT_TC)
                {
                    dma_chn[i].dma_chn_lldes.count = 0;
                }
                snapshot[i].callback(snapshot[i].context, events);
            }
        }
        else if (dma_chn[i].dma_callback.callback)
        {
            if ((DMAC_INTR_TC(status) >> i) & 1u)
            {
                dma_chn[i].dma_chn_lldes.count = 0;
                dma_chn[i].dma_callback.callback(dma_chn[i].dma_callback.data);
            }
        }
    }
#if defined(CONFIG_ESWIN_ISR_DEBUG_HOOK)
    eswin_exit_isr_hook(VECTOR_NUM_DMA);
#endif
}


/**
 * @brief Clean DMA status
 * @param num DMA channel number
 */
void drv_dma_status_clean(int num)
{
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    p_dma_dev->reg_base->IntStatus = (1 << num) << 16;
    p_dma_dev->p_chn[num].dma_chn_lldes.count = 0;
}


/**
 * @brief Configure DMA channel
 * @param num DMA channel number
 * @param cfg Pointer to DMA configuration info
 * @return 0 on success, -1 on failure
 */
int drv_dma_cfg(int num, T_DMA_CFG_INFO *cfg)
{
    T_DRV_DMA_DEV *	  p_dma_dev		 = &dma_dev;
    DMA_CHAIN *		  pDMA_chn_chain = &p_dma_dev->p_chn[num].dma_chn_lldes;
    DMA_LLDESCRIPTOR *pDMA_chn_LLD	 = &p_dma_dev->reg_base->Ch[num];

    if (cfg == NULL || cfg->len == 0) 
    {
        return -1;
    }

    if (pDMA_chn_chain->count == 0) 
    {
        pDMA_chn_chain->dma_ctrl_cfg = dma_get_ctrl_cfg(cfg);

        pDMA_chn_LLD->ChSrcAddr	  = cfg->src;
        pDMA_chn_LLD->ChDstAddr	  = cfg->dst;
        pDMA_chn_LLD->ChTranSize  = cfg->len;
        pDMA_chn_LLD->ChLLPointer = 0;
    } 
    else if (pDMA_chn_chain->count > DRV_DMA_DESCRIPTOR_MAX) 
    {
        return -1;
    } 
    else 
    {
        if (pDMA_chn_chain->count == 1) 
        {
            pDMA_chn_LLD->ChLLPointer = (unsigned int)(&(pDMA_chn_chain->llDes[0]));
        } 
        else 
        {
            pDMA_chn_chain->llDes[pDMA_chn_chain->count - 2].ChLLPointer = \
                (unsigned int)(&(pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1]));
        }

        pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1].ChCtrl		 = dma_get_ctrl_cfg(cfg);
        pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1].ChSrcAddr	 = cfg->src;
        pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1].ChDstAddr	 = cfg->dst;
        pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1].ChTranSize	 = cfg->len;
        pDMA_chn_chain->llDes[pDMA_chn_chain->count - 1].ChLLPointer = 0;
    }

    pDMA_chn_chain->count++;

    return 0;
}


int drv_dma_cfg_loop(int num, T_DMA_CFG_INFO *cfg)
{
    T_DRV_DMA_DEV *	  p_dma_dev		 = &dma_dev;
    DMA_CHAIN *		  pDMA_chn_chain = &p_dma_dev->p_chn[num].dma_chn_lldes;
    DMA_LLDESCRIPTOR *pDMA_chn_LLD	 = &p_dma_dev->reg_base->Ch[num];
    DMA_LLDESCRIPTOR *pDMA_chn_LLD0	 = &pDMA_chn_chain->llDes[0];

    if (cfg == NULL || cfg->len == 0) 
    {
        return -1;
    }

    pDMA_chn_chain->dma_ctrl_cfg = dma_get_ctrl_cfg(cfg);

    // cfg dma hw
    pDMA_chn_LLD->ChSrcAddr	   = cfg->src;
    pDMA_chn_LLD->ChDstAddr	   = cfg->dst;
    pDMA_chn_LLD->ChTranSize   = cfg->len;
    pDMA_chn_LLD->ChLLPointer  = (unsigned int)pDMA_chn_LLD0;

    // make des0 for loop
    pDMA_chn_LLD0->ChCtrl	   = pDMA_chn_chain->dma_ctrl_cfg;
    pDMA_chn_LLD0->ChSrcAddr   = cfg->src;
    pDMA_chn_LLD0->ChDstAddr   = cfg->dst;
    pDMA_chn_LLD0->ChTranSize  = cfg->len;
    pDMA_chn_LLD0->ChLLPointer = (unsigned int)pDMA_chn_LLD0;

    return 0;
}


/**
 * @brief Start DMA channel
 * @param num DMA channel number
 */
void drv_dma_start(int num)
{
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    p_dma_dev->reg_base->IntStatus = (1 << (num+16)) | (1 << (num+8)) | (1 << (num+0));
    p_dma_dev->reg_base->Ch[num].ChCtrl = p_dma_dev->p_chn[num].dma_chn_lldes.dma_ctrl_cfg;
}


/**
 * @brief Stop DMA channel
 * @param num DMA channel number
 * @return Remaining transfer size
 */
unsigned int drv_dma_stop(int num)
{
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    p_dma_dev->reg_base->ChAbort = 1 << num;
    return p_dma_dev->reg_base->Ch[num].ChTranSize;
}


/**
 * @brief Register DMA interrupt service routine
 * @param num DMA channel number
 * @param callback Callback function
 * @param data Callback data
 */
void drv_dma_isr_register(int num, void (*callback)(void *), void *data)
{
    DRV_DMA_CHECK_CHN(num);
    unsigned long key = system_irq_save();
    ++dma_events[num].generation;
    dma_events[num].callback = NULL;
    dma_events[num].context = NULL;
    dma_chn[num].dma_callback.callback = callback;
    dma_chn[num].dma_callback.data = data;
    system_irq_restore(key);
}

void drv_dma_event_register(int num, DrvDmaEventCallback callback, void *context)
{
    DRV_DMA_CHECK_CHN(num);
    unsigned long key = system_irq_save();
    ++dma_events[num].generation;
    dma_chn[num].dma_callback.callback = NULL;
    dma_chn[num].dma_callback.data = NULL;
    dma_events[num].callback = callback;
    dma_events[num].context = context;
    system_irq_restore(key);
}


/**
 * @brief Allocate DMA channel
 * @return DMA channel number on success, -1 on failure
 */
int drv_dma_ch_alloc(void)
{
    unsigned long flags;
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    if (p_dma_dev->init == 0) 
    {
        chip_clk_enable(CLK_DMA);
        p_dma_dev->p_chn	= dma_chn;
        p_dma_dev->reg_base = (T_DMA_REG_MAP *)MEM_BASE_DMAC;
        p_dma_dev->init		= 1;
        arch_irq_unmask(VECTOR_NUM_DMA);
#ifdef CHN_REQ_SEL_I2C1
        WRITE_REG(CHIP_SMU_PD_I2C2_DMA_SEL, 2);
#endif
    }

    for (int i = 0; i < DRV_DMA_CH_NUM; i++) 
    {
        flags = system_irq_save();
        if ((p_dma_dev->alloc_flag & (1 << i)) == 0) 
        {
            p_dma_dev->alloc_flag |= 1 << i;
            p_dma_dev->p_chn[i].dma_chn_lldes.count = 0;
            system_irq_restore(flags);
            return i;
        }

        system_irq_restore(flags);
    }

    return -1;
}


/**
 * @brief Release DMA channel
 * @param chn DMA channel number
 */
void drv_dma_ch_release(int chn)
{
    unsigned long flags;
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    DRV_DMA_CHECK_CHN(chn);

    if (p_dma_dev->init == 0) 
    {
        return;
    }

    flags = system_irq_save();
    if ((p_dma_dev->alloc_flag & (1 << chn))) 
    {
        p_dma_dev->alloc_flag &= (~(1 << chn));
        p_dma_dev->p_chn[chn].dma_chn_lldes.count = 0;
    }
    system_irq_restore(flags);
}


/**
 * @brief Close DMA
 * @return 0 on success, -1 on failure
 */
int drv_dma_close(void)
{
    T_DRV_DMA_DEV *p_dma_dev = &dma_dev;

    memset(p_dma_dev, 0, sizeof(dma_dev));

    arch_irq_mask(VECTOR_NUM_DMA);
    arch_irq_clean(VECTOR_NUM_DMA);

    return 0;
}

