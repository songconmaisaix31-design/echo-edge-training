#include "syn6288e.h"
#include <string.h>
#include <limits.h>

BspStatus syn6288e_init(Syn6288e *device, const Syn6288eConfig *config)
{
    if (!device || !config || !config->timeout_ms || config->timeout_ms > INT32_MAX ||
        (config->port >> 8) != BSP_UART)
    {
        return BSP_INVALID_ARG;
    }
    if (device->initialized)
    {
        return BSP_INVALID_STATE;
    }
    uint32_t missing = 0;
    BspStatus status = bsp_port_require(config->port, BSP_CAP_BLOCKING, &missing);
    if (status == BSP_OK)
    {
        device->config = *config;
        device->has_sent = false;
        device->initialized = true;
    }
    return status;
}

static BspStatus send_frame(Syn6288e *device, uint8_t *frame, size_t length)
{
    if (device->has_sent)
    {
        uint32_t elapsed = bsp_time_ms() - device->last_send_ms;
        if (elapsed < 9u)
        {
            BspStatus status = bsp_delay_ms(9u - elapsed);
            if (status != BSP_OK)
            {
                return status;
            }
        }
    }
    uint8_t checksum = 0;
    for (size_t i = 0; i + 1u < length; ++i)
    {
        checksum ^= frame[i];
    }
    frame[length - 1u] = checksum;
    BspStatus status = bsp_uart_write(device->config.port, frame, length,
                                      device->config.timeout_ms);
    device->last_send_ms = bsp_time_ms();
    device->has_sent = true;
    return status;
}

BspStatus syn6288e_speak_gbk(Syn6288e *device, const uint8_t *text, size_t length)
{
    if (!device || !text || !length || length > SYN6288E_MAX_TEXT)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized)
    {
        return BSP_NOT_READY;
    }
    uint8_t frame[SYN6288E_MAX_TEXT + 6u] = {0xfd, 0, (uint8_t)(length + 3u), 1, 1};
    memcpy(frame + 5, text, length);
    return send_frame(device, frame, length + 6u);
}

BspStatus syn6288e_command(Syn6288e *device, Syn6288eCommand command)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    switch (command)
    {
    case SYN6288E_STOP:
    case SYN6288E_PAUSE:
    case SYN6288E_RESUME:
    case SYN6288E_QUERY:
        break;
    default:
        return BSP_INVALID_ARG;
    }
    if (!device->initialized)
    {
        return BSP_NOT_READY;
    }
    uint8_t frame[] = {0xfd, 0, 2, (uint8_t)command, 0};
    return send_frame(device, frame, sizeof(frame));
}

BspStatus syn6288e_read_reply(Syn6288e *device, uint8_t *reply)
{
    if (!device || !reply)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized)
    {
        return BSP_NOT_READY;
    }
    return bsp_uart_read(device->config.port, reply, 1, device->config.timeout_ms);
}

BspStatus syn6288e_deinit(Syn6288e *device)
{
    if (!device)
    {
        return BSP_INVALID_ARG;
    }
    if (!device->initialized)
    {
        return BSP_NOT_READY;
    }
    memset(device, 0, sizeof(*device));
    return BSP_OK;
}
