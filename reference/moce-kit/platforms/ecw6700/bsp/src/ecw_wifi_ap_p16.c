#include "ecw_wifi_ap_p16.h"
#include "ecw_wifi_credentials.h"
#include "bsp_wifi_ap_validation.h"
#include "system_wifi.h"
#include "wpas_cntrl_api.h"
#include "wpa_debug.h"
#include <string.h>

static BspError native_error(sys_err_t raw)
{
    BspStatus status = BSP_IO;
    switch (raw)
    {
    case SYS_OK:
        status = BSP_OK;
        break;
    case SYS_ERR_NO_MEM:
        status = BSP_NO_RESOURCE;
        break;
    case SYS_ERR_INVALID_ARG:
        status = BSP_INVALID_ARG;
        break;
    case SYS_ERR_INVALID_STATE:
        status = BSP_NOT_READY;
        break;
    case SYS_ERR_NOT_SUPPORTED:
        status = BSP_UNSUPPORTED;
        break;
    case SYS_ERR_TIMEOUT:
    case SYS_ERR_WIFI_TIMEOUT:
        status = BSP_TIMEOUT;
        break;
    case SYS_ERR_BUSY:
        status = BSP_BUSY;
        break;
    default:
        break;
    }
    return (BspError){
        .code = status, .native_valid = true, .native_domain = 0x45435743u, .native_code = raw};
}
BspStatus ecw_wifi_ap_check(const BspWifiApConfig *config)
{
    if (!bsp_wifi_ap_config_valid(config))
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_wifi_credentials_check(&config->credentials);
    if (status != BSP_OK)
    {
        return status;
    }
    /* The native high-level AP converter reads C strings. Do not truncate SSID. */
    if (memchr(config->credentials.ssid, 0, config->credentials.ssid_length) ||
        config->channel > UINT8_MAX || config->max_clients > sys_wifi_ap_get_sta_max_capability())
    {
        return BSP_UNSUPPORTED;
    }
    return BSP_OK;
}
BspError ecw_wifi_ap_start_native(const BspWifiApConfig *config, uint32_t started, uint32_t budget,
                                  bool *entered)
{
    *entered = false;
    if ((uint32_t)(bsp_time_ms() - started) >= budget)
    {
        return (BspError){.code = BSP_TIMEOUT};
    }
    /* AP-only may not stop an existing/native retrying STA as a side effect. */
    if (!config->concurrent_sta && sys_wifi_sta_get_status() != STA_STATUS_STOP)
    {
        return (BspError){.code = BSP_BUSY};
    }
    BspError error = native_error(wpas_ctrl_system_set_log_level(MSG_WARNING));
    if (error.code != BSP_OK)
    {
        return error;
    }
    if ((uint32_t)(bsp_time_ms() - started) >= budget)
    {
        return (BspError){.code = BSP_TIMEOUT};
    }
    wifi_config_u native = {0};
    memcpy(native.ap.ssid, config->credentials.ssid, config->credentials.ssid_length);
    memcpy(native.ap.password, config->credentials.secret, config->credentials.secret_length);
    native.ap.authmode = config->credentials.security == BSP_WIFI_OPEN       ? AUTH_OPEN
                         : config->credentials.security == BSP_WIFI_WPA2_PSK ? AUTH_WPA2_PSK
                                                                             : AUTH_WPA3_SAE;
    native.ap.channel = (unsigned char)config->channel;
    native.ap.max_connect = (unsigned char)config->max_clients;
    native.ap.hidden_ssid = config->hidden;
    /* P16 AP_STA permits both roles but starts neither. Its AP/STA setters would
     * implicitly stop the excluded role and discard that return value. Use the
     * non-stopping mode even for AP-only, whose STA admission the BSP forbids.
     * init has disabled runtime auto-save under the explicit exclusive owner. */
    sys_wifi_set_mode(WIFI_MODE_AP_STA);
    *entered = true;
    error = native_error(sys_wifi_ap_start(&native));
    volatile unsigned char *bytes = (volatile unsigned char *)&native;
    for (size_t i = 0; i < sizeof(native); ++i)
    {
        bytes[i] = 0;
    }
    return error;
}
BspError ecw_wifi_ap_stop_native(void)
{
    /* sys_wifi_ap_stop discards this native return value; do not use that wrapper.
     * The ACK is still not proof that all radio events or clients have retired. */
    return native_error(wpas_ctrl_network_stop(WIFI_VIF_AP));
}
bool ecw_wifi_ap_observe(bool *active)
{
    wifi_status_e status = sys_wifi_ap_get_status();
    *active = status == AP_STATUS_STARTED;
    return status == AP_STATUS_STARTED || status == AP_STATUS_STOP;
}
