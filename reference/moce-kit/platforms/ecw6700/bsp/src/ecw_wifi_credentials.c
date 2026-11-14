#include "ecw_wifi_credentials.h"
#include <string.h>
BspStatus ecw_wifi_credentials_check(const BspWifiCredentials *c)
{
    if (!c || !c->ssid_length || c->ssid_length > sizeof(c->ssid) ||
        c->secret_length > sizeof(c->secret) || c->security < BSP_WIFI_OPEN ||
        c->security > BSP_WIFI_WPA3_SAE)
    {
        return BSP_INVALID_ARG;
    }
    if (c->security == BSP_WIFI_OPEN && c->secret_length)
    {
        return BSP_INVALID_ARG;
    }
    if (c->security == BSP_WIFI_WPA2_PSK && c->secret_length < 8)
    {
        return BSP_INVALID_ARG;
    }
    if (c->security == BSP_WIFI_WPA3_SAE && (!c->secret_length || c->secret_length > 63))
    {
        return BSP_INVALID_ARG;
    }
    if (c->secret_length == 64)
    {
        for (size_t i = 0; i < 64; ++i)
        {
            uint8_t x = c->secret[i];
            if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F')))
            {
                return BSP_INVALID_ARG;
            }
        }
    }
    /* P16 SSID uses explicit length. Passphrases use os_strlen in
     * wpas_core_network_config, so embedded NUL must not silently truncate. */
    if (memchr(c->secret, 0, c->secret_length))
    {
        return BSP_UNSUPPORTED;
    }
#ifndef CONFIG_SAE
    if (c->security == BSP_WIFI_WPA3_SAE)
    {
        return BSP_UNSUPPORTED;
    }
#endif
    return BSP_OK;
}
BspStatus ecw_wifi_credentials_copy(wpa_network_config_t *out, const BspWifiCredentials *c)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ecw_wifi_credentials_check(c);
    if (status != BSP_OK)
    {
        return status;
    }
    /* No variable-length pointers (Enterprise identity/certificates) retained. */
    memset(out, 0, sizeof(*out));
    memcpy(out->ssid, c->ssid, c->ssid_length);
    memcpy(out->password, c->secret, c->secret_length);
    out->ssid_len = c->ssid_length;
    out->password_len = c->secret_length;
    out->auth_mode = c->security == BSP_WIFI_OPEN       ? WPA_CTRL_AUTH_OPEN
                     : c->security == BSP_WIFI_WPA2_PSK ? WPA_CTRL_AUTH_WPA2_PSK
                                                        : WPA_CTRL_AUTH_WPA3_SAE;
    out->pairwise = c->security == BSP_WIFI_OPEN ? WPA_CTRL_CIPHER_NONE : WPA_CTRL_CIPHER_CCMP;
    out->group = out->pairwise;
    out->scan_ssid = 1; /* Allow explicit target SSID to be hidden. */
    return BSP_OK;
}
void ecw_wifi_credentials_clear(wpa_network_config_t *config)
{
    if (!config)
    {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *)config;
    for (size_t i = 0; i < sizeof(*config); ++i)
    {
        p[i] = 0;
    }
}
