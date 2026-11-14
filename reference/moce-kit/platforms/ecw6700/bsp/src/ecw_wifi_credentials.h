#ifndef ECW_WIFI_CREDENTIALS_H
#define ECW_WIFI_CREDENTIALS_H
#include "bsp_wifi.h"
#include "wpas_struct_iface.h"
/* Private fixed-P16 conversion. No SDK calls, retained pointers or public
 * capability assertion. Output must not overlap input. */
BspStatus ecw_wifi_credentials_check(const BspWifiCredentials *credentials);
BspStatus ecw_wifi_credentials_copy(wpa_network_config_t *out,
                                    const BspWifiCredentials *credentials);
void ecw_wifi_credentials_clear(wpa_network_config_t *config);
#endif
