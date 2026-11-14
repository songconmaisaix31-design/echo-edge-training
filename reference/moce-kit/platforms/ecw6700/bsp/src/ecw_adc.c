#include "ecw_adc.h"

BspStatus bsp_adc_capabilities(BspPort id, size_t channel, BspAdcCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    const Ecw6700Port *port;
    uint32_t caps;
    BspStatus status = ecw_adc_descriptor(id, &port, &caps);
    if (status != BSP_OK)
    {
        return status;
    }
    if (channel >= port->pin_count)
    {
        return BSP_INVALID_ARG;
    }
    BspAdcCapabilities result = {0};
    if ((caps & BSP_CAP_BLOCKING) && ecw_adc_available() && ecw_adc_port_valid(port))
    {
        result.formats = 1u << BSP_ADC_MICROVOLTS;
        result.wait_modes = 1u << BSP_ADC_SDK_NATIVE;
    }
    *out = result;
    return BSP_OK;
}
BspStatus bsp_adc_read(BspPort id, size_t channel, const BspAdcReadOptions *options,
                       BspAdcResult *out)
{
    if (!options || !out)
    {
        return BSP_INVALID_ARG;
    }
    const BspAdcReadOptions o = *options;
    if ((o.format != BSP_ADC_RAW && o.format != BSP_ADC_MICROVOLTS) ||
        (o.wait != BSP_ADC_CONVERSION_WAIT && o.wait != BSP_ADC_SDK_NATIVE) ||
        o.required_resolution_bits > 32 || o.timeout_ms == UINT32_MAX ||
        (o.wait == BSP_ADC_SDK_NATIVE && o.timeout_ms != 0) ||
        (o.format == BSP_ADC_MICROVOLTS && o.required_resolution_bits != 0))
    {
        return BSP_INVALID_ARG;
    }
    BspAdcCapabilities caps = {0};
    BspStatus e = bsp_adc_capabilities(id, channel, &caps);
    if (e != BSP_OK)
    {
        return e;
    }
    if (!(caps.formats & (1u << o.format)) || !(caps.wait_modes & (1u << o.wait)) ||
        (o.required_resolution_bits && o.required_resolution_bits != caps.resolution_bits))
    {
        return BSP_UNSUPPORTED;
    }
    const Ecw6700Port *port;
    e = ecw_claim_basic(id, BSP_ADC, &port);
    if (e != BSP_OK)
    {
        return e;
    }
    int microvolts = 0;
    EcwIoResult native = ecw_adc_sample(port->pins[channel], &microvolts);
    e = ecw_finish_basic(id, native);
    if (e == BSP_OK)
    {
        const BspAdcResult result = {
            .format = BSP_ADC_MICROVOLTS,
            .resolution_bits = 0,
            .raw_signed = false,
            .value = microvolts
        };
        *out = result;
    }
    return e;
}
