#include "soil_moisture.h"
#include <limits.h>
#include <string.h>

static bool in_domain(const SoilMoistureConfig *c, const BspAdcCapabilities *caps, int64_t v)
{
    if (c->adc.format == BSP_ADC_MICROVOLTS) return v >= 0 && v <= INT32_MAX;
    unsigned bits = caps->resolution_bits;
    if (bits == 0u || bits > 32u) return false;
    int64_t low = caps->raw_signed ? -(INT64_C(1) << (bits-1u)) : 0;
    int64_t high = caps->raw_signed ? (INT64_C(1) << (bits-1u))-1 : (INT64_C(1) << bits)-1;
    return v >= low && v <= high;
}
static bool endpoints(const SoilMoistureConfig *c, const BspAdcCapabilities *caps, int64_t dry, int64_t wet)
{
    return dry != wet && in_domain(c,caps,dry) && in_domain(c,caps,wet);
}
BspStatus soil_moisture_init(SoilMoisture *d, const SoilMoistureConfig *c)
{
    if (!d || !c) return BSP_INVALID_ARG;
    if (d->state.initialized) return BSP_INVALID_STATE;
    if ((c->adc.format != BSP_ADC_RAW && c->adc.format != BSP_ADC_MICROVOLTS) ||
        (c->adc.wait != BSP_ADC_CONVERSION_WAIT && c->adc.wait != BSP_ADC_SDK_NATIVE) ||
        c->adc.required_resolution_bits > 32u || c->samples == 0u || c->samples > 64u ||
        c->sample_gap_ms > 1000u ||
        (c->adc.wait == BSP_ADC_SDK_NATIVE && c->adc.timeout_ms != 0u) ||
        (c->adc.wait == BSP_ADC_CONVERSION_WAIT && (c->adc.timeout_ms == 0u || c->adc.timeout_ms > INT32_MAX)) ||
        (c->adc.format == BSP_ADC_MICROVOLTS && c->adc.required_resolution_bits != 0u)) return BSP_INVALID_ARG;
    uint32_t missing = 0;
    BspStatus s = bsp_port_require(c->port,BSP_CAP_BLOCKING,&missing);
    if (s != BSP_OK) return s;
    BspAdcCapabilities caps = {0};
    s = bsp_adc_capabilities(c->port,c->channel,&caps);
    if (s != BSP_OK) return s;
    if (!(caps.formats & (1u << c->adc.format)) || !(caps.wait_modes & (1u << c->adc.wait)) ||
        (c->adc.format == BSP_ADC_RAW && (caps.resolution_bits == 0u || caps.resolution_bits > 32u)) ||
        (c->adc.required_resolution_bits != 0u && c->adc.required_resolution_bits != caps.resolution_bits)) return BSP_UNSUPPORTED;
    if (c->calibrated && !endpoints(c,&caps,c->dry,c->wet)) return BSP_INVALID_ARG;
    SoilMoisture result = {0};
    result.config = *c;
    result.capabilities = caps;
    result.state.initialized = true;
    result.state.last_error = BSP_OK;
    *d = result;
    return BSP_OK;
}
static BspStatus failed(SoilMoisture *d, BspStatus status)
{
    d->state.last_valid = false;
    d->state.last_error = status;
    return status;
}
BspStatus soil_moisture_read(SoilMoisture *d, SoilMoistureSample *out)
{
    if (!d) return BSP_INVALID_ARG;
    if (!out) return failed(d,BSP_INVALID_ARG);
    if (!d->state.initialized) return failed(d,BSP_NOT_READY);
    SoilMoistureSample result = {0};
    result.format = d->config.adc.format;
    result.resolution_bits = result.format == BSP_ADC_RAW ? d->capabilities.resolution_bits : 0u;
    result.raw_signed = result.format == BSP_ADC_RAW && d->capabilities.raw_signed;
    result.samples = d->config.samples;
    int64_t sum = 0;
    for (unsigned i=0; i<d->config.samples; ++i) {
        BspAdcResult sample = {0};
        BspStatus s = bsp_adc_read(d->config.port,d->config.channel,&d->config.adc,&sample);
        if (s != BSP_OK) return failed(d,s);
        if (sample.format != result.format || sample.resolution_bits != result.resolution_bits ||
            sample.raw_signed != result.raw_signed || !in_domain(&d->config,&d->capabilities,sample.value)) return failed(d,BSP_IO);
        if (i == 0u || sample.value < result.minimum) result.minimum = sample.value;
        if (i == 0u || sample.value > result.maximum) result.maximum = sample.value;
        sum += sample.value; /* <=64 x 32-bit domain: fits int64_t */
        if (i+1u < d->config.samples && d->config.sample_gap_ms != 0u) {
            s = bsp_delay_ms(d->config.sample_gap_ms);
            if (s != BSP_OK) return failed(d,s);
        }
    }
    result.mean = sum / d->config.samples;
    if (d->config.calibrated) {
        int64_t span = d->config.wet - d->config.dry;
        int64_t n = result.mean - d->config.dry;
        if (span < 0) { span = -span; n = -n; }
        result.clipped = n < 0 || n > span;
        if (n < 0) n = 0;
        if (n > span) n = span;
        result.moisture_bp = (uint16_t)((n * 10000 + span/2) / span);
        result.moisture_valid = true;
    }
    *out = result;
    d->state.last_valid = true;
    d->state.last_error = BSP_OK;
    if (d->state.successful_reads != UINT32_MAX) ++d->state.successful_reads;
    return BSP_OK;
}
BspStatus soil_moisture_calibrate(SoilMoisture *d, int64_t dry, int64_t wet)
{
    if (!d) return BSP_INVALID_ARG;
    if (!d->state.initialized) return BSP_NOT_READY;
    if (!endpoints(&d->config,&d->capabilities,dry,wet)) return BSP_INVALID_ARG;
    d->config.dry=dry; d->config.wet=wet; d->config.calibrated=true;
    d->state.last_valid=false;
    return BSP_OK;
}
BspStatus soil_moisture_clear_calibration(SoilMoisture *d)
{
    if (!d) return BSP_INVALID_ARG;
    if (!d->state.initialized) return BSP_NOT_READY;
    d->config.calibrated=false; d->config.dry=0; d->config.wet=0;
    d->state.last_valid=false;
    return BSP_OK;
}
BspStatus soil_moisture_query(const SoilMoisture *d, SoilMoistureState *out)
{
    if (!d || !out) return BSP_INVALID_ARG;
    *out=d->state;
    return BSP_OK;
}
BspStatus soil_moisture_deinit(SoilMoisture *d)
{
    if (!d) return BSP_INVALID_ARG;
    memset(d,0,sizeof(*d));
    return BSP_OK;
}
