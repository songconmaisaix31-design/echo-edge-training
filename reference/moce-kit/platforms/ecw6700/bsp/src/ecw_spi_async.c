#include "ecw_spi_async.h"
#include "ecw_uart_async.h"
#include "ecw_internal.h"
#include "ecw_sdk.h"
#include "bsp_request_internal.h"
#include <string.h>

#include "ecw_storage_internal.h"
typedef Ecw6700SPIStorage SpiSlot;
#define slot (*ecw_spi_storage)

static bool overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    if (!a || !b || !an || !bn)
    {
        return false;
    }
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}

/* Call with the shared IRQ lock held, including cross-UART reservations. */
bool ecw_spi_storage_overlaps(const void *buffer, size_t capacity)
{
    return ecw_spi_storage && (overlaps(buffer, capacity, slot.tx_storage, slot.capacity) ||
           overlaps(buffer, capacity, slot.rx_storage, slot.capacity));
}

int ecw_spi_open(const Ecw6700Port *port, const Ecw6700PortConfig *config)
{
    if (config->spi_mode > 3 || config->spi_bit_order > 1)
        return -2;
    uintptr_t key = ecw_sdk_lock();
    if (config->spi_async_enabled &&
        (ecw_uart_storage_overlaps(config->spi_dma_tx_buffer, config->spi_dma_buffer_bytes) ||
         ecw_uart_storage_overlaps(config->spi_dma_rx_buffer, config->spi_dma_buffer_bytes)))
    {
        ecw_sdk_unlock(key);
        return ECW_SPI_RESOURCE_CONFLICT;
    }
    slot = (SpiSlot){.port = port};
    if (config->spi_async_enabled)
    {
        slot.tx_storage = config->spi_dma_tx_buffer;
        slot.rx_storage = config->spi_dma_rx_buffer;
        slot.capacity = config->spi_dma_buffer_bytes;
    }
    ecw_sdk_unlock(key);
    /* Keep reservations on failed hardware open until successful explicit close. */
    return ecw_sdk_spi_open_config(port->instance, port->pins, config);
}

int ecw_spi_close(const Ecw6700Port *port)
{
    if (slot.request || ecw_spi_dma_active())
    {
        return -3;
    }
    int status = ecw_sdk_spi_close(port->instance, port->pins);
    if (!status)
    {
        uintptr_t key = ecw_sdk_lock();
        slot = (SpiSlot){0};
        ecw_sdk_unlock(key);
    }
    return status;
}

static void retire(void *owner)
{
    SpiSlot *s = owner;
    s->request = NULL;
    s->receive = NULL;
    (void)ecw_finish_basic(s->port->id, s->outcome);
}

static void completed(void *context, BspStatus status, uint32_t native)
{
    SpiSlot *s = context;
    bool receive = s->receive != NULL;
    if (receive && status == BSP_OK)
    {
        memcpy(s->receive, s->rx_storage, s->count);
    }
    s->receive = NULL;
    s->outcome = (EcwIoResult){status, (int32_t)native, status != BSP_OK};
    BspResult result = {.error = {.code = status,
                                  .native_valid = native != 0,
                                  .native_domain = 0x45430000u | BSP_SPI,
                                  .native_code = (int32_t)native},
                        .cleanup_error = {.code = BSP_OK},
                        .tx_done = status == BSP_OK ? s->count : 0,
                        .rx_done = receive && status == BSP_OK ? s->count : 0,
                        .tx_count_valid = status == BSP_OK,
                        .rx_count_valid = receive && status == BSP_OK,
                        .hardware_started = BSP_YES,
                        .wire_complete = status == BSP_OK ? BSP_YES : BSP_UNKNOWN,
                        .buffers_returned = true};
    ecw_record_async(s->port->id, s->outcome);
    (void)bsp_request_publish(s->request, s->generation, &result);
}

BspStatus bsp_spi_transfer_async(BspPort id, const uint8_t *tx, uint8_t *rx, size_t count,
                                 BspRequest *request)
{
    if (!request || !tx || !count || count > UINT16_MAX || (id >> 8) != BSP_SPI)
    {
        return BSP_INVALID_ARG;
    }
    uint32_t missing;
    BspStatus status = bsp_port_require(id, BSP_CAP_ASYNC, &missing);
    if (status != BSP_OK)
    {
        return status;
    }
    const Ecw6700Port *port;
    status = ecw_claim_basic(id, BSP_SPI, &port);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!slot.tx_storage || count > slot.capacity || (rx && !slot.rx_storage))
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_UNSUPPORTED, 0, false});
    }
    /* Caller buffers/request must never alias the exclusive DMA workspace.
     * In-place caller TX/RX is safe: the complete TX copy precedes DMA start.
     */
    if (ecw_spi_storage_overlaps(tx, count) || ecw_spi_storage_overlaps(rx, count) ||
        ecw_spi_storage_overlaps(request, sizeof(*request)))
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_INVALID_ARG, 0, false});
    }
    if (rx && rx != tx && overlaps(tx, count, rx, count))
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_UNSUPPORTED, 0, false});
    }
    status = bsp_request_begin(request, &slot, retire, &slot.generation);
    if (status != BSP_OK)
    {
        return ecw_finish_basic(id, (EcwIoResult){status, 0, false});
    }
    slot.request = request;
    slot.receive = rx;
    slot.count = count;
    slot.fault_reported = false;
    slot.outcome = (EcwIoResult){BSP_OK, 0, false};
    size_t padded = (count + 3u) & ~(size_t)3u;
    memcpy(slot.tx_storage, tx, count);
    memset(slot.tx_storage + count, 0, padded - count);
    int32_t native = 0;
    status = ecw_spi_dma_start(slot.tx_storage, rx ? slot.rx_storage : NULL, count, completed,
                               &slot, &native);
    if (status != BSP_OK)
    {
        slot.outcome = (EcwIoResult){status, native, status == BSP_IO || status == BSP_NOT_READY};
        (void)bsp_request_reject(request, slot.generation);
        bsp_request_end_submit(request);
        return status;
    }
    bsp_request_end_submit(request);
    return BSP_OK;
}

BspStatus ecw_spi_dispatch(size_t *count)
{
    if (!ecw_spi_storage) return BSP_OK;
    uintptr_t key = ecw_sdk_lock();
    BspRequest *request = slot.request;
    EcwSpiDmaState state = ecw_spi_dma_state();
    if (request && state.faulted && !slot.fault_reported)
    {
        slot.fault_reported = true;
        slot.outcome = (EcwIoResult){BSP_IO, (int32_t)state.dma_status, true};
        BspResult result = {.error = {.code = BSP_IO,
                                      .native_valid = true,
                                      .native_domain = 0x45430000u | BSP_SPI,
                                      .native_code = (int32_t)state.dma_status},
                            .cleanup_error = {.code = BSP_UNSUPPORTED},
                            .hardware_started = BSP_YES,
                            .wire_complete = BSP_UNKNOWN,
                            .buffers_returned = false};
        ecw_record_async(slot.port->id, slot.outcome);
        /* Held failures never invoke the terminal callback. This consumes only
         * latched software metadata; it does not poll a peripheral to progress IO.
         */
        (void)bsp_request_publish(request, slot.generation, &result);
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
    return ecw_spi_plan_dispatch(count);
}

void ecw_spi_workspace(uint8_t **tx, uint8_t **rx, size_t *capacity)
{
    *tx = slot.tx_storage;
    *rx = slot.rx_storage;
    *capacity = slot.capacity;
}
