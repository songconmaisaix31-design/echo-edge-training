#include "bsp_wifi_ap_validation.h"
#include "bsp_ble_validation.h"
/* ECW hardware radio inventory is retained. Complete public success backends
 * are still being integrated; zero capabilities mean missing software adaptation,
 * not absent hardware. Replace each family when its lifecycle is complete.
 * No transport is inferred from UART/SPI and no hardware is touched. */
#include "bsp_wifi.h"
#include "bsp_ble.h"
#include "bsp_net.h"
#if !defined(BSP_ECW_NATIVE_WIFI) || !defined(BSP_ECW_NATIVE_NET) ||                               \
    !defined(BSP_ECW_NATIVE_BLE) || !defined(BSP_ECW_NATIVE_GATT)
static BspStatus radio(BspRadio id)
{
    BspStatus s = bsp_delay_ms(0);
    if (s != BSP_OK)
    {
        return s;
    }
    const BspRadioInventory *list = bsp_board_get_radio_inventory();
    if (!list)
    {
        return BSP_NOT_READY;
    }
    for (size_t i = 0; i < list->count; ++i)
    {
        if (list->radios[i].id == id)
        {
            return BSP_UNSUPPORTED;
        }
    }
    return BSP_INVALID_ARG;
}
#endif
#ifndef BSP_ECW_NATIVE_NET
static BspStatus socket_missing(BspSocket socket)
{
    (void)socket;
    BspStatus s = bsp_delay_ms(0);
    return s == BSP_OK ? BSP_INVALID_ARG : s;
}
static bool family_valid(BspNetFamily f)
{
    return f == BSP_NET_IPV4 || f == BSP_NET_IPV6;
}
#endif
#ifndef BSP_ECW_NATIVE_WIFI
static bool credentials_valid(const BspWifiCredentials *c)
{
    if (!c || !c->ssid_length || c->ssid_length > 32 || c->secret_length > 64)
    {
        return false;
    }
    if (c->security == BSP_WIFI_OPEN)
    {
        return c->secret_length == 0;
    }
    if (c->security == BSP_WIFI_WPA3_SAE)
    {
        return c->secret_length >= 1 && c->secret_length <= 63;
    }
    if (c->security != BSP_WIFI_WPA2_PSK || c->secret_length < 8)
    {
        return false;
    }
    if (c->secret_length == 64)
    {
        for (unsigned i = 0; i < 64; ++i)
        {
            unsigned char x = c->secret[i];
            if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F')))
            {
                return false;
            }
        }
    }
    return true;
}

BspStatus bsp_wifi_ap_start(BspRadio id, const BspWifiApConfig *config, uint32_t timeout)
{
    if (!bsp_wifi_ap_config_valid(config) || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}
BspStatus bsp_wifi_ap_query(BspRadio id, BspWifiApState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}
BspStatus bsp_wifi_ap_stop(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    return status == BSP_OK ? BSP_UNSUPPORTED : status;
}

BspStatus bsp_wifi_capabilities(BspRadio id, BspWifiCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = radio(id);
    if (s == BSP_UNSUPPORTED)
    {
        *out = (BspWifiCapabilities){0};
        return BSP_OK;
    }
    return s;
}
BspStatus bsp_wifi_init(BspRadio id)
{
    return radio(id);
}
BspStatus bsp_wifi_connect(BspRadio id, const BspWifiCredentials *c, uint32_t timeout,
                           uint64_t *operation)
{
    if (!credentials_valid(c) || !operation || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_reconnect_capabilities(BspRadio id, BspWifiReconnectCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = radio(id);
    if (status == BSP_UNSUPPORTED)
    {
        *out = (BspWifiReconnectCapabilities){0};
        return BSP_OK;
    }
    return status;
}
BspStatus bsp_wifi_connect_configured(BspRadio id, const BspWifiConnectConfig *config,
                                      uint32_t timeout, uint64_t *operation)
{
    if (!config || !operation || timeout > INT32_MAX ||
        (config->reconnect_policy != BSP_WIFI_RECONNECT_DISABLED &&
         config->reconnect_policy != BSP_WIFI_RECONNECT_NATIVE) ||
        !credentials_valid(&config->credentials))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_connect_result(BspRadio id, uint64_t operation, BspWifiConnectResult *out)
{
    if (!operation || !out)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_query(BspRadio id, BspWifiState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_events(BspRadio id, BspWifiEvent *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_scan(BspRadio id, BspWifiAccessPoint *out, size_t capacity, size_t *written,
                        size_t *total, uint32_t timeout)
{
    if (!out || !capacity || !written || !total || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_disconnect(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_wifi_stop(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
#endif

#ifndef BSP_ECW_NATIVE_BLE
BspStatus bsp_ble_scan_start(BspRadio id, const BspBleScanConfig *config, uint64_t *operation)
{
    if (!operation || !bsp_ble_scan_config_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_scan_query(BspRadio id, uint64_t operation, BspBleScanState *out)
{
    if (!operation || !out)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_scan_read(BspRadio id, uint64_t operation, BspBleScanReport *out, size_t capacity,
                            size_t *count)
{
    if (!operation || !out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_scan_stop(BspRadio id, uint64_t operation)
{
    if (!operation)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_capabilities(BspRadio id, BspBleCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = radio(id);
    if (s == BSP_UNSUPPORTED)
    {
        *out = (BspBleCapabilities){0};
        return BSP_OK;
    }
    return s;
}
BspStatus bsp_ble_init(BspRadio id, const BspBleService *services, size_t count)
{
    if (!bsp_ble_services_valid(services, count))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_query(BspRadio id, BspBleState *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
#endif
#ifndef BSP_ECW_NATIVE_BLE
BspStatus bsp_ble_advertise_configured(BspRadio id, const BspBleAdvertisingConfig *config)
{
    if (!bsp_ble_advertising_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_advertise(BspRadio id, const uint8_t *ad, size_t ad_size, const uint8_t *scan,
                            size_t scan_size, uint32_t interval)
{
    if (!interval || !bsp_ble_ad_valid(ad, ad_size) || !bsp_ble_ad_valid(scan, scan_size))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
#endif
#ifndef BSP_ECW_NATIVE_GATT
BspStatus bsp_ble_set_value(BspRadio id, uint16_t attribute, const void *data, size_t n)
{
    (void)attribute;
    if (!data && n)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_send(BspRadio id, BspBleConnection connection, uint16_t attribute,
                       const void *data, size_t n, bool indicate, uint64_t *operation)
{
    (void)connection;
    (void)attribute;
    (void)indicate;
    if ((!data && n) || !operation)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_ble_events(BspRadio id, BspBleEvent *out, size_t capacity, size_t *count)
{
    if (!out || !capacity || !count)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
#endif /* BSP_ECW_NATIVE_GATT */
#ifndef BSP_ECW_NATIVE_BLE
BspStatus bsp_ble_stop(BspRadio id, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
#endif
#ifndef BSP_ECW_NATIVE_NET
BspStatus bsp_net_capabilities(BspRadio id, BspNetCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus s = radio(id);
    if (s == BSP_UNSUPPORTED)
    {
        *out = (BspNetCapabilities){0};
        return BSP_OK;
    }
    return s;
}
BspStatus bsp_net_open(BspRadio id, BspNetTransport transport, BspNetFamily family, BspSocket *out)
{
    if (!out || !family_valid(family) || (transport != BSP_NET_TCP && transport != BSP_NET_UDP))
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_net_connect(BspSocket socket, const BspNetEndpoint *peer, uint32_t timeout)
{
    if (!peer || !family_valid(peer->address.family) || !peer->port || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_send(BspSocket socket, const void *data, size_t n, uint32_t timeout,
                       BspNetIo *out)
{
    if (!data || !n || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_receive(BspSocket socket, void *data, size_t n, uint32_t timeout, BspNetIo *out)
{
    if (!data || !n || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_send_to(BspSocket socket, const BspNetEndpoint *peer, const void *data, size_t n,
                          uint32_t timeout, BspNetIo *out)
{
    if (!peer || !family_valid(peer->address.family) || !peer->port || (!data && n) || !out ||
        timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_receive_from(BspSocket socket, BspNetEndpoint *peer, void *data, size_t n,
                               uint32_t timeout, BspNetIo *out)
{
    if (!peer || (!data && n) || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_resolve(BspRadio id, const char *name, BspNetFamily family, BspNetAddress *out,
                          uint32_t timeout)
{
    if (!name || !*name || !out || !family_valid(family) || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return radio(id);
}
BspStatus bsp_net_close(BspSocket socket, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}
BspStatus bsp_net_error(BspSocket socket, BspError *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    return socket_missing(socket);
}

#endif
