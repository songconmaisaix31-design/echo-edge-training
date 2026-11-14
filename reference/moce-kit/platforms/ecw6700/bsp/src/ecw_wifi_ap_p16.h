#ifndef ECW_WIFI_AP_P16_H
#define ECW_WIFI_AP_P16_H
#include "bsp_wifi.h"
/* Service worker serializes native AP/STA commands; SDK startup is exclusively owned.
 * These adapters never start STA, write NV, or patch vendor code. */
BspStatus ecw_wifi_ap_check(const BspWifiApConfig *config);
BspError ecw_wifi_ap_start_native(const BspWifiApConfig *config, uint32_t started, uint32_t budget,
                                  bool *entered);
BspError ecw_wifi_ap_stop_native(void);
bool ecw_wifi_ap_observe(bool *active);
#endif
