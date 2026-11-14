#include "adc.h"
#include "aux_adc_common.h"
#include "FreeRTOS.h"
#include "oshal.h"


#define DRV_ADC_CHECK_IO( io )                      \
    do {                                            \
        if( (io) > ADC_SIGNAL_MAX )                 \
        {                                           \
            return -1;                              \
        }                                           \
    } while( 0 )


#define DRV_ADC_CHECK_DIFF( a, b )                  \
    do {                                            \
        DRV_ADC_CHECK_IO((a));                      \
        DRV_ADC_CHECK_IO((b));                      \
        if( (a) == (b) )                            \
        {                                           \
            return -1;                              \
        }                                           \
    } while( 0 )


#define DRV_ADC_CHECK_RES( res )                    \
    do {                                            \
        if( (res) == NULL )                         \
        {                                           \
            return -1;                              \
        }                                           \
    } while( 0 )

#define DRV_ADC_IO2SIGNAL(x)              (1<<(x))
#define DRV_ADC_PROT_TICKS                (16)
#define DRV_ADC_MODE_ADC                  (0)
#define DRV_ADC_MODE_OTHERS               (1)

typedef struct _T_DRV_ADC_PROTECT
{
    os_mutex_handle_t mutex;
    unsigned int      prot_tick;
    unsigned int      prot_need;
} T_DRV_ADC_PROTECT;

static T_DRV_ADC_PROTECT s_adc_prot;


static void drv_adc_protect(unsigned int mode)
{
	os_mutex_lock(s_adc_prot.mutex, WAIT_FOREVER);	

    if (!mode && s_adc_prot.prot_need)
    {
        unsigned int tick_diff = os_get_tick_count() - s_adc_prot.prot_tick;
        if (tick_diff < DRV_ADC_PROT_TICKS)
        {
            os_msleep(pdTICKS_TO_MS(DRV_ADC_PROT_TICKS - tick_diff));
        }
    }
}


static void drv_adc_unprotect(unsigned int mode)
{
    if (mode)
    {
        s_adc_prot.prot_tick = os_get_tick_count();
    }

    s_adc_prot.prot_need = mode;
	os_mutex_unlock(s_adc_prot.mutex);
}


void drv_adc_init(void)
{
    drv_aux_adc_init();

	if (s_adc_prot.mutex == NULL)
	{
		s_adc_prot.mutex = os_mutex_create();
	}
}


float drv_adc_vbat(void)
{
	float vout;

    drv_adc_protect(DRV_ADC_MODE_OTHERS);
	drv_aux_adc_vbat_cfg();
	vout = drv_aux_adc_vbat_data();
    drv_adc_unprotect(DRV_ADC_MODE_OTHERS);
	return vout;
}


float drv_adc_tempsensor(void)
{
	float temp;

    drv_adc_protect(DRV_ADC_MODE_OTHERS);
	drv_aux_adc_tempsensor_cfg();
	temp = drv_aux_adc_tempsensor_data();
    drv_adc_unprotect(DRV_ADC_MODE_OTHERS);
	return temp;
}


int drv_adc_read(unsigned int io_num, DRV_ADC_EXPECT_VOLT volt, int *result)
{
    DRV_ADC_CHECK_IO(io_num);
    DRV_ADC_CHECK_RES(result);

    drv_adc_protect(DRV_ADC_MODE_ADC);
	drv_aux_adc_config(DRV_ADC_IO2SIGNAL(io_num), DRV_ADC_INPUT_VREF_BUFFER, volt);
	*result = drv_aux_adc_single();
    drv_adc_unprotect(DRV_ADC_MODE_ADC);
	return 0;
}


int drv_adc_read_differential(unsigned int io_num_a, unsigned int io_num_b,
			DRV_ADC_EXPECT_VOLT volt, int *result)
{
    DRV_ADC_CHECK_DIFF(io_num_a, io_num_b);
    DRV_ADC_CHECK_RES(result);

    drv_adc_protect(DRV_ADC_MODE_ADC);
	drv_aux_adc_config(DRV_ADC_IO2SIGNAL(io_num_a), DRV_ADC_IO2SIGNAL(io_num_b), volt);
	*result = drv_aux_adc_differential();
    drv_adc_unprotect(DRV_ADC_MODE_ADC);
	return 0;
}


// only for test
int drv_adc_read_single(unsigned int signal_a, unsigned int signal_b, DRV_ADC_EXPECT_VOLT volt)
{
    int vout;

    os_mutex_lock(s_adc_prot.mutex, WAIT_FOREVER);   
    drv_aux_adc_config(signal_a, signal_b, volt);
    vout = drv_aux_adc_single();
    os_mutex_unlock(s_adc_prot.mutex);
    return vout;
}

/* MOCE additive checked entry; existing SDK ADC/VBAT/temperature API unchanged. */
#include "moce_adc_checked.h"
int drv_aux_adc_bsp_ready(void);
int drv_aux_adc_bsp_sample(unsigned int channel, int *microvolts);

static int moce_adc_lock_fault;

int drv_adc_bsp_ready(void)
{
    return !moce_adc_lock_fault && s_adc_prot.mutex && drv_aux_adc_bsp_ready() == 0 ? 0 : -3;
}

int drv_adc_bsp_read(unsigned int channel, int *microvolts)
{
    if (channel >= ADC_SIGNAL_MAX || !microvolts)
    {
        return -1;
    }
    if (drv_adc_bsp_ready() != 0)
    {
        return -3;
    }
    if (os_mutex_lock(s_adc_prot.mutex, WAIT_FOREVER) != 0)
    {
        return -2;
    }
    if (s_adc_prot.prot_need)
    {
        unsigned int elapsed = os_get_tick_count() - s_adc_prot.prot_tick;
        if (elapsed < DRV_ADC_PROT_TICKS)
        {
            os_msleep(pdTICKS_TO_MS(DRV_ADC_PROT_TICKS - elapsed));
        }
    }
    int value = 0;
    int result = drv_aux_adc_bsp_sample(channel, &value);
    s_adc_prot.prot_need = DRV_ADC_MODE_ADC;
    if (os_mutex_unlock(s_adc_prot.mutex) != 0)
    {
        moce_adc_lock_fault = 1; /* No silent recovery of broken native lock ownership. */
        return -5;
    }
    if (result == 0)
    {
        *microvolts = value;
    }
    return result;
}
