/* B02P03 F ADC: fixed Board pins, SDK calibration and shared native lock. */
#include "ecw_adc.h"
#include "ecw_sdk.h"
#if defined(BSP_ECW_ADC_CHECKED) && BSP_ECW_ADC_CHECKED && defined(CONFIG_ADC) && defined(CONFIG_ECW6700F)
#include "moce_adc_checked.h"
#define ECW_ADC_ENABLED 1
#else
#define ECW_ADC_ENABLED 0
#endif

bool ecw_adc_available(void)
{
    return ECW_ADC_ENABLED != 0;
}

bool ecw_adc_port_valid(const Ecw6700Port *port)
{
    if (!port || port->instance != 0 || !port->pins || !port->pin_count)
    {
        return false;
    }
    for (size_t i = 0; i < port->pin_count; ++i)
    {
        if (port->pins[i] < 18 || port->pins[i] > 23)
        {
            return false;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (port->pins[j] == port->pins[i])
            {
                return false;
            }
        }
    }
    return true;
}

int ecw_adc_open(const Ecw6700Port *port)
{
#if ECW_ADC_ENABLED
    if (!ecw_adc_port_valid(port))
    {
        return -1;
    }
    /* SDK chip_init owns calibration/mutex. Do not initialize all ADC channels. */
    return drv_adc_bsp_ready();
#else
    (void)port;
    return -3;
#endif
}

int ecw_adc_close(const Ecw6700Port *port)
{
    /* No borrowed buffer or in-flight conversion after synchronous return.
     * Keep the SDK ADC powered for its temperature/VBAT users. */
    for (size_t i = 0; i < port->pin_count; ++i)
    {
        uintptr_t key = ecw_sdk_lock();
        int result = ecw_sdk_gpio_open(port->pins[i], false, false);
        ecw_sdk_unlock(key);
        if (result)
        {
            return result;
        }
    }
    return 0;
}

EcwIoResult ecw_adc_sample(unsigned pin, int *microvolts)
{
#if ECW_ADC_ENABLED
    if (pin < 18 || pin > 23 || !microvolts)
    {
        return (EcwIoResult){BSP_INVALID_ARG, 0, false};
    }
    int result = drv_adc_bsp_read(23u - pin, microvolts);
    return (EcwIoResult){result ? BSP_IO : BSP_OK, result,
                         result == -3 || result == -5};
#else
    (void)pin;
    (void)microvolts;
    return (EcwIoResult){BSP_UNSUPPORTED, 0, false};
#endif
}
