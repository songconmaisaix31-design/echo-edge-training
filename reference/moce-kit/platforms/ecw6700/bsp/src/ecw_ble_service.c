#include "ecw_ble_service.h"
#include "ecw_ble_scan.h"
#include "ecw_ble_advertising.h"
#include "ecw_ble_peripheral.h"
#include "ecw_ble_stop_trace.h"
#ifdef BSP_ECW_NATIVE_GATT
#include "ecw_ble_gatt.h"
#include "syscfg/syscfg.h"
static bool start_pending, starting;
#endif
#include "bsp_ble_validation.h"
#include "bsp_request_internal.h"
/* P16 build force-includes generated/config.h before all source headers. */
#include "FreeRTOS.h"
#include "task.h"
#if CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#endif
static BspBleState state;
static uint32_t worker_bytes;
static unsigned worker_priority;
static bool exclusive, owned, api_busy, stop_requested, stopping, stop_waiter;
static TaskHandle_t worker;
BspStatus ecw_bsp_ble_check_radio(BspRadio id)
{
    BspStatus status = bsp_delay_ms(0);
    if (status != BSP_OK)
    {
        return status;
    }
    const BspRadioInventory *inventory = bsp_board_get_radio_inventory();
    if (!inventory || (!inventory->radios && inventory->count))
    {
        return BSP_NOT_READY;
    }
    bool first = true;
    for (size_t i = 0; i < inventory->count; ++i)
    {
        bool ble = (inventory->radios[i].hardware_features & BSP_RADIO_BLE) != 0;
        if (inventory->radios[i].id == id)
        {
            return ble && first ? BSP_OK : BSP_UNSUPPORTED;
        }
        if (ble)
        {
            first = false;
        }
    }
    return BSP_INVALID_ARG;
}
BspStatus ecw_bsp_ble_service_configure(uint32_t stack_bytes, unsigned priority, bool sole_owner)
{
    if ((stack_bytes || priority || sole_owner) &&
        (stack_bytes < 4096 || !priority || priority >= configMAX_PRIORITIES || !sole_owner ||
         stack_bytes % sizeof(StackType_t) ||
         (uint64_t)(configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)) !=
             stack_bytes / sizeof(StackType_t)))
    {
        return BSP_INVALID_ARG;
    }
    if (worker || owned || api_busy)
    {
        return BSP_BUSY;
    }
    worker_bytes = stack_bytes;
    worker_priority = priority;
    exclusive = sole_owner;
    return BSP_OK;
}
BspStatus bsp_ble_capabilities(BspRadio id, BspBleCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status == BSP_OK || status == BSP_UNSUPPORTED)
    {
        *out = (BspBleCapabilities){.operations = status == BSP_OK
                                                      ? (ecw_bsp_ble_scan_capability() |
                                                         ecw_bsp_ble_advertising_capability())
                                                      : 0};
#ifdef BSP_ECW_NATIVE_GATT
        if (status == BSP_OK)
        {
            out->operations |= BSP_BLE_PERIPHERAL | BSP_BLE_GATT_SERVER;
            out->max_mtu = MYNEWT_VAL(BLE_ATT_PREFERRED_MTU);
            out->max_connections = MYNEWT_VAL(BLE_MAX_CONNECTIONS) < BSP_ECW_BLE_CONNECTIONS
                                       ? MYNEWT_VAL(BLE_MAX_CONNECTIONS)
                                       : BSP_ECW_BLE_CONNECTIONS;
            out->max_attributes = ECW_BSP_BLE_CHARACTERISTICS;
        }
#endif
        return BSP_OK;
    }
    return status;
}
BspStatus ecw_bsp_ble_api_claim(BspRadio id)
{
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    status = api_busy || stop_requested || stopping || stop_waiter ? BSP_BUSY
             : !owned                                              ? BSP_NOT_READY
                                                                   : BSP_OK;
#ifdef BSP_ECW_NATIVE_GATT
    if (status == BSP_OK)
    {
        BspBleState observed = state;
        ecw_bsp_ble_peripheral_query(&observed);
        status = start_pending || starting         ? BSP_BUSY
                 : observed.phase == BSP_BLE_FAULT ? BSP_IO
                 : observed.phase != BSP_BLE_READY ? BSP_NOT_READY
                                                   : BSP_OK;
    }
#endif
    if (status == BSP_OK)
    {
        api_busy = true;
    }
    bsp_request_unlock(key);
    return status;
}
BspStatus ecw_bsp_ble_api_release(BspError error)
{
    uintptr_t key = bsp_request_lock();
    state.error = error;
    api_busy = false;
    bsp_request_unlock(key);
    return error.code;
}
#ifdef BSP_ECW_NATIVE_GATT
void ecw_bsp_ble_service_fault(BspError error)
{
    uintptr_t key = bsp_request_lock();
    state.phase = BSP_BLE_FAULT;
    state.error = error;
    bsp_request_unlock(key);
}
#endif
void ecw_bsp_ble_service_step(void)
{
    uintptr_t key = bsp_request_lock();
#ifdef BSP_ECW_NATIVE_GATT
    if (starting)
    {
        bsp_request_unlock(key);
        return;
    }
    if (start_pending && !api_busy)
    {
        starting = true;
        bsp_request_unlock(key);
        BspError result = ecw_bsp_ble_peripheral_activate();
        key = bsp_request_lock();
        starting = start_pending = false;
        state.error = result;
        state.phase = result.code == BSP_OK ? BSP_BLE_READY : BSP_BLE_FAULT;
        bsp_request_unlock(key);
        return;
    }
#endif
    if (!stop_requested || api_busy || stopping)
    {
        bsp_request_unlock(key);
        return;
    }
    stopping = true;
    bsp_request_unlock(key);
    BspError error =
        ecw_bsp_ble_scan_capability() ? ecw_bsp_ble_scan_cancel() : (BspError){.code = BSP_OK};
    ECW_BLE_STOP_TRACE("scan_cancel_status", error.code);
    if (error.code == BSP_OK)
    {
        error = ecw_bsp_ble_advertising_cancel();
        ECW_BLE_STOP_TRACE("advertising_cancel_status", error.code);
    }
#ifdef BSP_ECW_NATIVE_GATT
    if (error.code == BSP_OK)
    {
        error = ecw_bsp_ble_peripheral_retire();
        ECW_BLE_STOP_TRACE("peripheral_retire_status", error.code);
    }
#endif
    key = bsp_request_lock();
    stopping = false;
    stop_requested = false;
    state.error = error;
    state.phase = error.code == BSP_OK ? BSP_BLE_OFF : BSP_BLE_FAULT;
    if (error.code == BSP_OK)
    {
        owned = false;
    }
    bsp_request_unlock(key);
}
static void service_task(void *unused)
{
    (void)unused;
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ecw_bsp_ble_service_step();
    }
}
BspStatus bsp_ble_init(BspRadio id, const BspBleService *services, size_t count)
{
    if (!bsp_ble_services_valid(services, count))
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (
#ifndef BSP_ECW_NATIVE_GATT
        count ||
#endif
        !(ecw_bsp_ble_scan_capability() | ecw_bsp_ble_advertising_capability()))
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = bsp_request_lock();
    if (owned || api_busy || stop_requested || stopping || stop_waiter)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!exclusive || !worker_bytes)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    api_busy = true;
    bsp_request_unlock(key);
#if CONFIG_BT_NIMBLE_ENABLED
    if (!ble_hs_synced())
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_NOT_READY});
    }
    if (ble_gap_disc_active() || !ecw_bsp_ble_advertising_available())
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_BUSY});
    }
#endif
#ifdef BSP_ECW_NATIVE_GATT
    status = ecw_bsp_ble_peripheral_prepare(services, count);
    if (status != BSP_OK)
    {
        return ecw_bsp_ble_api_release((BspError){.code = status});
    }
#endif
    if (!worker)
    {
        configSTACK_DEPTH_TYPE words = (configSTACK_DEPTH_TYPE)(worker_bytes / sizeof(StackType_t));
        if (xTaskCreate(service_task, "bsp_ble", words, NULL, worker_priority, &worker) != pdPASS)
        {
#ifdef BSP_ECW_NATIVE_GATT
            ecw_bsp_ble_peripheral_discard_unregistered();
#endif
            return ecw_bsp_ble_api_release((BspError){.code = BSP_NO_RESOURCE});
        }
    }
    key = bsp_request_lock();
    ecw_bsp_ble_scan_reset_locked();
    owned = true;
#ifdef BSP_ECW_NATIVE_GATT
    state = (BspBleState){.phase = BSP_BLE_STARTING};
    start_pending = true;
#else
    state = (BspBleState){.phase = BSP_BLE_READY};
#endif
    api_busy = false;
    bsp_request_unlock(key);
#ifdef BSP_ECW_NATIVE_GATT
    xTaskNotifyGive(worker);
#endif
    return BSP_OK;
}
BspStatus bsp_ble_query(BspRadio id, BspBleState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!(ecw_bsp_ble_scan_capability() | ecw_bsp_ble_advertising_capability()))
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = bsp_request_lock();
    *out = state;
#ifdef BSP_ECW_NATIVE_GATT
    ecw_bsp_ble_peripheral_query(out);
#endif
    bsp_request_unlock(key);
#if CONFIG_BT_NIMBLE_ENABLED
    if (out->phase == BSP_BLE_READY && !ble_hs_synced())
    {
        out->phase = BSP_BLE_STARTING; /* Native observation, not an automatic reset. */
    }
#endif
    return BSP_OK;
}
BspStatus bsp_ble_stop(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!(ecw_bsp_ble_scan_capability() | ecw_bsp_ble_advertising_capability()))
    {
        return BSP_UNSUPPORTED;
    }
    uint32_t started = bsp_time_ms();
    uintptr_t key = bsp_request_lock();
    if (
#ifdef BSP_ECW_NATIVE_GATT
        start_pending || starting ||
#endif
        api_busy || stopping || stop_requested || stop_waiter)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (!owned)
    {
        bsp_request_unlock(key);
        return BSP_OK;
    }
    state.phase = BSP_BLE_STOPPING;
    stop_waiter = true;
    stop_requested = true;
    bsp_request_unlock(key);
    xTaskNotifyGive(worker);
    for (;;)
    {
        key = bsp_request_lock();
        bool done = !stop_requested && !stopping;
        BspError error = state.error;
        bool expired = (uint32_t)(bsp_time_ms() - started) >= timeout;
        if (done || expired)
        {
            stop_waiter = false;
        }
        bsp_request_unlock(key);
        if (done)
        {
            return error.code;
        }
        if (expired)
        {
            return BSP_TIMEOUT;
        }
        vTaskDelay(1);
    }
}
