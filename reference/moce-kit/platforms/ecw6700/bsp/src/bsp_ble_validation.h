/* Shared private syntax validation. No Board, SDK, allocation or capacity policy. */
#ifndef BSP_BLE_VALIDATION_H
#define BSP_BLE_VALIDATION_H
#include "bsp_ble.h"
/* Counted legacy AD records, with no implicit padding or vendor parsing. */
static inline bool bsp_ble_ad_valid(const uint8_t *data, size_t size)
{
    if ((!data && size) || size > 31u)
    {
        return false;
    }
    for (size_t i = 0; i < size;)
    {
        size_t length = data[i++];
        if (!length || length > size - i)
        {
            return false;
        }
        i += length;
    }
    return true;
}
static inline bool bsp_ble_advertising_valid(const BspBleAdvertisingConfig *c)
{
    return c &&
           (c->mode == BSP_BLE_ADVERTISE_NONCONNECTABLE ||
            c->mode == BSP_BLE_ADVERTISE_CONNECTABLE) &&
           c->interval_us && bsp_ble_ad_valid(c->ad, c->ad_size) &&
           bsp_ble_ad_valid(c->scan_response, c->scan_size);
}
static inline bool bsp_ble_services_valid(const BspBleService *services, size_t count)
{
    if (!services && !count)
    {
        return true;
    }
    if (!services || !count)
    {
        return false;
    }
    for (size_t s = 0; s < count; ++s)
    {
        if (!services[s].items || !services[s].count)
        {
            return false;
        }
        for (size_t c = 0; c < services[s].count; ++c)
        {
            const BspBleCharacteristic *a = &services[s].items[c];
            if (!a->max_length || !a->properties ||
                (a->properties &
                 ~(uint32_t)(BSP_BLE_READ | BSP_BLE_WRITE | BSP_BLE_NOTIFY | BSP_BLE_INDICATE)))
            {
                return false;
            }
            /* No caller buffers retained or finite-capacity ID table introduced. */
            for (size_t old_s = 0; old_s <= s; ++old_s)
            {
                size_t old_count = old_s == s ? c : services[old_s].count;
                for (size_t old_c = 0; old_c < old_count; ++old_c)
                {
                    if (services[old_s].items[old_c].id == a->id)
                    {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}
static inline bool bsp_ble_scan_config_valid(const BspBleScanConfig *c)
{
    return c && c->interval_us && c->window_us && c->window_us <= c->interval_us &&
           c->duration_ms <= INT32_MAX;
}
#endif
