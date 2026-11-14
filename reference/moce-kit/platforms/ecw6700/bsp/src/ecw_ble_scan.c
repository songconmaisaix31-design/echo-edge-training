#include "ecw_ble_scan.h"
#include "ecw_ble_service.h"
#include "bsp_ble_validation.h"
#include "bsp_request_internal.h"
/* P16 build force-includes generated/config.h before all source headers. */
#include <string.h>

#include "syscfg/syscfg.h"
#if CONFIG_BT_NIMBLE_ENABLED && MYNEWT_VAL(BLE_EXT_ADV) &&                                         \
    (MYNEWT_VAL(BLE_ROLE_OBSERVER) || MYNEWT_VAL(BLE_ROLE_CENTRAL)) &&                             \
    (MYNEWT_VAL(BLE_ROLE_CENTRAL) || MYNEWT_VAL(BLE_ROLE_PERIPHERAL))
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_id.h"
#include "bluetooth.h"
#define SCAN_REPORTS 16u
typedef struct
{
    BspBleScanState state;
    BspBleScanReport reports[SCAN_REPORTS];
    unsigned head, count;
} Scan;
static Scan scan;
static uintptr_t serial;
static bool admitting;

static BspError native_error(int rc)
{
    BspStatus status = rc == 0                                        ? BSP_OK
                       : rc == BLE_HS_ENOMEM                          ? BSP_NO_RESOURCE
                       : rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY  ? BSP_BUSY
                       : rc == BLE_HS_EINVAL || rc == BLE_HS_EMSGSIZE ? BSP_INVALID_ARG
                       : rc == BLE_HS_ENOTSUP                         ? BSP_UNSUPPORTED
                       : rc == BLE_HS_ETIMEOUT                        ? BSP_TIMEOUT
                       : rc == BLE_HS_ENOTSYNCED                      ? BSP_INVALID_STATE
                                                                      : BSP_IO;
    return (BspError){
        .code = status, .native_valid = true, .native_domain = 0x4e494d42u, .native_code = rc};
}
static void dropped(void)
{
    if (scan.state.dropped_reports != UINT32_MAX)
    {
        ++scan.state.dropped_reports;
    }
}
static int report(struct ble_gap_event *event, void *cookie)
{
    /* Native callback argument is an immutable, never-reused ECW uintptr token;
     * it is not a pointer to caller memory and is never dereferenced. */
    uintptr_t key = bsp_request_lock();
    if (!scan.state.active || scan.state.operation != (uintptr_t)cookie)
    {
        bsp_request_unlock(key);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE)
    {
        scan.state.active = false;
        scan.state.error = native_error(event->disc_complete.reason);
    }
    else if (event->type == BLE_GAP_EVENT_EXT_DISC)
    {
        const struct ble_gap_ext_disc_desc *in = &event->ext_disc;
        if (!(in->props & BLE_HCI_ADV_LEGACY_MASK) ||
            in->data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE ||
            in->length_data > BSP_BLE_SCAN_DATA_MAX || (!in->data && in->length_data) ||
            in->addr.type > 3 || in->legacy_event_type > 4 || scan.count == SCAN_REPORTS)
        {
            dropped();
        }
        else
        {
            BspBleScanReport *out = &scan.reports[(scan.head + scan.count) % SCAN_REPORTS];
            *out = (BspBleScanReport){.address_type = (BspBleAddressType)in->addr.type,
                                      .type = (BspBleReportType)in->legacy_event_type,
                                      .rssi_dbm = in->rssi,
                                      .rssi_valid = in->rssi != 127,
                                      .size = in->length_data};
            for (unsigned i = 0; i < 6; ++i)
            {
                out->address[i] = in->addr.val[5 - i];
            }
            if (in->length_data)
            {
                memcpy(out->data, in->data, in->length_data);
            }
            ++scan.count;
        }
    }
    bsp_request_unlock(key);
    return 0;
}
uint32_t ecw_bsp_ble_scan_capability(void)
{
    return BSP_BLE_SCAN;
}
void ecw_bsp_ble_scan_reset_locked(void)
{
    memset(&scan, 0, sizeof(scan));
    admitting = false;
}
void ecw_bsp_ble_scan_retire(void)
{
    uintptr_t key = bsp_request_lock();
    if (scan.state.active)
    {
        scan.state.active = false;
        scan.state.error = (BspError){.code = BSP_INVALID_STATE};
    }
    bsp_request_unlock(key);
}
BspStatus bsp_ble_scan_start(BspRadio id, const BspBleScanConfig *config, uint64_t *operation)
{
    if (!operation || !bsp_ble_scan_config_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (config->duration_ms % 10 || config->duration_ms / 10 > UINT16_MAX ||
        config->interval_us % 625 || config->window_us % 625 || config->window_us / 625 < 4 ||
        config->interval_us / 625 > 0x4000)
    {
        return BSP_UNSUPPORTED;
    }
    status = ecw_bsp_ble_api_claim(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    if (scan.state.active || serial == UINTPTR_MAX)
    {
        status = scan.state.active ? BSP_BUSY : BSP_NO_RESOURCE;
        bsp_request_unlock(key);
        return ecw_bsp_ble_api_release((BspError){.code = status});
    }
    Scan previous = scan;
    uintptr_t token = ++serial;
    scan = (Scan){.state = {.operation = token, .active = true}};
    admitting = true;
    bsp_request_unlock(key);
    struct ble_gap_ext_disc_params params = {.type = ECR_SCAN_TYPE_OBSERVER,
                                             .itvl = (uint16_t)(config->interval_us / 625),
                                             .window = (uint16_t)(config->window_us / 625),
                                             .passive = !config->active};
    uint8_t address_type = 0;
    int rc = config->active ? ble_hs_id_infer_auto(0, &address_type) : 0;
    if (!rc)
    {
        /* P16 extended API uses 10 ms units. Its legacy convenience wrapper
         * forwards a millisecond argument without this conversion. */
        rc =
            ble_gap_ext_disc(address_type, (uint16_t)(config->duration_ms / 10), 0,
                             config->filter_duplicates, 0, 0, &params, NULL, report, (void *)token);
    }
    key = bsp_request_lock();
    if (rc)
    {
        scan = previous;
    }
    else
    {
        *operation = token;
    }
    admitting = false;
    bsp_request_unlock(key);
    return ecw_bsp_ble_api_release(native_error(rc));
}
BspStatus bsp_ble_scan_query(BspRadio id, uint64_t operation, BspBleScanState *out)
{
    if (!operation || !out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    status = admitting ? BSP_BUSY : operation != scan.state.operation ? BSP_INVALID_ARG : BSP_OK;
    if (status == BSP_OK)
    {
        *out = scan.state;
    }
    bsp_request_unlock(key);
    return status;
}
BspStatus bsp_ble_scan_read(BspRadio id, uint64_t operation, BspBleScanReport *out, size_t capacity,
                            size_t *count)
{
    if (!operation || !out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    status = admitting ? BSP_BUSY : operation != scan.state.operation ? BSP_INVALID_ARG : BSP_OK;
    if (status == BSP_OK)
    {
        size_t n = scan.count < capacity ? scan.count : capacity;
        for (size_t i = 0; i < n; ++i)
        {
            out[i] = scan.reports[scan.head];
            scan.head = (scan.head + 1) % SCAN_REPORTS;
            --scan.count;
        }
        *count = n;
    }
    bsp_request_unlock(key);
    return status;
}
/* Service owner serializes mutation; native callbacks only copy into static
 * storage. Native successful cancel is the stop boundary, not global RF silence. */
BspError ecw_bsp_ble_scan_cancel(void)
{
    uintptr_t key = bsp_request_lock();
    bool active = scan.state.active;
    bsp_request_unlock(key);
    if (!active)
    {
        return (BspError){0};
    }
    int rc = ble_gap_disc_cancel();
    BspError error = native_error(rc);
    if (rc == BLE_HS_EALREADY)
    {
        error.code = BSP_OK;
    }
    key = bsp_request_lock();
    if (scan.state.active)
    {
        if (error.code == BSP_OK)
        {
            scan.state.active = false;
        }
        scan.state.error = error;
    }
    bsp_request_unlock(key);
    return error;
}
BspStatus bsp_ble_scan_stop(BspRadio id, uint64_t operation)
{
    BspBleScanState current;
    BspStatus status = bsp_ble_scan_query(id, operation, &current);
    if (status != BSP_OK || !current.active)
    {
        return status;
    }
    status = ecw_bsp_ble_api_claim(id);
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    bool same = operation == scan.state.operation;
    bsp_request_unlock(key);
    if (!same)
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_INVALID_ARG});
    }
    return ecw_bsp_ble_api_release(ecw_bsp_ble_scan_cancel());
}

#else
uint32_t ecw_bsp_ble_scan_capability(void)
{
    return 0;
}
void ecw_bsp_ble_scan_reset_locked(void)
{
}
void ecw_bsp_ble_scan_retire(void)
{
}
BspError ecw_bsp_ble_scan_cancel(void)
{
    return (BspError){.code = BSP_UNSUPPORTED};
}
static BspStatus unavailable(BspRadio id)
{
    BspStatus status = ecw_bsp_ble_check_radio(id);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}
BspStatus bsp_ble_scan_start(BspRadio id, const BspBleScanConfig *config, uint64_t *operation)
{
    if (!operation || !bsp_ble_scan_config_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    return unavailable(id);
}
BspStatus bsp_ble_scan_query(BspRadio id, uint64_t operation, BspBleScanState *out)
{
    if (!operation || !out)
    {
        return BSP_INVALID_ARG;
    }
    return unavailable(id);
}
BspStatus bsp_ble_scan_read(BspRadio id, uint64_t operation, BspBleScanReport *out, size_t capacity,
                            size_t *count)
{
    if (!operation || !out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    return unavailable(id);
}
BspStatus bsp_ble_scan_stop(BspRadio id, uint64_t operation)
{
    if (!operation)
    {
        return BSP_INVALID_ARG;
    }
    return unavailable(id);
}
#endif
