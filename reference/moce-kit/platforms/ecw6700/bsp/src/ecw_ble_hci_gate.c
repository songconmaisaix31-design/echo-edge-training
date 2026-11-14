#include "ecw_ble_hci_gate.h"
#include "bsp_request_internal.h"
#include <limits.h>
static hci_ll_tx_func host_sink;
static uint32_t in_flight;
static bool installed, enabled, installing;

static void receive(uint8_t type, uint8_t *packet, uint16_t length)
{
    uintptr_t key = bsp_request_lock();
    if (!installed || !enabled || in_flight == UINT32_MAX)
    {
        bsp_request_unlock(key);
        return;
    }
    ++in_flight;
    hci_ll_tx_func sink = host_sink;
    bsp_request_unlock(key);
    /* No BSP lock across SDK processing; no caller-owned state retained. */
    sink(type, packet, length);
    key = bsp_request_lock();
    --in_flight;
    bsp_request_unlock(key);
}
BspStatus ecw_ble_hci_gate_install(hci_ll_tx_func sink)
{
    if (!sink)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (installed || installing)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    installing = true;
    host_sink = sink;
    bsp_request_unlock(key);
    /* Fixed native void API, not a hardware success acknowledgement. */
    hci_adapt_register_ll_tx_cb(receive);
    key = bsp_request_lock();
    installed = true;
    installing = false;
    bsp_request_unlock(key);
    return BSP_OK;
}
BspStatus ecw_ble_hci_gate_enable(void)
{
    uintptr_t key = bsp_request_lock();
    BspStatus result = !installed  ? BSP_NOT_READY
                       : enabled   ? BSP_INVALID_STATE
                       : in_flight ? BSP_BUSY
                                   : BSP_OK;
    if (result == BSP_OK)
    {
        enabled = true;
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_ble_hci_gate_close(void)
{
    uintptr_t key = bsp_request_lock();
    BspStatus result = installed ? BSP_OK : BSP_NOT_READY;
    if (result == BSP_OK)
    {
        enabled = false;
    }
    bsp_request_unlock(key);
    return result;
}
BspStatus ecw_ble_hci_gate_quiet(bool *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    BspStatus result = installed ? BSP_OK : BSP_NOT_READY;
    if (result == BSP_OK)
    {
        *out = !enabled && !in_flight;
    }
    bsp_request_unlock(key);
    return result;
}
