#include "ecw_wifi_scan_native.h"
#include "system_wifi.h"
#include "wpas_cntrl_api.h"
#include <string.h>
static BspStatus scan_error(sys_err_t status)
{
    return status == SYS_OK ? BSP_OK
           : status == SYS_ERR_NO_MEM ? BSP_NO_RESOURCE
           : status == SYS_ERR_BUSY ? BSP_BUSY
           : status == SYS_ERR_TIMEOUT ? BSP_TIMEOUT
           : status == SYS_ERR_INVALID_STATE ? BSP_NOT_READY : BSP_IO;
}
BspStatus ecw_wifi_scan_native(BspWifiAccessPoint *out, size_t capacity,
                               size_t *written, size_t *total)
{
    /* Runs only in the existing explicitly configured Wi-Fi service task.
     * Its output belongs to the retained BSP job, never to the waiting caller. */
    sys_err_t raw = sys_wifi_scan_start_checked(true, NULL);
    if (raw != SYS_OK)
    {
        return scan_error(raw);
    }
    unsigned int count = 0;
    raw = wpas_ctrl_scan_get_count_checked(WIFI_VIF_STA, &count);
    if (raw != SYS_OK)
    {
        return scan_error(raw);
    }
    size_t used = count < capacity ? count : capacity;
    for (size_t i = 0; i < used; ++i)
    {
        wifi_info_t native = {0};
        raw = sys_wifi_scan_get_result((unsigned int)i, &native);
        if (raw != SYS_OK)
        {
            return scan_error(raw);
        }
        if (native.ssid_len > sizeof(out[i].ssid))
        {
            return BSP_IO;
        }
        BspWifiAccessPoint ap = {.ssid_length = native.ssid_len,
                                 .rssi_dbm = native.rssi, .channel = native.channel,
                                 .security_known = true};
        memcpy(ap.ssid, native.ssid, native.ssid_len);
        memcpy(ap.bssid, native.bssid, sizeof(ap.bssid));
        switch (native.auth)
        {
        case AUTH_OPEN: ap.security = BSP_WIFI_OPEN; break;
        case AUTH_WPA2_PSK: ap.security = BSP_WIFI_WPA2_PSK; break;
        case AUTH_WPA3_SAE: ap.security = BSP_WIFI_WPA3_SAE; break;
        default: ap.security_known = false; break;
        }
        out[i] = ap;
    }
    *written = used;
    *total = count;
    return BSP_OK;
}
