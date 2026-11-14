/**
*@file     adc.c
*@brief    This part of program is AUX_ADC algorithm Software interface
*@author   wangc
*@data     2024.7.7
*@version  v0.1
*@par Copyright (c):
     eswin inc.
*@par History:
   version:autor,data,desc\n
*/

#include <stdio.h>
#include "adc.h"
#include "aux_adc_common.h"
#include "chip_pinmux.h"
#include "efuse.h"
#include "chip_smupd.h"
#include "oshal.h"
#include "chip_clk_ctrl.h"


/** Define the registers required for aux_adc */
#define AUX_ADC_EN                      CHIP_SMU_PD_AUX_ADC_SEL     ///< :aux_adc enable 
#define AUX_ADC_FIFO_WR_CONFIG          CHIP_SMU_PD_AUX_ADC_FIFO_WR_CFG  ///< :fifo write data configuration signal, control down-sampling rate (re-sampling 2~512 times)
#define AUX_ADC_DATA_OUT                CHIP_SMU_PD_AUX_ADC_OUT    
#define AUX_ADC_FIFO_DATA               CHIP_SMU_PD_AUX_ADC_FIFO_DATA
#define AUX_ADC_GAIN_CAL_FIX            CHIP_SMU_PD_AUX_GAIN_CAL_FIX
#define AUX_ADC_VOS_DATA_FIX            CHIP_SMU_PD_AUX_VOS_DATA_FIX 
#define AUX_ADC_CLR                     CHIP_SMU_PD_AUX_ADC_CLR 


#define ADC_TEMP_SENSOR_TO_TEMP                         -4.3285
//#define ADC_TEMP_SENSOR_TO_TEMP                         -4.0476
#define TEMP_OF_25                                      25


#define DRV_ADC_VREF_BUF_factor             0.390625
#define DRV_ADC_VREF_BUF_OFFSET             800
#ifdef CONFIG_ECW6700
#define DRV_ADC_GET_VREF_BUF(x)         (((float)(x) + DRV_ADC_VREF_BUF_OFFSET)/1000)
#else
#define DRV_ADC_GET_VREF_BUF(x)         (((float)(x) * DRV_ADC_VREF_BUF_factor + DRV_ADC_VREF_BUF_OFFSET)/1000)
#endif

///< :8 kinds split voltage ratio
const float input_divider[8]={0.125,0.25,0.375,0.5,0.625,0.75,0.875,1};


#define DRV_ADC_ARG_TO_CFG(a, b, v)  ((a << 22) | (b<<12) | (v)) 
#define DRV_ADC_CFG_TO_B(x)          (((x)>>12) & 0x3FF)  

#define DRV_ADC_CFG_NONE        0x00000000
#define DRV_ADC_CFG_VBAT        0xEEEEEEEE
#define DRV_ADC_CFG_TEMP        0xFFFFFFFF

typedef struct _T_DRV_ADC_DEV
{
    unsigned int cfg;
    float d0;
    float d1;
    float k;
    float divider_a;
    float divider_b;
    unsigned char delay_cfg;
    unsigned char delay_read;
    unsigned char data_cnt;
    unsigned short efuse_temp_sensor;
    float efuse_calc_val;
} T_DRV_ADC_DEV;

static T_DRV_ADC_DEV s_drv_adc_dev;


static float drv_aux_adc_init_internel(void)
{
    int val;
    float sum_f = 0;

    os_usdelay(s_drv_adc_dev.delay_cfg);

#define DRV_ADC_INIT_READ_CNT       20
    for (int i=0; i<DRV_ADC_INIT_READ_CNT; i++)
    {
        os_usdelay(s_drv_adc_dev.delay_read);
        val = READ_REG(AUX_ADC_DATA_OUT);
        //os_printf(LL_ERR,"unit test adc init, aux_adc dx[%d]: 0x%08x!\r\n", i, val);
        val = (val >> 16) & 0xFFF;
        //os_printf(LL_ERR,"unit test adc init, aux_adc dx[%d]: 0x%08x!\r\n", i, val);
        val -= 2048;
        //os_printf(LL_ERR,"unit test adc init, aux_adc dx[%d]: 0x%08x!\r\n", i, val);
        //os_printf(LL_ERR,"unit test adc init, aux_adc dx[%d]: %f!\r\n", i, (float)val);
        sum_f += (float)(val);
        //os_printf(LL_ERR,"unit test adc init, aux_adc dx[%d]: %f!\r\n", i, sum_f);
    }

    return sum_f/DRV_ADC_INIT_READ_CNT;
}


static float drv_aux_adc_vout(void)
{
    float vout_sum = 0.0;
    float vout_min, vout_max;

#define DRV_ADC_GET_DATA_CNT     20
    for (int i=0; i<DRV_ADC_GET_DATA_CNT; i++)
    {
        int val;
        float vout_val;

        os_usdelay(s_drv_adc_dev.delay_read);

        //os_printf(LL_ERR,"unit test adc get, aux_adc data[%d]: %x!\r\n", i, READ_REG(AUX_ADC_DATA_OUT));
        val = (int)(READ_REG(AUX_ADC_DATA_OUT) >> 16) - 2048;
        vout_val = ((float)val - s_drv_adc_dev.d0)/s_drv_adc_dev.k;
        //os_printf(LL_ERR,"unit test adc get, aux_adc data[%d]: %f!\r\n", i, vout_val);

        if (i==0)
        {
            vout_min = vout_val;
            vout_max = vout_val;
        }

        vout_sum += vout_val;
        vout_min = MIN(vout_min, vout_val);
        vout_max = MAX(vout_max, vout_val);
    }

    vout_sum -= vout_min;   
    vout_sum -= vout_max;   
    vout_sum /= DRV_ADC_GET_DATA_CNT-2;
    //os_printf(LL_ERR,"unit test adc get, aux_adc Vin_adc: %f!\r\n", vout_sum);

    return vout_sum;
}


/************************ adc interface **********************************************/
/************************ adc interface **********************************************/

/**
@brief  single-ended interface function
@return vout: single-ended test output voltage value
*/
int drv_aux_adc_single(void)
{
    T_DRV_ADC_DEV * pDev = &s_drv_adc_dev;
    float vout = drv_aux_adc_vout();

    if (DRV_ADC_CFG_TO_B(pDev->cfg) == DRV_ADC_INPUT_VREF_BUFFER)
    {
        vout = (vout + (float)(pDev->efuse_calc_val) * pDev->divider_b) / pDev->divider_a;
    }
    else
    {
        vout = ((float)(pDev->efuse_calc_val) * pDev->divider_a - vout) / pDev->divider_b;
    }

    return vout * 1000000; // change to uV
}


/**
@brief      differential test interface function
@return     vout: Differential test output voltage value
*/
int drv_aux_adc_differential(void)
{   
    return drv_aux_adc_vout() * 1000000 / s_drv_adc_dev.divider_a;
}   


/**
@brief  calculate battery voltage
*/
float drv_aux_adc_vbat_data(void)
{
    float vbat = drv_aux_adc_vout();
    vbat += (float)(s_drv_adc_dev.efuse_calc_val) * s_drv_adc_dev.divider_b;
    vbat /= s_drv_adc_dev.divider_a;
    //os_printf(LL_ERR,"unit test adc get, aux_adc vbat: %f!\r\n", vbat);
    return vbat;
}

 
int drv_aux_adc_tempsensor_pa(void)
{
    float vout_sum = 0.0;

    for (int i=0; i<s_drv_adc_dev.data_cnt; i++)
    {
        os_usdelay(s_drv_adc_dev.delay_read);
        unsigned int adc_temp = (READ_REG(AUX_ADC_DATA_OUT))>>16;
        //os_printf(LL_ERR,"unit test adc temp, volt_25[%d]: %d!\r\n", i, temp);
        vout_sum += (float)(adc_temp & 0xFFF);
    }

    return (int)(vout_sum / s_drv_adc_dev.data_cnt);
}


float drv_aux_adc_tempsensor_data(void)
{
    float current_temp;

    current_temp = drv_aux_adc_tempsensor_pa() - s_drv_adc_dev.efuse_temp_sensor;
    current_temp = current_temp/ADC_TEMP_SENSOR_TO_TEMP + TEMP_OF_25;
    //os_printf(LL_INFO,"temp now=%f\r\n", current_temp);
    return current_temp;
}

#include "aux_adc_6630.c"
#include "aux_adc_6700.c"


/* Called only while the existing drv_adc mutex is held. No register bypass. */
#include <math.h>
#include <limits.h>
int drv_aux_adc_bsp_ready(void)
{
    return isfinite(s_drv_adc_dev.k) && s_drv_adc_dev.k > 0.0f &&
           isfinite(s_drv_adc_dev.d0) && isfinite(s_drv_adc_dev.efuse_calc_val) &&
           s_drv_adc_dev.efuse_calc_val > 0.0f ? 0 : -3;
}

int drv_aux_adc_bsp_sample(unsigned int channel, int *microvolts)
{
    if (channel >= ADC_SIGNAL_MAX || !microvolts)
    {
        return -1;
    }
    if (drv_aux_adc_bsp_ready() != 0)
    {
        return -3;
    }
    /* The native cache bypasses pinmux for identical configurations. Reapply
     * under the shared mutex after a prior GPIO owner/deinit. */
    s_drv_adc_dev.cfg = DRV_ADC_CFG_NONE;
    if (drv_aux_adc_config(1u << channel, DRV_ADC_INPUT_VREF_BUFFER,
                          DRV_ADC_EXPECT_MAX_VOLT4) != 0)
    {
        return -4;
    }
    float volts = drv_aux_adc_vout();
    volts = (volts + s_drv_adc_dev.efuse_calc_val * s_drv_adc_dev.divider_b) /
            s_drv_adc_dev.divider_a;
    double uv = (double)volts * 1000000.0;
    if (!isfinite(uv) || uv < INT_MIN || uv > INT_MAX)
    {
        return -4;
    }
    *microvolts = (int)uv;
    return 0;
}
