/**
*@file     i2s_ll.c
*@brief    This part of program is I2S Software interface
*@author   wangc
*@data     2024.1.29
*@version  v0.1
*@par Copyright (c):
     eswin inc.
*@par History:
   version:autor,data,desc\n
*/


#include "chip_pinmux.h"
#include "chip_memmap.h"
#include "chip_clk_ctrl.h"
#include "oshal.h"
#include "i2s.h"
#include "i2s_ll.h"
#include "i2s_core.h"

#define RST_BIT_I2S       (1<<5)
#define CLK_160M          (160000000) // 160M
#define CLK_COEFFICIENT_4 (4)

#define FRA_BASE_100      (100)
#define FRA_DIV_OFF       (8)
#define I2S_CONFIG_CLK_SEL_DIV_BIT    (1U << 2)
#define I2S_CONFIG_MCLK_SEL_BIT       (1U << 1)

#define I2S_CONFIG_INTERNAL_FRAC_MCLK_BITS \
    (I2S_CONFIG_CLK_SEL_DIV_BIT | I2S_CONFIG_MCLK_SEL_BIT)

#define  I2S_ARG_TO_DATA_WIDTH(X)    (((X) + 1) <<3)

static int s_i2s_init;
static unsigned int s_i2s_cfg;


void drv_i2s_cfg_rx(unsigned int slot_mode, unsigned int master_mode)
{
    unsigned int rx_cfg = s_i2s_cfg;

    if (master_mode == I2S_MODE_MASTER)
    {
        rx_cfg |= 2 | (1<<I2S_CFG_OFFSET_CLK_SEL);
    }
    else
    {
        rx_cfg |= 4 | (1<<26);
        if (master_mode == I2S_MODE_SLAVE)
        {
            I2S_REG_BASE_TOP->cfg |= I2S_CFG_RX_MCLK_SEL;
        }
    }

    rx_cfg |= (slot_mode << I2S_CFG_OFFSET_SLOT_MODE) | (1<<3);
	#ifdef CONFIG_ECW6280
	    rx_cfg |= (1<<1); // increase the rx delay time
	#endif
    if (slot_mode == SLOT_MODE_STEREO)
    {
        I2S_REG_BASE_RX->rx_fifo_cfg = 0;
    }
    else
    {
        I2S_REG_BASE_RX->rx_fifo_cfg = 4;
    }

    I2S_REG_BASE_RX->rx_cfg = rx_cfg;
    I2S_REG_BASE_RX->rx_limit = 1;
    I2S_REG_BASE_RX->rx_ism = 1;
}


void drv_i2s_cfg_tx(unsigned int slot_mode, unsigned int master_mode)
{
    unsigned int tx_cfg = s_i2s_cfg;

    if (master_mode == I2S_MODE_MASTER)
    {
        tx_cfg |= (1<<I2S_CFG_OFFSET_CLK_SEL);
    }
    else
    {
        tx_cfg |= 3;
        if (master_mode == I2S_MODE_SLAVE)
        {
            I2S_REG_BASE_TOP->cfg |= I2S_CFG_TX_MCLK_SEL;
        }
    }

    tx_cfg |= (slot_mode << I2S_CFG_OFFSET_SLOT_MODE) | (1<<3) | (1<<2);

    I2S_REG_BASE_TX->tx_cfg = tx_cfg;
    I2S_REG_BASE_TX->tx_limit = 1;
    I2S_REG_BASE_TX->tx_ism = 1;
}


void drv_i2s_cfg_slot(unsigned int format, unsigned int slot_width, unsigned int data_width)
{
    s_i2s_cfg = (format << I2S_CFG_OFFSET_FORMAT) 
                | ((slot_width - 1) << I2S_CFG_OFFSET_SLOT_WIDTH)
                | ((data_width - 1) << I2S_CFG_OFFSET_DATA_WIDTH);
}


void drv_i2s_cfg_clk(unsigned int clk, unsigned int data_width)
{
    unsigned int val;

    val = CLK_160M/(data_width * 2 * CLK_COEFFICIENT_4);
    val = (val * FRA_BASE_100)/clk;

    WRITE_REG(CHIP_SMU_PD_I2S_CONFIG,
              I2S_CONFIG_INTERNAL_FRAC_MCLK_BITS |
              (READ_REG(CHIP_SMU_PD_I2S_CONFIG) &
               CHIP_SMU_PD_I2S_CONFIG_RX_CLK_FROM_TX_BIT));
    WRITE_REG(CHIP_SMU_PD_I2S_FRA, ((val%FRA_BASE_100) << FRA_DIV_OFF) | FRA_BASE_100);

    val /= FRA_BASE_100;
    val = ((val%2) ? (val/2): (val/2-1));
    WRITE_REG(CHIP_SMU_PD_INTEGER_NUM, val);
}


void drv_i2s_cfg_mclk(unsigned int mclk)
{
    drv_i2s_cfg_clk(mclk/(2*CLK_COEFFICIENT_4), 1);
    I2S_REG_BASE_TOP->cfg &= ~(I2S_CFG_TX_MCLK_SEL | I2S_CFG_RX_MCLK_SEL);
    drv_i2s_rx_en();
}


static int i2s_bus_init(const int *pins)
{
    if (s_i2s_init == 0)
    {
        int ret;
        unsigned int value;

        // en i2s clk
        chip_clk_enable(CLK_I2S_MCLK);
        chip_clk_enable(CLK_I2S_AHB);

        // reset i2s
        value = READ_REG(CHIP_SMU_PD_SOFTWARE_RESET0);
        WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0, value & ~RST_BIT_I2S);
        ret = os_usdelay(10);
        if (ret)
        {
            WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0, value | RST_BIT_I2S);
            chip_clk_disable(CLK_I2S_AHB);
            chip_clk_disable(CLK_I2S_MCLK);
            return ret;
        }
        WRITE_REG(CHIP_SMU_PD_SOFTWARE_RESET0, value | RST_BIT_I2S);

        I2S_REG_BASE_TOP->cfg = 0;
        if (pins)
        {
            /* Same native mux primitive as vendor Board initialization.
             * No MCLK or independent RX clock pad is selected. */
            chip_io_mode_pinmux_cfg(pins[0], FUNC_I2S_TXSCK);
            chip_io_mode_pinmux_cfg(pins[1], FUNC_I2S_TXWS);
            chip_io_mode_pinmux_cfg(pins[2], FUNC_I2S_TXD);
            chip_io_mode_pinmux_cfg(pins[3], FUNC_I2S_RXD);
            for (unsigned int i = 0; i < 4; ++i)
            {
                chip_io_drive_current(pins[i], CHIP_IO_DRIVE_CURRENT_8mA);
            }
            WRITE_REG(CHIP_SMU_PD_I2S_CONFIG,
                      READ_REG(CHIP_SMU_PD_I2S_CONFIG) |
                      CHIP_SMU_PD_I2S_CONFIG_RX_CLK_FROM_TX_BIT);
            ret = 0;
        }
        else
        {
            ret = chip_i2s_pinmux_init();
        }
        if (ret)
        {
            chip_clk_disable(CLK_I2S_AHB);
            chip_clk_disable(CLK_I2S_MCLK);
            return ret;
        }
        s_i2s_init = 1;
        return 0;
    }

    return 1;
}


int drv_i2s_bus_init(void)
{
    return i2s_bus_init(NULL);
}

int drv_i2s_bus_init_four_wire(int txsck, int txws, int txd, int rxd)
{
    const int pins[4] = {txsck, txws, txd, rxd};
    /* Reject before clocks/reset/pins, not midway through initialization. */
    for (unsigned int i = 0; i < 4; ++i)
    {
        if (pins[i] < 0 || pins[i] > 24)
        {
            return -1;
        }
        for (unsigned int j = 0; j < i; ++j)
        {
            if (pins[i] == pins[j])
            {
                return -1;
            }
        }
    }
    return i2s_bus_init(pins);
}

void drv_i2s_bus_release(void)
{
    WRITE_REG(CHIP_SMU_PD_I2S_CONFIG,
              READ_REG(CHIP_SMU_PD_I2S_CONFIG) &
              ~CHIP_SMU_PD_I2S_CONFIG_RX_CLK_FROM_TX_BIT);
    chip_clk_disable(CLK_I2S_AHB);
    chip_clk_disable(CLK_I2S_MCLK);
    s_i2s_init = 0;
    s_i2s_cfg = 0; 
}
