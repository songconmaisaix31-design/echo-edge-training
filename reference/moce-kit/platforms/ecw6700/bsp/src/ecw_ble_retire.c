#include "ecw_ble_retire.h"
#include "ecw_ble_adv_guard.h"
#include "ecw_ble_stop_trace.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

int ecw_ble_retire_begin_on_host(EcwBspBleGatt *g, const uint16_t *handles, size_t count, bool *waiting)
{
    if (!waiting)
    {
        return BLE_HS_EINVAL;
    }
    *waiting = false;
    if (!g || !g->prepared || (count && !handles) || count > ECW_BLE_RETIRE_LINKS)
    {
        ECW_BLE_STOP_TRACE("retire_arguments", BLE_HS_EINVAL);
        return BLE_HS_EINVAL;
    }
    for (size_t i = 0; i < count; ++i)
    {
        for (size_t j = 0; j < i; ++j)
        {
            if (handles[i] == handles[j])
            {
                return BLE_HS_EINVAL;
            }
        }
    }
    if (g->native_detached)
    {
        /* Idempotent only while the sole owner has kept new work closed. */
        return count ? BLE_HS_EBUSY : 0;
    }
    /* Public peripheral owner reserves slot 0 only. Never stop a foreign slot
     * or destroy its shared GATT database to make our own stop succeed. */
    if (ecw_ble_adv_active_from(1))
    {
        return BLE_HS_EBUSY;
    }
    if (ble_gap_get_adv_state(0))
    {
#if MYNEWT_VAL(BLE_EXT_ADV)
        int result = ble_gap_ext_adv_stop(0);
#else
        int result = ble_gap_adv_stop();
#endif
        if (result != 0 && result != BLE_HS_EALREADY)
        {
            return result;
        }
    }
    if (ecw_ble_adv_active_from(0) || ble_gap_disc_active() || ble_gap_conn_active())
    {
        /* This is a peripheral-only owner; never cancel somebody else's scan
         * or central connection attempt to make its service set mutable. */
        return BLE_HS_EBUSY;
    }
    for (size_t i = 0; i < count; ++i)
    {
        /* Bluetooth reason 0x13 = remote-user termination, same native reason
         * used by P16 Host stop. Return0/EALREADY only admits/is already stopping;
         * neither proves the native connection has disappeared. */
        int result = ble_gap_terminate(handles[i], 0x13u);
        ECW_BLE_STOP_TRACE("terminate", result);
        if (result != 0 && result != BLE_HS_EALREADY && result != BLE_HS_ENOTCONN)
        {
            return result;
        }
    }
    /* Accepted termination is not completion. The sole Host consumer receives
     * DISCONNECT after native connection deletion; retain GATT until then. */
    for (size_t i = 0; i < count; ++i)
    {
        struct ble_gap_conn_desc desc;
        int result = ble_gap_conn_find(handles[i], &desc);
        if (result == 0)
        {
            *waiting = true;
        }
        else if (result != BLE_HS_ENOTCONN)
        {
            *waiting = false;
            return result;
        }
    }
    if (*waiting)
    {
        return 0;
    }
    /* Detach adds the all-advertising-slot guard; native reset checks connections
     * including one not yet reflected in the caller's event cache. */
    int result = ecw_bsp_ble_gatt_detach_on_host(g);
    ECW_BLE_STOP_TRACE("gatt_detach", result);
    return result;
}

/* Existing internal single-step checker: still never calls pending work done. */
int ecw_ble_retire_on_host(EcwBspBleGatt *g, const uint16_t *handles, size_t count)
{
    bool waiting = false;
    int result = ecw_ble_retire_begin_on_host(g, handles, count, &waiting);
    return result ? result : waiting ? BLE_HS_EBUSY : 0;
}
