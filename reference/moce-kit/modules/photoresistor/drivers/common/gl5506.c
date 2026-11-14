#include "gl5506.h"
#include <math.h>

static bool config_valid(const Gl5506Config *config)
{
    if (!config || config->sample_count == 0u ||
        config->sample_count > GL5506_MAX_SAMPLES || config->valid_min >= config->valid_max) {
        return false;
    }
    if (!((config->wait == BSP_ADC_SDK_NATIVE && config->timeout_ms == 0u) ||
          (config->wait == BSP_ADC_CONVERSION_WAIT && config->timeout_ms != UINT32_MAX))) {
        return false;
    }
    if (config->lux_enabled &&
        (!isfinite(config->r10_ohms) ||
         config->r10_ohms <= 0.0 || !isfinite(config->gamma) || config->gamma <= 0.0 ||
         config->supply_uv == 0u || config->pullup_ohms == 0u)) {
        return false;
    }
    if (config->format == BSP_ADC_RAW) {
        if (config->resolution_bits == 0u || config->resolution_bits > 32u ||
            (config->raw_reference_uv == 0u &&
             (config->supply_uv != 0u || config->pullup_ohms != 0u || config->lux_enabled)) ||
            (config->raw_reference_uv != 0u &&
             (config->supply_uv == 0u || config->pullup_ohms == 0u))) {
            return false;
        }
        return config->valid_max <= ((UINT64_C(1) << config->resolution_bits) - 1u);
    }
    return config->format == BSP_ADC_MICROVOLTS && config->resolution_bits == 0u &&
           config->valid_min > 0u && config->valid_max < config->supply_uv &&
           config->pullup_ohms > 0u && config->raw_reference_uv == 0u;
}

BspStatus gl5506_init(Gl5506 *device, const Gl5506Config *config)
{
    if (!device || !config_valid(config)) {
        return BSP_INVALID_ARG;
    }
    if (device->initialized) {
        return BSP_INVALID_STATE;
    }
    BspAdcCapabilities caps = {0};
    BspStatus status = bsp_adc_capabilities(config->port, config->channel_index, &caps);
    if (status != BSP_OK) {
        return status;
    }
    if (!(caps.formats & (1u << config->format)) ||
        !(caps.wait_modes & (1u << config->wait)) ||
        (config->format == BSP_ADC_RAW &&
         (caps.raw_signed || caps.resolution_bits != config->resolution_bits))) {
        return BSP_UNSUPPORTED;
    }
    const Gl5506 next = {.config = *config, .initialized = true};
    *device = next;
    return BSP_OK;
}

BspStatus gl5506_read_diagnostic(const Gl5506 *device, Gl5506Reading *out,
                                 Gl5506Diagnostic *diagnostic)
{
    if (diagnostic) {
        *diagnostic = (Gl5506Diagnostic){0};
    }
    if (!device || !out) {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized) {
        return BSP_NOT_READY;
    }
    const Gl5506Config *config = &device->config;
    if (!config_valid(config)) {
        return BSP_INVALID_ARG;
    }
    const BspAdcReadOptions options = {.format = config->format, .wait = config->wait,
        .timeout_ms = config->timeout_ms, .required_resolution_bits = config->resolution_bits};
    uint64_t total = 0u;
    uint32_t sample_min = UINT32_MAX, sample_max = 0u;
    bool outside_window = false;
    for (uint8_t i = 0u; i < config->sample_count; ++i) {
        BspAdcResult sample = {0};
        BspStatus status = bsp_adc_read(config->port, config->channel_index, &options, &sample);
        if (status != BSP_OK) {
            if (diagnostic) {
                diagnostic->reason = GL5506_DIAG_ADC;
                diagnostic->sample_index = i;
            }
            return status;
        }
        if (sample.format != config->format || sample.raw_signed ||
            sample.resolution_bits != config->resolution_bits ||
            sample.value < 0 || sample.value > UINT32_MAX ||
            (config->format == BSP_ADC_RAW &&
             (uint64_t)sample.value > ((UINT64_C(1) << config->resolution_bits) - 1u))) {
            if (diagnostic) {
                *diagnostic = (Gl5506Diagnostic){GL5506_DIAG_FORMAT, i, sample};
            }
            return BSP_IO;
        }
        if (sample.value < config->valid_min || sample.value > config->valid_max) {
            outside_window = true;
            if (diagnostic) {
                *diagnostic = (Gl5506Diagnostic){GL5506_DIAG_WINDOW, i, sample};
            }
            if (!config->report_out_of_window) {
                return BSP_IO;
            }
        }
        if ((uint32_t)sample.value < sample_min) {
            sample_min = (uint32_t)sample.value;
        }
        if ((uint32_t)sample.value > sample_max) {
            sample_max = (uint32_t)sample.value;
        }
        total += (uint64_t)sample.value;
    }
    Gl5506Reading result = {.format = config->format, .resolution_bits = config->resolution_bits,
        .value = (uint32_t)((total + config->sample_count / 2u) / config->sample_count)};
    result.sample_min = sample_min;
    result.sample_max = sample_max;
    result.outside_window = outside_window;
    double voltage = 0.0;
    if (config->format == BSP_ADC_MICROVOLTS) {
        voltage = result.value;
    } else if (config->raw_reference_uv != 0u && sample_min > 0u &&
               sample_max < ((UINT64_C(1) << config->resolution_bits) - 1u)) {
        voltage = (double)result.value * config->raw_reference_uv /
                  ((UINT64_C(1) << config->resolution_bits) - 1u);
    }
    if (!outside_window && voltage > 0.0 && voltage < config->supply_uv) {
        result.resistance_ohms = (double)config->pullup_ohms * voltage /
                                 (config->supply_uv - voltage);
        result.resistance_valid = true;
        if (config->lux_enabled) {
            const double lux = 10.0 * pow(config->r10_ohms / result.resistance_ohms,
                                           1.0 / config->gamma);
            if (isfinite(lux) && lux > 0.0) {
                result.illuminance_lux = lux;
                result.illuminance_valid = true;
            }
        }
    }
    *out = result;
    return BSP_OK;
}

BspStatus gl5506_read(const Gl5506 *device, Gl5506Reading *out)
{
    return gl5506_read_diagnostic(device, out, NULL);
}
