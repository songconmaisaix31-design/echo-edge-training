#include "bsp_ecw6700_io_config.h"
#include <string.h>
static BspStatus gpio_apply(uint8_t *p, Ecw6700Port *port, const BspIoResource *r,
                            const BspGpioConfig *c)
{
    BspStatus status = bsp_io_gpio_validate(r, c);
    if (status != BSP_OK)
    {
        return status;
    }

    (void)p;
    if (c->speed != BSP_IO_SPEED_NA || c->mode > BSP_IO_OUTPUT_PP)
    {
        return BSP_UNSUPPORTED;
    }
    port->output = c->mode == BSP_IO_OUTPUT_PP;
    port->initial_high = c->initial == BSP_IO_HIGH;
    port->gpio_pull = c->pull == BSP_IO_PULL_NONE ? ECW_GPIO_PULL_NONE
                      : c->pull == BSP_IO_PULL_UP ? ECW_GPIO_PULL_UP
                                                  : ECW_GPIO_PULL_DOWN;

    return BSP_OK;
}
static bool same_pin(const Ecw6700Port *a, const Ecw6700Port *b)
{
    for (size_t i = 0; i < a->pin_count; ++i)
    {
        for (size_t j = 0; j < b->pin_count; ++j)
        {
            if (a->pins[i] == b->pins[j])
            {
                return true;
            }
        }
    }
    return false;
}
BspStatus bsp_ecw6700_io_compose(const Ecw6700Board *source, const BspIoChoice *choices,
                                 size_t count, Ecw6700IoStorage *out)
{
    if (!out || !source || source == &out->board || source->io_composed)
    {
        return BSP_INVALID_ARG;
    }
    out->board.io_composed = false;
    if (!source->board || !source->ports || !source->board->ports ||
        source->count != source->board->count || (count && !choices) || count > out->capacity ||
        (count && (!out->ports || !out->info)) ||
        (source->io_resource_count && !source->io_resources))
    {
        return BSP_INVALID_ARG;
    }
    size_t np = 0, nc = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const BspIoChoice *choice = &choices[i];
        const BspIoResource *r = NULL;
        const Ecw6700Port *src = NULL;
        const BspPortInfo *info = NULL;
        for (size_t j = 0; j < i; ++j)
        {
            if (choices[j].port == choice->port)
            {
                return BSP_INVALID_ARG;
            }
        }
        for (size_t j = 0; j < source->io_resource_count; ++j)
        {
            if (source->io_resources[j].id == choice->port)
            {
                if (r)
                {
                    return BSP_INVALID_ARG;
                }
                r = &source->io_resources[j];
            }
        }
        BspPort source_id = r ? r->source : choice->port;
        for (size_t j = 0; j < source->count; ++j)
        {
            if (source->ports[j].id == source_id)
            {
                src = &source->ports[j];
                info = &source->board->ports[j];
                break;
            }
        }
        if (!src || !info || info->id != src->id || !src->pins || !src->pin_count)
        {
            return BSP_INVALID_ARG;
        }
        Ecw6700Port *p = &out->ports[i];
        *p = *src;
        out->info[i] = *info;
        if ((choice->port >> 8) == BSP_GPIO)
        {
            if (!r || r->source_pin >= src->pin_count || choice->adc_mask ||
                ((src->id >> 8) != BSP_GPIO && (src->id >> 8) != BSP_ADC) || !out->pins ||
                np >= out->pin_capacity)
            {
                return BSP_INVALID_ARG;
            }
            if ((src->id >> 8) == BSP_ADC && choice->gpio.mode != BSP_IO_INPUT)
            {
                return BSP_UNSUPPORTED;
            }
            memset(p, 0, sizeof(*p));
            p->id = choice->port;
            p->pins = &out->pins[np];
            p->pin_count = 1;
            out->pins[np] = src->pins[r->source_pin];
            BspStatus status = gpio_apply(&out->pins[np++], p, r, &choice->gpio);
            if (status != BSP_OK)
            {
                return status;
            }
            out->info[i].id = choice->port;
            out->info[i].name = r->name;
            out->info[i].capabilities = BSP_CAP_BLOCKING;
        }
        else if ((choice->port >> 8) == BSP_ADC)
        {
            size_t channels = src->pin_count;
            if (!bsp_io_gpio_unset(&choice->gpio) || !channels || channels > 16 ||
                !choice->adc_mask || (choice->adc_mask >> channels) || !out->pins)
            {
                return BSP_INVALID_ARG;
            }
            p->pins = &out->pins[np];
            p->pin_count = 0;

            for (size_t j = 0; j < channels; ++j)
            {
                if (choice->adc_mask & (1u << j))
                {
                    if (np >= out->pin_capacity)
                    {
                        return BSP_INVALID_ARG;
                    }
                    out->pins[np++] = src->pins[j];
                    ++p->pin_count;
                }
            }
        }
        else if (choice->adc_mask || !bsp_io_gpio_unset(&choice->gpio) || r)
        {
            return BSP_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (same_pin(p, &out->ports[j]))
            {
                return BSP_BUSY;
            }
        }
    }
    (void)nc;
    out->descriptor = *source->board;
    out->descriptor.ports = out->info;
    out->descriptor.count = count;
    out->board = *source;
    out->board.board = &out->descriptor;
    out->board.ports = out->ports;
    out->board.count = count;
    out->board.io_composed = true;
    return BSP_OK;
}
