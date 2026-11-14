#include "ecw_uart_async.h"
#include "bsp_request_internal.h"
#include "ecw_internal.h"
#include "ecw_sdk.h"
#include "ecw_uart_format.h"
#include "ecw_spi_async.h"
#include "ecw_uart_rx.h"
#include <string.h>

#include "ecw_storage_internal.h"
typedef Ecw6700UARTStorage UartSlot;

static bool buffers_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return a && b && an && bn && (x <= y ? y - x < an : x - y < bn);
}
bool ecw_uart_storage_overlaps(const void *buffer, size_t capacity)
{
    if (!buffer || !capacity)
    {
        return false;
    }
    uintptr_t first = (uintptr_t)buffer;
    for (size_t i = 0; i < 2; ++i)
    {
        if (!ecw_uart_storage[i])
            continue;
        UartSlot *slot = ecw_uart_storage[i];
        Ecw6700UARTRxStorage *rx = slot->background;
        if (buffers_overlap(buffer, capacity, slot, sizeof(*slot)) ||
            (rx && (buffers_overlap(buffer, capacity, rx, sizeof(*rx)) ||
                    buffers_overlap(buffer, capacity, rx->bytes, rx->capacity))))
        {
            return true;
        }
        uintptr_t other = (uintptr_t)ecw_uart_storage[i]->storage;
        if (other && (first <= other ? other - first < capacity
                                     : first - other < ecw_uart_storage[i]->capacity))
        {
            return true;
        }
    }
    return false;
}

int ecw_uart_open(const Ecw6700Port *port, const Ecw6700PortConfig *config)
{
    uint32_t lcr;
    if (ecw_uart_format(config, &lcr) != BSP_OK || port->instance > 1 ||
        (config->uart_async_enabled &&
         !ecw_uart_dma_storage_valid(config->uart_dma_buffer, config->uart_dma_buffer_bytes)) ||
        (!config->uart_async_enabled && (config->uart_dma_buffer || config->uart_dma_buffer_bytes)))
    {
        return -2;
    }
    uintptr_t key = ecw_sdk_lock();
    UartSlot *slot = ecw_uart_storage[port->instance];
    if (config->uart_async_enabled)
    {
        if (ecw_spi_storage_overlaps(config->uart_dma_buffer, config->uart_dma_buffer_bytes) ||
            ecw_uart_storage_overlaps(config->uart_dma_buffer, config->uart_dma_buffer_bytes))
        {
            ecw_sdk_unlock(key);
            return ECW_UART_RESOURCE_CONFLICT;
        }
    }
    *slot = (UartSlot){.port = port, .background = slot->background};
    if (config->uart_async_enabled)
    {
        slot->storage = config->uart_dma_buffer;
        slot->capacity = config->uart_dma_buffer_bytes;
    }
    ecw_sdk_unlock(key);
    int result = ecw_basic_open_config(port, config);
    if (result)
    {
        return result; /* Keep reserved storage until explicit successful close. */
    }
    ecw_uart_rx_bind(port);
    if (config->uart_async_enabled)
    {
        ecw_uart_dma_enable(port->instance);
    }
    return 0;
}

int ecw_uart_close(const Ecw6700Port *port)
{
    UartSlot *slot = ecw_uart_storage[port->instance];
    if (slot->request)
    {
        return -3;
    }
    ecw_uart_rx_detach(port->instance);
    int result = ecw_basic_close(port);
    if (!result)
    {
        *slot = (UartSlot){.background = slot->background};
        ecw_uart_rx_forget(port->instance);
    }
    return result;
}

static void retire(void *owner)
{
    UartSlot *slot = owner;
    slot->request = NULL;
    slot->receive = NULL;
    (void)ecw_finish_basic(slot->port->id, slot->outcome);
}

static void completed(void *context, uint32_t line_error, BspEvidence wire)
{
    UartSlot *slot = context;
    bool receive = slot->receive != NULL;
    if (receive && !line_error)
    {
        memmove(slot->receive, slot->storage, slot->count);
    }
    slot->receive = NULL;
    slot->outcome =
        ecw_uart_rx_result(slot->port->instance, (EcwIoResult){line_error ? BSP_IO : BSP_OK,
                                                               line_error, line_error != 0});
    line_error = (uint32_t)slot->outcome.native_status;
    BspResult result = {.error = {.code = slot->outcome.status,
                                  .native_valid = line_error != 0,
                                  .native_domain = 0x45430000u | BSP_UART,
                                  .native_code = line_error},
                        .cleanup_error = {.code = BSP_OK},
                        .tx_done = receive ? 0 : slot->count,
                        .rx_done = receive && !line_error ? slot->count : 0,
                        .tx_count_valid = !receive,
                        .rx_count_valid = receive && !line_error,
                        .hardware_started = BSP_YES,
                        .wire_complete = wire,
                        .buffers_returned = true};
    ecw_record_async(slot->port->id, slot->outcome);
    (void)bsp_request_publish(slot->request, slot->generation, &result);
}

static BspStatus submit(BspPort id, const uint8_t *tx, uint8_t *rx, size_t count,
                        BspRequest *request)
{
    if (!request || (!tx && !rx) || !count || count > UINT16_MAX)
    {
        return BSP_INVALID_ARG;
    }
    uint32_t missing;
    BspStatus supported = bsp_port_require(id, BSP_CAP_ASYNC, &missing);
    if (supported != BSP_OK)
    {
        return supported;
    }
    const Ecw6700Port *port;
    BspStatus status = ecw_claim_basic(id, BSP_UART, &port);
    if (status != BSP_OK)
    {
        return status;
    }
    UartSlot *slot = ecw_uart_storage[port->instance];
    if (rx && ecw_uart_rx_active(port->instance))
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_BUSY, 0, false});
    }
    if (!slot->storage || count > slot->capacity)
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_UNSUPPORTED, 0, false});
    }
    status = bsp_request_begin(request, slot, retire, &slot->generation);
    if (status != BSP_OK)
    {
        return ecw_finish_basic(id, (EcwIoResult){status, 0, false});
    }
    slot->request = request;
    slot->receive = rx;
    slot->count = count;
    slot->outcome = (EcwIoResult){BSP_OK, 0, false};
    slot->fault_reported = false;
    if (tx)
    {
        memmove(slot->storage, tx, count);
    }
    int32_t native_error = 0;
    status = ecw_uart_dma_start(port->instance, slot->storage, count, rx != NULL, completed, slot,
                                &native_error);
    if (status != BSP_OK)
    {
        slot->outcome = (EcwIoResult){status, native_error, false};
        (void)bsp_request_reject(request, slot->generation);
        bsp_request_end_submit(request);
        return status;
    }
    bsp_request_end_submit(request);
    return BSP_OK;
}

BspStatus bsp_uart_read_async(BspPort id, uint8_t *data, size_t count, BspRequest *request)
{
    return submit(id, NULL, data, count, request);
}

BspStatus bsp_uart_write_async_configured(BspPort id, const uint8_t *data, size_t count,
                                          const BspUartTxConfig *config, BspRequest *request)
{
    if (!config ||
        (config->completion != BSP_UART_TX_WIRE_COMPLETE &&
         config->completion != BSP_UART_TX_BUFFER_RELEASED) ||
        !data || !count || count > UINT16_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (config->completion == BSP_UART_TX_WIRE_COMPLETE)
    {
        return ecw_reject_request(id, BSP_UART, request);
    }
    return submit(id, data, NULL, count, request);
}

BspStatus bsp_uart_write_async(BspPort id, const uint8_t *data, size_t count, BspRequest *request)
{
    const BspUartTxConfig config = {.completion = BSP_UART_TX_WIRE_COMPLETE};
    return bsp_uart_write_async_configured(id, data, count, &config, request);
}

BspStatus ecw_uart_dispatch(size_t *count)
{
    for (size_t i = 0; i < 2; ++i)
    {
        if (!ecw_uart_storage[i])
            continue;
        /* Claim notification while the owner's request pointer is locked. A
         * snapshot followed by dispatch_one could race reset/free in another task.
         * callback_running prevents reset until the unlocked callback returns. */
        uintptr_t key = ecw_sdk_lock();
        BspRequest *request = ecw_uart_storage[i]->request;
        EcwUartDmaState state = ecw_uart_dma_state((unsigned)i);
        if (request && state.faulted && !ecw_uart_storage[i]->fault_reported)
        {
            UartSlot *slot = ecw_uart_storage[i];
            slot->fault_reported = true;
            slot->outcome = (EcwIoResult){BSP_IO, (int32_t)state.dma_status, true};
            BspResult result = {.error = {.code = BSP_IO,
                                          .native_valid = true,
                                          .native_domain = 0x45430000u | BSP_UART,
                                          .native_code = (int32_t)state.dma_status},
                                .cleanup_error = {.code = BSP_UNSUPPORTED},
                                .hardware_started = BSP_YES,
                                .wire_complete = BSP_UNKNOWN,
                                .buffers_returned = false};
            ecw_record_async(slot->port->id, slot->outcome);
            /* Publish FAULT_HELD, not a terminal notification. Only reads the
             * IRQ-latched metadata; this is not hardware polling for progress. */
            (void)bsp_request_publish(request, slot->generation, &result);
        }
        bool deliver = request && request->callback_pending && !request->callback_running;
        if (deliver)
        {
            request->callback_pending = false;
            request->callback_running = true;
        }
        ecw_sdk_unlock(key);
        if (deliver)
        {
            request->notify.done(request, request->notify.user);
            key = ecw_sdk_lock();
            request->callback_running = false;
            ecw_sdk_unlock(key);
            ++*count;
        }
    }
    return BSP_OK;
}
