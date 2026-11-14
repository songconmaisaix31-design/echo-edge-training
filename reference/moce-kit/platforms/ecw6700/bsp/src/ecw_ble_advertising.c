#include "ecw_ble_advertising.h"
#include "ecw_ble_service.h"
#include "ecw_ble_peripheral.h"
#include "ecw_ble_stop_trace.h"
#include "bsp_ble_validation.h"
/* P16 build force-includes generated/config.h before all source headers. */
#include "syscfg/syscfg.h"
#if CONFIG_BT_NIMBLE_ENABLED && MYNEWT_VAL(BLE_EXT_ADV) &&                                         \
    (MYNEWT_VAL(BLE_ROLE_BROADCASTER) || MYNEWT_VAL(BLE_ROLE_PERIPHERAL))
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "os/os_mbuf.h"
/* Payloads are copied to native mbufs. The API/stop worker serializes BSP
 * ownership; native advertising may end/remove automatically after connection. */
static bool configured;
static BspError native_error(int rc)
{
    BspStatus status = rc == 0                                        ? BSP_OK
                       : rc == BLE_HS_ENOMEM                          ? BSP_NO_RESOURCE
                       : rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY  ? BSP_BUSY
                       : rc == BLE_HS_EINVAL || rc == BLE_HS_EMSGSIZE ? BSP_INVALID_ARG
                       : rc == BLE_HS_ENOTSUP                         ? BSP_UNSUPPORTED
                       : rc == BLE_HS_ETIMEOUT                        ? BSP_TIMEOUT
                       : rc == BLE_HS_ENOTSYNCED                      ? BSP_NOT_READY
                                                                      : BSP_IO;
    return (BspError){
        .code = status, .native_valid = true, .native_domain = 0x4e494d42u, .native_code = rc};
}
uint32_t ecw_bsp_ble_advertising_capability(void)
{
    return BSP_BLE_ADVERTISE;
}
bool ecw_bsp_ble_advertising_available(void)
{
    return !configured && !ble_gap_get_adv_state(0);
}
BspError ecw_bsp_ble_advertising_cancel(void)
{
    if (!configured)
    {
        return (BspError){.code = BSP_OK};
    }
    int rc = ble_gap_ext_adv_stop(0);
    ECW_BLE_STOP_TRACE("adv_stop", rc);
    /* B02P03 auto-removes successful advertising AFTER its GAP callback.
     * A locally owned instance can therefore already be absent: stop returns
     * EINVAL. Do not treat that error alone as success. The locked native remove
     * below must independently prove removal (0) or absence (EALREADY).
     * EBUSY/HCI failures keep ownership and prevent GATT retirement. */
    if (rc && rc != BLE_HS_EALREADY && rc != BLE_HS_EINVAL)
    {
        return native_error(rc);
    }
    rc = ble_gap_ext_adv_remove(0);
    ECW_BLE_STOP_TRACE("adv_remove", rc);
    if (!rc || rc == BLE_HS_EALREADY)
    {
        configured = false;
        return (BspError){.code = BSP_OK};
    }
    return native_error(rc);
}
BspStatus bsp_ble_advertise_configured(BspRadio id, const BspBleAdvertisingConfig *config)
{
    if (!bsp_ble_advertising_valid(config))
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
        config->mode != BSP_BLE_ADVERTISE_NONCONNECTABLE ||
#endif
        config->interval_us % 625 || config->interval_us / 625 < 0x20 ||
        config->interval_us / 625 > 0x4000)
    {
        return BSP_UNSUPPORTED;
    }
    status = ecw_bsp_ble_api_claim(id);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!ble_hs_synced())
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_NOT_READY});
    }
    if (!ecw_bsp_ble_advertising_available())
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_BUSY});
    }
    uint8_t address_type = 0;
    int rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc)
    {
        return ecw_bsp_ble_api_release(native_error(rc));
    }
    /* Allocate both independent copied chains before native configuration.
     * Even an empty AD payload needs a valid mbuf for P16 PKTLEN access. */
    const uint8_t empty = 0;
    struct os_mbuf *ad =
        ble_hs_mbuf_from_flat(config->ad_size ? config->ad : &empty, (uint16_t)config->ad_size);
    if (!ad)
    {
        return ecw_bsp_ble_api_release((BspError){.code = BSP_NO_RESOURCE});
    }
    struct os_mbuf *response = ble_hs_mbuf_from_flat(
        config->scan_size ? config->scan_response : &empty, (uint16_t)config->scan_size);
    if (!response)
    {
        os_mbuf_free_chain(ad);
        return ecw_bsp_ble_api_release((BspError){.code = BSP_NO_RESOURCE});
    }
    struct ble_gap_ext_adv_params params = {.legacy_pdu = 1,
                                            .connectable =
                                                config->mode == BSP_BLE_ADVERTISE_CONNECTABLE,
                                            .scannable = 1,
                                            .own_addr_type = address_type,
                                            .tx_power = 0,
                                            .itvl_min = config->interval_us / 625,
                                            .itvl_max = config->interval_us / 625};
#ifdef BSP_ECW_NATIVE_GATT
    rc = ble_gap_ext_adv_configure(0, &params, NULL, ecw_bsp_ble_peripheral_gap,
                                   ecw_bsp_ble_peripheral_cookie());
#else
    rc = ble_gap_ext_adv_configure(0, &params, NULL, NULL, NULL);
#endif
    if (!rc)
    {
        configured = true;
        rc = ble_gap_ext_adv_set_data(0, ad);
        ad = NULL; /* Native entry consumes it even on failure. */
    }
    if (!rc)
    {
        rc = ble_gap_ext_adv_rsp_set_data(0, response);
        response = NULL;
    }
    if (!rc)
    {
        rc = ble_gap_ext_adv_start(0, 0, 0);
    }
    os_mbuf_free_chain(ad);
    os_mbuf_free_chain(response);
    /* Partial native configuration remains owned until explicit service stop;
     * do not hide the original error with rollback or an automatic retry. */
    return ecw_bsp_ble_api_release(native_error(rc));
}
#else
uint32_t ecw_bsp_ble_advertising_capability(void)
{
    return 0;
}
bool ecw_bsp_ble_advertising_available(void)
{
    return true;
}
BspError ecw_bsp_ble_advertising_cancel(void)
{
    return (BspError){.code = BSP_OK};
}
BspStatus bsp_ble_advertise_configured(BspRadio id, const BspBleAdvertisingConfig *config)
{
    if (!bsp_ble_advertising_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_bsp_ble_check_radio(id);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}
#endif
BspStatus bsp_ble_advertise(BspRadio id, const uint8_t *ad, size_t ad_size, const uint8_t *scan,
                            size_t scan_size, uint32_t interval)
{
    BspBleAdvertisingConfig config = {.mode = BSP_BLE_ADVERTISE_CONNECTABLE,
                                      .ad = ad,
                                      .ad_size = ad_size,
                                      .scan_response = scan,
                                      .scan_size = scan_size,
                                      .interval_us = interval};
    return bsp_ble_advertise_configured(id, &config);
}
