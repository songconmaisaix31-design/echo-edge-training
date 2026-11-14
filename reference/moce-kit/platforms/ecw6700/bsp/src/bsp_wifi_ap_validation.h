#ifndef BSP_WIFI_AP_VALIDATION_H
#define BSP_WIFI_AP_VALIDATION_H
#include "bsp_wifi.h"
static inline bool bsp_wifi_ap_config_valid(const BspWifiApConfig *config)
{
    if (!config)
    {
        return false;
    }
    const BspWifiCredentials *c = &config->credentials;
    if (!c->ssid_length || c->ssid_length > sizeof(c->ssid) || c->secret_length > sizeof(c->secret))
    {
        return false;
    }
    if (c->security == BSP_WIFI_OPEN)
    {
        return c->secret_length == 0;
    }
    if (c->security == BSP_WIFI_WPA3_SAE)
    {
        return c->secret_length > 0 && c->secret_length <= 63;
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
#endif
