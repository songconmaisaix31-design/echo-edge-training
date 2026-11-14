#include "ecw_uart_rx.h"
#include "ecw_sdk.h"
#include "ecw_storage_internal.h"
#include "ecw_uart_async.h"
#include <string.h>
typedef Ecw6700UARTRxStorage RxSlot;
static RxSlot *receiver(unsigned instance)
{
    return instance < 2 && ecw_uart_storage[instance] ? ecw_uart_storage[instance]->background
                                                      : NULL;
}
static void clear_receiver(RxSlot *slot, const Ecw6700Port *port, bool active)
{
    uint8_t *bytes = slot->bytes;
    size_t capacity = slot->capacity;
    *slot = (RxSlot){.bytes = bytes, .capacity = capacity, .port = port, .active = active};
}
void ecw_uart_rx_bind(const Ecw6700Port *port)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(port->instance);
    if (slot)
        clear_receiver(slot, port, false);
    ecw_sdk_unlock(key);
}
bool ecw_uart_rx_active(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    bool active = slot && slot->active;
    ecw_sdk_unlock(key);
    return active;
}
void ecw_uart_rx_detach(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    if (slot && slot->active)
    {
        ecw_uart_rx_hw_stop(instance);
        slot->active = false;
    }
    ecw_sdk_unlock(key);
}
void ecw_uart_rx_forget(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    if (slot)
        clear_receiver(slot, NULL, false);
    ecw_sdk_unlock(key);
}
void ecw_uart_rx_observe(unsigned instance, uint32_t status)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    uint32_t error = status & 0x1eu;
    if (slot && slot->active && error && !slot->native_error)
    {
        slot->native_error = error;
        if (slot->errors != UINT32_MAX)
        {
            ++slot->errors;
        }
        /* Keep logical ownership for explicit recovery; mask a possibly level
         * asserted error IRQ. Do not discard FIFO data or change TX/DMA state. */
        ecw_uart_rx_hw_stop(instance);
        ecw_record_async(slot->port->id, (EcwIoResult){BSP_IO, (int32_t)error, true});
    }
    ecw_sdk_unlock(key);
}
EcwIoResult ecw_uart_rx_result(unsigned instance, EcwIoResult result)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    uint32_t error = slot ? slot->native_error : 0;
    if (error)
    {
        result = (EcwIoResult){BSP_IO, (int32_t)error, true};
    }
    ecw_sdk_unlock(key);
    return result;
}
static void receive(unsigned instance)
{
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(instance);
    /* Drain only already available hardware data; bound work per IRQ. No wait
     * for future bytes, no software timing loop or background polling task. */
    for (unsigned i = 0; slot && slot->active && !slot->native_error && i < BSP_UART_RX_DEPTH; ++i)
    {
        uint32_t status = ecw_uart_line_status(instance);
        if (slot->native_error || !(status & 1u))
        {
            break;
        }
        uint8_t value = ecw_uart_rx_hw_byte(instance);
        if (slot->queued == slot->capacity)
        {
            if (slot->dropped != UINT32_MAX)
            {
                ++slot->dropped;
            }
        }
        else
        {
            slot->bytes[slot->head] = value;
            slot->head = slot->head + 1u == slot->capacity ? 0u : slot->head + 1u;
            ++slot->queued;
        }
    }
    ecw_sdk_unlock(key);
}
static void receive0(void) { receive(0); }
static void receive1(void) { receive(1); }
static BspStatus control(BspPort id, bool start)
{
    if (!ecw_sdk_thread_ready())
    {
        return BSP_INVALID_STATE;
    }
    uint32_t missing;
    BspStatus status = bsp_port_require(id, BSP_CAP_BACKGROUND_RX, &missing);
    if (status != BSP_OK)
    {
        return status;
    }
    const Ecw6700Port *port;
    status = ecw_claim_basic(id, BSP_UART, &port);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = ecw_sdk_lock();
    RxSlot *slot = receiver(port->instance);
    if (!slot || !slot->capacity)
    {
        status = start ? BSP_UNSUPPORTED : BSP_INVALID_STATE;
    }
    else if (slot->active == start)
    {
        status = BSP_INVALID_STATE;
    }
    else if (start)
    {
        /* IRQ is masked by the lock until state is committed; BUSY from the
         * native vector owner check has no side effects and preserves cache. */
        status = ecw_uart_rx_hw_start(port->instance, port->instance ? receive1 : receive0);
        if (status == BSP_OK)
        {
            clear_receiver(slot, port, true);
        }
    }
    else
    {
        ecw_uart_rx_hw_stop(port->instance);
        slot->active = false;
        status = BSP_OK;
    }
    ecw_sdk_unlock(key);
    return ecw_finish_basic(id, (EcwIoResult){status, 0, false});
}
BspStatus bsp_uart_rx_start(BspPort id) { return control(id, true); }
BspStatus bsp_uart_rx_stop(BspPort id) { return control(id, false); }
BspStatus bsp_uart_read_cached(BspPort id, uint8_t *data, size_t capacity, size_t *count)
{
    if (!data || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = ecw_sdk_lock();
    const Ecw6700Port *port;
    bool ready;
    BspStatus status = ecw_uart_inspect(id, &port, &ready);
    if (status == BSP_OK && !ready)
    {
        status = BSP_NOT_READY;
    }
    if (status == BSP_OK)
    {
        RxSlot *slot = receiver(port->instance);
        size_t used = slot ? (capacity < slot->queued ? capacity : slot->queued) : 0;
        for (size_t i = 0; i < used; ++i)
        {
            data[i] = slot->bytes[slot->tail];
            slot->tail = slot->tail + 1u == slot->capacity ? 0u : slot->tail + 1u;
        }
        if (slot)
            slot->queued -= (unsigned)used;
        *count = used;
    }
    ecw_sdk_unlock(key);
    return status;
}
BspStatus bsp_uart_rx_stats(BspPort id, BspUartRxStats *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = ecw_sdk_lock();
    const Ecw6700Port *port;
    bool ready;
    BspStatus status = ecw_uart_inspect(id, &port, &ready);
    if (status == BSP_OK)
    {
        const RxSlot *slot = receiver(port->instance);
        *out = slot ? (BspUartRxStats){slot->queued, slot->dropped, slot->errors, slot->active}
                    : (BspUartRxStats){0};
    }
    ecw_sdk_unlock(key);
    return status;
}
