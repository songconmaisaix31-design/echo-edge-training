#include "veml6040.h"
#include <math.h>
static bool valid_it(unsigned value) { return value <= VEML6040_IT_1280_MS; }
static bool valid_mode(Veml6040Mode mode)
{
    return mode == VEML6040_MODE_FORCE || mode == VEML6040_MODE_AUTO;
}
static BspStatus state(const Veml6040 *dev)
{
    if (!dev) return BSP_INVALID_ARG;
    return dev->configured && dev->initialized ? BSP_OK : BSP_INVALID_STATE;
}
static BspStatus write_settings(Veml6040 *dev, const Veml6040Settings *settings,
                                bool trigger)
{
    uint8_t flags = (settings->mode == VEML6040_MODE_FORCE ? 2u : 0u)
                  | (settings->shutdown ? 1u : 0u) | (trigger ? 4u : 0u);
    const uint8_t bytes[2] = {(uint8_t)((settings->integration << 4) | flags), 0};
    BspStatus s = bsp_i2c_mem_write(dev->config.port, VEML6040_ADDRESS,
        VEML6040_REG_CONF, 1, bytes, sizeof bytes, dev->config.timeout_ms);
    if (s != BSP_OK) dev->initialized = false;
    return s;
}
static BspStatus read_word(Veml6040 *dev, uint8_t reg, uint16_t *out)
{
    uint8_t bytes[2];
    BspStatus s = bsp_i2c_mem_read(dev->config.port, VEML6040_ADDRESS, reg, 1,
                                  bytes, sizeof bytes, dev->config.timeout_ms);
    if (s == BSP_OK) *out = (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
    return s;
}
BspStatus veml6040_init(Veml6040 *dev, const Veml6040Config *config)
{
    if (!dev || !config || !valid_it(config->integration) || !config->timeout_ms ||
        config->timeout_ms > INT32_MAX || (config->port >> 8) != BSP_I2C)
        return BSP_INVALID_ARG;
    if (dev->configured) return BSP_INVALID_STATE;
    uint32_t missing;
    BspStatus s = bsp_port_require(config->port, BSP_CAP_BLOCKING | BSP_CAP_RESTART, &missing);
    if (s != BSP_OK) return s;
    dev->config = *config;
    dev->settings = (Veml6040Settings){config->integration, VEML6040_MODE_FORCE, false};
    dev->configured = true;
    dev->initialized = false;
    return veml6040_configure(dev, &dev->settings);
}
BspStatus veml6040_configure(Veml6040 *dev, const Veml6040Settings *settings)
{
    if (!dev || !settings || !valid_it(settings->integration) || !valid_mode(settings->mode))
        return BSP_INVALID_ARG;
    if (!dev->configured) return BSP_INVALID_STATE;
    /* Local copy permits configure(dev, &dev->settings). */
    Veml6040Settings value = *settings;
    BspStatus s = write_settings(dev, &value, false);
    if (s == BSP_OK) {
        dev->settings = value;
        dev->config.integration = value.integration;
        dev->initialized = true;
    }
    return s;
}
BspStatus veml6040_set_integration(Veml6040 *dev, Veml6040Integration integration)
{
    if (!valid_it((unsigned)integration)) return BSP_INVALID_ARG;
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    Veml6040Settings settings = dev->settings;
    settings.integration = (uint8_t)integration;
    return veml6040_configure(dev, &settings);
}
BspStatus veml6040_set_mode(Veml6040 *dev, Veml6040Mode mode)
{
    if (!valid_mode(mode)) return BSP_INVALID_ARG;
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    Veml6040Settings settings = dev->settings;
    settings.mode = mode;
    return veml6040_configure(dev, &settings);
}
BspStatus veml6040_set_shutdown(Veml6040 *dev, bool shutdown)
{
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    Veml6040Settings settings = dev->settings;
    settings.shutdown = shutdown;
    return veml6040_configure(dev, &settings);
}
BspStatus veml6040_sleep(Veml6040 *dev) { return veml6040_set_shutdown(dev, true); }
BspStatus veml6040_wake(Veml6040 *dev) { return veml6040_set_shutdown(dev, false); }
BspStatus veml6040_read_config(Veml6040 *dev, Veml6040ConfigReadback *out)
{
    if (!dev || !out) return BSP_INVALID_ARG;
    if (!dev->configured) return BSP_INVALID_STATE;
    uint16_t raw;
    BspStatus s = read_word(dev, VEML6040_REG_CONF, &raw);
    if (s != BSP_OK) return s;
    uint8_t integration = (uint8_t)((raw >> 4) & 7);
    if ((raw & 0xff88u) || !valid_it(integration)) return BSP_IO;
    Veml6040ConfigReadback result = {
        .settings = {integration, (raw & 2) ? VEML6040_MODE_FORCE : VEML6040_MODE_AUTO,
                     (raw & 1) != 0},
        .trigger = (raw & 4) != 0, .raw = raw
    };
    *out = result;
    return BSP_OK;
}
BspStatus veml6040_trigger(Veml6040 *dev)
{
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    if (dev->settings.shutdown || dev->settings.mode != VEML6040_MODE_FORCE)
        return BSP_INVALID_STATE;
    return write_settings(dev, &dev->settings, true);
}
BspStatus veml6040_wait_conversion(Veml6040 *dev)
{
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    if (dev->settings.shutdown) return BSP_INVALID_STATE;
    uint32_t ms;
    s = veml6040_conversion_wait_ms((Veml6040Integration)dev->settings.integration, &ms);
    return s == BSP_OK ? bsp_delay_ms(ms) : s;
}
BspStatus veml6040_read_channel(Veml6040 *dev, Veml6040Channel channel, uint16_t *out)
{
    if (!out || (unsigned)channel > VEML6040_CHANNEL_WHITE) return BSP_INVALID_ARG;
    BspStatus s = state(dev);
    return s == BSP_OK ? read_word(dev, (uint8_t)(VEML6040_REG_RED + channel), out) : s;
}
BspStatus veml6040_read_rgbw(Veml6040 *dev, Veml6040Sample *out)
{
    if (!out) return BSP_INVALID_ARG;
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    uint16_t values[4];
    for (unsigned i = 0; i < 4; ++i) {
        s = read_word(dev, (uint8_t)(VEML6040_REG_RED + i), &values[i]);
        if (s != BSP_OK) return s;
    }
    *out = (Veml6040Sample){values[0], values[1], values[2], values[3]};
    return BSP_OK;
}
BspStatus veml6040_measure(Veml6040 *dev, Veml6040Sample *out)
{
    if (!out) return BSP_INVALID_ARG;
    BspStatus s = state(dev);
    if (s != BSP_OK) return s;
    if (dev->settings.shutdown) return BSP_INVALID_STATE;
    if (dev->settings.mode == VEML6040_MODE_FORCE) s = veml6040_trigger(dev);
    if (s == BSP_OK) s = veml6040_wait_conversion(dev);
    return s == BSP_OK ? veml6040_read_rgbw(dev, out) : s;
}
BspStatus veml6040_deinit(Veml6040 *dev)
{
    if (!dev) return BSP_INVALID_ARG;
    if (!dev->configured) return BSP_OK;
    Veml6040Settings settings = dev->settings;
    settings.shutdown = true;
    dev->initialized = false;
    BspStatus s = write_settings(dev, &settings, false);
    if (s == BSP_OK) {
        dev->settings = settings;
        dev->configured = false;
    }
    return s;
}
BspStatus veml6040_integration_ms(Veml6040Integration integration, uint32_t *out)
{
    if (!out || !valid_it((unsigned)integration)) return BSP_INVALID_ARG;
    *out = 40u << (unsigned)integration;
    return BSP_OK;
}
BspStatus veml6040_conversion_wait_ms(Veml6040Integration integration, uint32_t *out)
{
    if (!out) return BSP_INVALID_ARG;
    uint32_t ms;
    BspStatus s = veml6040_integration_ms(integration, &ms);
    if (s == BSP_OK) *out = 2u * ms + 10u;
    return s;
}
BspStatus veml6040_green_sensitivity(Veml6040Integration integration, float *out)
{
    if (!out || !valid_it((unsigned)integration)) return BSP_INVALID_ARG;
    *out = 0.25168f / (float)(1u << (unsigned)integration);
    return BSP_OK;
}
BspStatus veml6040_full_scale_lux(Veml6040Integration integration, float *out)
{
    if (!out) return BSP_INVALID_ARG;
    float sensitivity;
    BspStatus s = veml6040_green_sensitivity(integration, &sensitivity);
    if (s == BSP_OK) *out = 65535.0f * sensitivity;
    return s;
}
BspStatus veml6040_lux(uint16_t green, Veml6040Integration integration, float *out)
{
    if (!out) return BSP_INVALID_ARG;
    float sensitivity;
    BspStatus s = veml6040_green_sensitivity(integration, &sensitivity);
    if (s != BSP_OK) return s;
    if (green == UINT16_MAX) return BSP_NOT_READY;
    *out = green * sensitivity;
    return BSP_OK;
}
BspStatus veml6040_saturation_mask(const Veml6040Sample *sample, uint16_t threshold,
                                 uint8_t *mask)
{
    if (!sample || !mask || !threshold) return BSP_INVALID_ARG;
    *mask = (uint8_t)((sample->r >= threshold ? 1 : 0) |
                     (sample->g >= threshold ? 2 : 0) |
                     (sample->b >= threshold ? 4 : 0) |
                     (sample->w >= threshold ? 8 : 0));
    return BSP_OK;
}

BspStatus veml6040_cct_estimate(const Veml6040Sample *sample, float offset, float *kelvin)
{
    if (!sample || !kelvin || !isfinite(offset)) return BSP_INVALID_ARG;
    if (!sample->g || sample->r == UINT16_MAX ||
        sample->g == UINT16_MAX || sample->b == UINT16_MAX) return BSP_NOT_READY;
    float index = ((float)sample->r - (float)sample->b) / sample->g + offset;
    if (!isfinite(index) || index <= 0.0f) return BSP_NOT_READY;
    float result = 4278.6f * powf(index, -1.2455f);
    if (!isfinite(result) || result <= 0.0f) return BSP_NOT_READY;
    *kelvin = result;
    return BSP_OK;
}
