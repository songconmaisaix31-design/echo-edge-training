
#ifndef __I2S_LL_H__
#define __I2S_LL_H__

#include "i2s_reg_struct.h"
#include "i2s_reg_struct.h"


#define I2S_REG_BASE_TX  ((T_I2S_TX_REG_MAP *)(MEM_BASE_I2S + I2S_REG_BASE_OFFSET_TX))
#define I2S_REG_BASE_RX  ((T_I2S_RX_REG_MAP *)(MEM_BASE_I2S + I2S_REG_BASE_OFFSET_RX))
#define I2S_REG_BASE_TOP ((T_I2S_REG_MAP *)(MEM_BASE_I2S + I2S_REG_BASE_OFFSET_TOP))



static inline unsigned int drv_i2s_reg_rx_dma_addr(void)
{
    return (unsigned int)(&I2S_REG_BASE_RX->rx_fifo);
}

static inline unsigned int drv_i2s_reg_tx_dma_addr(void)
{
    return (unsigned int)(&I2S_REG_BASE_TX->tx_fifo);
}

static inline void drv_i2s_tx_en(void)
{
    I2S_REG_BASE_TOP->cfg |= I2S_CFG_TX_EN;
}

static inline void drv_i2s_tx_dis(void)
{
    I2S_REG_BASE_TOP->cfg &= ~I2S_CFG_TX_EN;
}

static inline void drv_i2s_rx_en(void)
{
    I2S_REG_BASE_TOP->cfg |= I2S_CFG_RX_EN;
}

static inline void drv_i2s_rx_dis(void)
{
    I2S_REG_BASE_TOP->cfg &= ~I2S_CFG_RX_EN;
}

void drv_i2s_cfg_tx(unsigned int slot_mode, unsigned int master_mode);
void drv_i2s_cfg_rx(unsigned int slot_mode, unsigned int master_mode);
void drv_i2s_cfg_slot(unsigned int format, unsigned int slot_width, unsigned int data_width);
void drv_i2s_cfg_clk(unsigned int clk, unsigned int data_width);
void drv_i2s_cfg_mclk(unsigned int mclk);
int drv_i2s_bus_init(void);
/* MOCE integration extension: fixed four-wire full-duplex bus, no MCLK pad.
 * Caller owns four distinct GPIO0..24 pins and serialized I2S lifecycle.
 * Legacy drv_i2s_bus_init retains the vendor default pin configuration. */
#define MOCE_P16_I2S_FOUR_WIRE_API 1
int drv_i2s_bus_init_four_wire(int txsck, int txws, int txd, int rxd);
void drv_i2s_bus_release(void);

#endif // __I2S_LL_H__
