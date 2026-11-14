#include "ecw_i2c_async.h"
#include "ecw_i2c_irq.h"
#include "ecw_internal.h"
#include "ecw_uart_async.h"
#include "ecw_sdk.h"
#include "bsp_request_internal.h"
#include "ecw_storage_internal.h"
typedef Ecw6700I2CStorage I2cSlot;
#define slot (*ecw_i2c_storage)

int ecw_i2c_open(const Ecw6700Port *port, const Ecw6700PortConfig *config)
{
    slot = (I2cSlot){.port = port, .enabled = config->i2c_async_enabled};
    return ecw_basic_open(port, config->rate_hz);
}
int ecw_i2c_close(const Ecw6700Port *port)
{
    if (slot.request || ecw_i2c_irq_active())
    {
        return -3;
    }
    int status = ecw_basic_close(port);
    if (!status)
    {
        slot = (I2cSlot){0};
    }
    return status;
}
static void retire(void *owner)
{
    I2cSlot *s = owner;
    s->request = NULL;
    (void)ecw_finish_basic(s->port->id, s->outcome);
}
static void completed(void *context, BspStatus status, uint32_t native, size_t tx_count,
                      bool tx_valid, size_t rx_count)
{
    I2cSlot *s = context;
    s->outcome = (EcwIoResult){status, (int32_t)native, status != BSP_OK};
    BspResult result = {.error = {.code = status,
                                  .native_valid = native != 0,
                                  .native_domain = 0x45430000u | BSP_I2C,
                                  .native_code = (int32_t)native},
                        .cleanup_error = {.code = BSP_OK},
                        .tx_done = tx_count,
                        .tx_count_valid = tx_valid,
                        .rx_done = rx_count,
                        .rx_count_valid = true,
                        .hardware_started = BSP_YES,
                        .wire_complete = status == BSP_OK ? BSP_YES : BSP_UNKNOWN,
                        .buffers_returned = true};
    /* IRQ engine detached all borrowed pointers; IO still faults the port.
     * CPU buffer return is not proof of physical bus recovery. */
    ecw_record_async(s->port->id, s->outcome);
    (void)bsp_request_publish(s->request, s->generation, &result);
}
BspStatus bsp_i2c_write_read_async(BspPort id, uint8_t address, const uint8_t *tx, size_t nt,
                                   uint8_t *rx, size_t nr, BspRequest *request)
{
    if (!request || !tx || !rx || !nt || !nr || nt > UINT16_MAX || nr > UINT16_MAX ||
        address > 127 || (id >> 8) != BSP_I2C)
    {
        return BSP_INVALID_ARG;
    }
    uint32_t missing;
    BspStatus status = bsp_port_require(id, BSP_CAP_ASYNC | BSP_CAP_RESTART, &missing);
    if (status != BSP_OK)
    {
        return status;
    }
    if (nr > 256)
    {
        return BSP_UNSUPPORTED;
    }
    const Ecw6700Port *port;
    status = ecw_claim_basic(id, BSP_I2C, &port);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!slot.enabled)
    {
        return ecw_finish_basic(id, (EcwIoResult){BSP_UNSUPPORTED, 0, false});
    }
    status = bsp_request_begin(request, &slot, retire, &slot.generation);
    if (status != BSP_OK)
    {
        return ecw_finish_basic(id, (EcwIoResult){status, 0, false});
    }
    slot.request = request;
    slot.outcome = (EcwIoResult){BSP_OK, 0, false};
    status = ecw_i2c_irq_start(address, tx, nt, rx, nr, completed, &slot);
    if (status != BSP_OK)
    {
        slot.outcome = (EcwIoResult){status, 0, false};
        (void)bsp_request_reject(request, slot.generation);
        bsp_request_end_submit(request);
        return status;
    }
    bsp_request_end_submit(request);
    return BSP_OK;
}
BspStatus ecw_i2c_dispatch(size_t *count)
{
    if (!ecw_i2c_storage) return BSP_OK;
    uintptr_t key = ecw_sdk_lock();
    BspRequest *request = slot.request;
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
    return BSP_OK;
}
