#include "ntc_thermistor.h"
#include <math.h>
#include <string.h>

static bool valid_config(const NtcThermistorConfig *c)
{
    if (!c || c->port == BSP_PORT_NONE || c->samples == 0 ||
        c->samples > NTC_THERMISTOR_MAX_SAMPLES ||
        (unsigned)c->filter > NTC_MEDIAN ||
        (c->filter == NTC_TRIMMED_MEAN && c->samples < 3) ||
        !isfinite(c->ema_alpha) || c->ema_alpha <= 0 || c->ema_alpha > 1 ||
        !isfinite(c->full_scale) || c->full_scale <= 0 ||
        !isfinite(c->input_gain) || c->input_gain <= 0 || !isfinite(c->input_offset) ||
        !isfinite(c->valid_input_min) || !isfinite(c->valid_input_max) ||
        c->valid_input_min < 0 || c->valid_input_max <= c->valid_input_min ||
        !isfinite(c->fixed_resistance_ohm) || c->fixed_resistance_ohm <= 0 ||
        !isfinite(c->rail_margin) || c->rail_margin <= 0 || c->rail_margin >= 0.5 ||
        (c->topology != NTC_TO_GND && c->topology != NTC_TO_SUPPLY) ||
        (unsigned)c->model > NTC_TABLE ||
        !isfinite(c->temperature_offset_c) || !isfinite(c->minimum_c) ||
        !isfinite(c->maximum_c) || c->minimum_c <= -273.15 ||
        c->maximum_c <= c->minimum_c ||
        (c->read.format != BSP_ADC_RAW && c->read.format != BSP_ADC_MICROVOLTS) ||
        (c->read.wait != BSP_ADC_CONVERSION_WAIT && c->read.wait != BSP_ADC_SDK_NATIVE) ||
        c->read.timeout_ms == UINT32_MAX ||
        (c->read.wait == BSP_ADC_SDK_NATIVE && c->read.timeout_ms != 0) ||
        c->read.required_resolution_bits > 32 ||
        (c->read.format == BSP_ADC_MICROVOLTS && c->read.required_resolution_bits != 0)) {
        return false;
    }
    if (c->model == NTC_BETA) {
        return isfinite(c->r0_ohm) && c->r0_ohm > 0 &&
            isfinite(c->t0_c) && c->t0_c > -273.15 &&
            isfinite(c->beta_kelvin) && c->beta_kelvin > 0;
    }
    if (c->model == NTC_STEINHART_HART) {
        return isfinite(c->sh_a) && isfinite(c->sh_b) && isfinite(c->sh_c) &&
            (c->sh_a != 0 || c->sh_b != 0 || c->sh_c != 0);
    }
    if (c->model == NTC_TABLE) {
        if (!c->table || c->table_count < 2 || c->table_count > NTC_THERMISTOR_MAX_TABLE_POINTS) {
            return false;
        }
        for (size_t i = 0; i < c->table_count; ++i) {
            const NtcThermistorPoint *p = &c->table[i];
            if (!isfinite(p->temperature_c) || p->temperature_c <= -273.15 ||
                !isfinite(p->resistance_ohm) || p->resistance_ohm <= 0 ||
                (i != 0 && (p->temperature_c <= c->table[i-1].temperature_c ||
                            p->resistance_ohm >= c->table[i-1].resistance_ohm))) {
                return false;
            }
        }
    }
    return true;
}

BspStatus ntc_thermistor_resistance_to_c(const NtcThermistorConfig *config,
                                       double resistance_ohm, double *out_c)
{
    if (!out_c || !valid_config(config) || !isfinite(resistance_ohm) || resistance_ohm <= 0) {
        return BSP_INVALID_ARG;
    }
    const NtcThermistorConfig *c = config;
    double inverse_k;
    if (c->model == NTC_BETA) {
        inverse_k = 1 / (c->t0_c + 273.15) +
            (log(resistance_ohm) - log(c->r0_ohm)) / c->beta_kelvin;
    } else if (c->model == NTC_STEINHART_HART) {
        double l = log(resistance_ohm);
        inverse_k = c->sh_a + c->sh_b * l + c->sh_c * l * l * l;
    } else if (c->model == NTC_TABLE) {
        if (resistance_ohm > c->table[0].resistance_ohm ||
            resistance_ohm < c->table[c->table_count-1].resistance_ohm) {
            return BSP_IO;
        }
        size_t i = 1;
        while (i < c->table_count-1 && resistance_ohm < c->table[i].resistance_ohm) {
            ++i;
        }
        const NtcThermistorPoint *a = &c->table[i-1], *b = &c->table[i];
        double fraction = (log(resistance_ohm) - log(a->resistance_ohm)) /
            (log(b->resistance_ohm) - log(a->resistance_ohm));
        inverse_k = (1-fraction)/(a->temperature_c+273.15) +
            fraction/(b->temperature_c+273.15);
    } else {
        return BSP_UNSUPPORTED;
    }
    if (!isfinite(inverse_k) || inverse_k <= 0) {
        return BSP_IO;
    }
    double t = 1 / inverse_k - 273.15 + c->temperature_offset_c;
    if (!isfinite(t) || t <= -273.15) {
        return BSP_IO;
    }
    *out_c = t;
    return BSP_OK;
}

BspStatus ntc_thermistor_c_to_resistance(const NtcThermistorConfig *config,
                                       double temperature_c, double *out_ohm)
{
    if (!out_ohm || !valid_config(config) || !isfinite(temperature_c)) {
        return BSP_INVALID_ARG;
    }
    if (config->model != NTC_BETA && config->model != NTC_TABLE) {
        return BSP_UNSUPPORTED;
    }
    double k = temperature_c - config->temperature_offset_c + 273.15;
    if (!isfinite(k) || k <= 0) {
        return BSP_INVALID_ARG;
    }
    double r;
    if (config->model == NTC_TABLE) {
        double t = temperature_c - config->temperature_offset_c;
        if (t < config->table[0].temperature_c ||
            t > config->table[config->table_count-1].temperature_c) {
            return BSP_IO;
        }
        size_t i = 1;
        while (i < config->table_count-1 && t > config->table[i].temperature_c) {
            ++i;
        }
        const NtcThermistorPoint *a = &config->table[i-1], *b = &config->table[i];
        double fraction = (1/k - 1/(a->temperature_c+273.15)) /
            (1/(b->temperature_c+273.15) - 1/(a->temperature_c+273.15));
        r = exp((1-fraction)*log(a->resistance_ohm) + fraction*log(b->resistance_ohm));
    } else {
        r = config->r0_ohm * exp(config->beta_kelvin *
            (1 / k - 1 / (config->t0_c + 273.15)));
    }
    if (!isfinite(r) || r <= 0) {
        return BSP_IO;
    }
    *out_ohm = r;
    return BSP_OK;
}

static BspStatus convert(const NtcThermistorConfig *c, double input, NtcThermistorReading *r)
{
    if (!valid_config(c) || !isfinite(input)) {
        return BSP_INVALID_ARG;
    }
    double ratio = (input * c->input_gain + c->input_offset) / c->full_scale;
    if (!isfinite(ratio)) {
        return BSP_IO;
    }
    r->filtered = input;
    r->ratio = ratio;
    if (input < c->valid_input_min || input > c->valid_input_max) {
        r->flags |= NTC_ADC_OUTSIDE_RANGE;
    }
    if (ratio <= c->rail_margin) {
        r->flags |= NTC_LOW_RAIL;
        return BSP_OK;
    }
    if (ratio >= 1 - c->rail_margin) {
        r->flags |= NTC_HIGH_RAIL;
        return BSP_OK;
    }
    if (r->flags & NTC_ADC_OUTSIDE_RANGE) {
        return BSP_OK;
    }
    double resistance = c->fixed_resistance_ohm *
        (c->topology == NTC_TO_GND ? ratio / (1 - ratio) : (1 - ratio) / ratio);
    if (!isfinite(resistance) || resistance <= 0) {
        return BSP_IO;
    }
    r->resistance_ohm = resistance;
    r->resistance_valid = true;
    if (c->model == NTC_RESISTANCE_ONLY) {
        return BSP_OK;
    }
    BspStatus s = ntc_thermistor_resistance_to_c(c, resistance, &r->temperature_c);
    if (s != BSP_OK) {
        return s;
    }
    r->temperature_k = r->temperature_c + 273.15;
    r->temperature_f = r->temperature_c * 1.8 + 32;
    if (r->temperature_c < c->minimum_c || r->temperature_c > c->maximum_c) {
        r->flags |= NTC_TEMPERATURE_OUTSIDE_RANGE;
    } else {
        r->temperature_valid = true;
    }
    return BSP_OK;
}

BspStatus ntc_thermistor_convert(const NtcThermistorConfig *config, double input,
                                NtcThermistorReading *out)
{
    if (!out) {
        return BSP_INVALID_ARG;
    }
    NtcThermistorReading r = {0};
    BspStatus s = convert(config, input, &r);
    if (s == BSP_OK) {
        *out = r;
    }
    return s;
}

BspStatus ntc_thermistor_init(NtcThermistor *device, const NtcThermistorConfig *config)
{
    if (!device || !valid_config(config)) {
        return BSP_INVALID_ARG;
    }
    if (device->initialized) {
        return BSP_INVALID_STATE;
    }
    BspAdcCapabilities caps;
    BspStatus s = bsp_adc_capabilities(config->port, config->channel, &caps);
    if (s != BSP_OK) {
        return s;
    }
    if (!(caps.formats & (1u << config->read.format)) ||
        !(caps.wait_modes & (1u << config->read.wait)) ||
        (config->read.format == BSP_ADC_RAW && (caps.raw_signed ||
         caps.resolution_bits == 0 || caps.resolution_bits > 32)) ||
        (config->read.required_resolution_bits != 0 &&
         config->read.required_resolution_bits != caps.resolution_bits)) {
        return BSP_UNSUPPORTED;
    }
    NtcThermistor next = {0};
    next.config = *config;
    next.initialized = true;
    *device = next;
    return BSP_OK;
}

BspStatus ntc_thermistor_read(NtcThermistor *device, NtcThermistorReading *out)
{
    if (!device || !out) {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized) {
        return BSP_NOT_READY;
    }
    const NtcThermistorConfig *c = &device->config;
    double values[NTC_THERMISTOR_MAX_SAMPLES], sum = 0;
    NtcThermistorReading r = {0};
    /* Rail guard observes EACH unsmoothed sample, so averaging/EMA cannot hide
     * an intermittent rail. Preserve diagnostic evidence, reset EMA on rails. */
    uint32_t rail_flags = 0;
    for (unsigned i = 0; i < c->samples; ++i) {
        BspAdcResult sample;
        BspStatus s = bsp_adc_read(c->port, c->channel, &c->read, &sample);
        if (s != BSP_OK) {
            return s;
        }
        if (sample.format != c->read.format || sample.raw_signed || sample.value < 0 ||
            (sample.format == BSP_ADC_RAW && (sample.resolution_bits == 0 ||
             sample.resolution_bits > 32 ||
             (uint64_t)sample.value > ((UINT64_C(1) << sample.resolution_bits) - 1))) ||
            (c->read.required_resolution_bits != 0 &&
             c->read.required_resolution_bits != sample.resolution_bits)) {
            return BSP_IO;
        }
        if (i > 0 && sample.resolution_bits != r.last.resolution_bits) {
            return BSP_IO;
        }
        r.last = sample;
        values[i] = (double)sample.value;
        sum += values[i];
        if (values[i] < c->valid_input_min || values[i] > c->valid_input_max) {
            rail_flags |= NTC_ADC_OUTSIDE_RANGE;
        }
        double ratio = (values[i] * c->input_gain + c->input_offset) / c->full_scale;
        if (!isfinite(ratio)) {
            return BSP_IO;
        }
        if (ratio <= c->rail_margin) {
            rail_flags |= NTC_LOW_RAIL;
        }
        if (ratio >= 1 - c->rail_margin) {
            rail_flags |= NTC_HIGH_RAIL;
        }
    }
    for (unsigned i = 1; i < c->samples; ++i) {
        double v = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > v) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = v;
    }
    r.minimum = values[0];
    r.maximum = values[c->samples - 1];
    r.average = sum / c->samples;
    double input = r.average;
    if (c->filter == NTC_TRIMMED_MEAN) {
        input = (sum - r.minimum - r.maximum) / (c->samples - 2);
    } else if (c->filter == NTC_MEDIAN) {
        input = (values[(c->samples - 1) / 2] + values[c->samples / 2]) / 2;
    }
    if (rail_flags != 0) {
        r.filtered = input;
        r.ratio = (input * c->input_gain + c->input_offset) / c->full_scale;
        r.flags = rail_flags;
        device->have_filter = false;
        *out = r;
        return BSP_OK;
    }
    if (device->have_filter) {
        input = device->filtered + c->ema_alpha * (input - device->filtered);
    }
    BspStatus s = convert(c, input, &r);
    if (s != BSP_OK) {
        return s;
    }
    device->filtered = input;
    device->have_filter = true;
    *out = r;
    return BSP_OK;
}

BspStatus ntc_thermistor_reset_filter(NtcThermistor *device)
{
    if (!device) {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized) {
        return BSP_NOT_READY;
    }
    device->have_filter = false;
    return BSP_OK;
}

BspStatus ntc_thermistor_deinit(NtcThermistor *device)
{
    if (!device) {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized) {
        return BSP_NOT_READY;
    }
    memset(device, 0, sizeof(*device));
    return BSP_OK;
}
