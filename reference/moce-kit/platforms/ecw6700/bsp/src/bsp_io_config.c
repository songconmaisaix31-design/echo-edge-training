#include "bsp_io_config.h"
bool bsp_io_gpio_unset(const BspGpioConfig *c)
{
    return c && !c->mode && !c->pull && !c->initial && !c->speed && !c->native_speed;
}
BspStatus bsp_io_gpio_validate(const BspIoResource *r, const BspGpioConfig *c)
{
    if (!r || !c || c->mode < BSP_IO_INPUT || c->mode > BSP_IO_INPUT_OUTPUT_OD ||
        c->pull < BSP_IO_PULL_NONE || c->pull > BSP_IO_PULL_DOWN || c->initial < BSP_IO_LEVEL_NA ||
        c->initial > BSP_IO_HIGH || c->speed < BSP_IO_SPEED_NA || c->speed > BSP_IO_SPEED_NATIVE)
    {
        return BSP_INVALID_ARG;
    }
    if (!(r->modes & BSP_IO_ALLOW(c->mode)) || !(r->pulls & BSP_IO_ALLOW(c->pull)))
    {
        return BSP_UNSUPPORTED;
    }
    if ((c->mode == BSP_IO_INPUT) != (c->initial == BSP_IO_LEVEL_NA) ||
        (c->speed == BSP_IO_SPEED_NA && c->native_speed) ||
        (c->mode == BSP_IO_INPUT && c->speed != BSP_IO_SPEED_NA))
    {
        return BSP_INVALID_ARG;
    }
    return BSP_OK;
}
BspStatus bsp_io_source_channel(uint32_t mask, size_t index, size_t *source)
{
    if (!source || !mask)
    {
        return BSP_INVALID_ARG;
    }
    for (size_t i = 0; i < 32; ++i)
    {
        if (mask & (1u << i))
        {
            if (index == 0)
            {
                *source = i;
                return BSP_OK;
            }
            --index;
        }
    }
    return BSP_INVALID_ARG;
}
