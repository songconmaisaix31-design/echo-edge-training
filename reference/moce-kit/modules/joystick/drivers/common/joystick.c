#include "joystick.h"
BspStatus joystick_init(Joystick *self, const JoystickConfig *config)
{
    if (!self || !config || config->adc == BSP_PORT_NONE ||
        config->button == BSP_PORT_NONE ||
        config->axis_a_channel == config->axis_b_channel ||
        config->conversion_timeout_ms == UINT32_MAX ||
        config->debounce_ms > INT32_MAX) { return BSP_INVALID_ARG; }
    if (self->bound) { return BSP_INVALID_STATE; }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->button, BSP_CAP_BLOCKING, &missing);
    if (status != BSP_OK) { return status; }
    const size_t channels[] = {config->axis_a_channel, config->axis_b_channel};
    for (size_t i = 0; i < 2; ++i) {
        BspAdcCapabilities caps = {0};
        status = bsp_adc_capabilities(config->adc, channels[i], &caps);
        if (status != BSP_OK) { return status; }
        if (!(caps.formats & (1u << BSP_ADC_RAW)) ||
            !(caps.wait_modes & (1u << BSP_ADC_CONVERSION_WAIT)) ||
            caps.resolution_bits != 12 || caps.raw_signed) { return BSP_UNSUPPORTED; }
    }
    *self = (Joystick){.config = *config, .bound = true};
    return BSP_OK;
}
static BspStatus read_axis(const JoystickConfig *config, size_t channel, uint16_t *raw)
{
    const BspAdcReadOptions options = {
        .format = BSP_ADC_RAW, .wait = BSP_ADC_CONVERSION_WAIT,
        .timeout_ms = config->conversion_timeout_ms, .required_resolution_bits = 12,
    };
    BspAdcResult result;
    BspStatus status = bsp_adc_read(config->adc, channel, &options, &result);
    if (status != BSP_OK) { return status; }
    if (result.format != BSP_ADC_RAW || result.resolution_bits != 12 ||
        result.raw_signed || result.value < 0 || result.value > 4095) { return BSP_IO; }
    *raw = (uint16_t)result.value;
    return BSP_OK;
}
BspStatus joystick_read(Joystick *self, JoystickSample *out)
{
    if (!self || !out) { return BSP_INVALID_ARG; }
    if (!self->bound) { return BSP_INVALID_STATE; }
    JoystickSample sample = {0};
    bool high = false;
    BspStatus status = read_axis(&self->config, self->config.axis_a_channel, &sample.axis_a_raw);
    if (status == BSP_OK) {
        status = read_axis(&self->config, self->config.axis_b_channel, &sample.axis_b_raw);
    }
    if (status == BSP_OK) { status = bsp_gpio_read(self->config.button, &high); }
    if (status != BSP_OK) {
        self->tracking = false;
        self->stable_valid = false;
        return status;
    }
    sample.sampled_ms = bsp_time_ms();
    sample.button_raw_pressed = self->config.button_active_low ? !high : high;
    if (!self->tracking || sample.button_raw_pressed != self->candidate) {
        self->tracking = true;
        self->candidate = sample.button_raw_pressed;
        self->candidate_since = sample.sampled_ms;
    }
    if ((uint32_t)(sample.sampled_ms - self->candidate_since) >= self->config.debounce_ms) {
        self->stable = self->candidate;
        self->stable_valid = true;
    }
    sample.button_pressed = self->stable;
    sample.button_valid = self->stable_valid;
    *out = sample;
    return BSP_OK;
}

