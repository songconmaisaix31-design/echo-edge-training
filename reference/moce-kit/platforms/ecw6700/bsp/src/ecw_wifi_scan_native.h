#ifndef ECW_WIFI_SCAN_NATIVE_H
#define ECW_WIFI_SCAN_NATIVE_H
#include "bsp_wifi.h"
/* Project build option, visible through scan_capacity. No extra task or timer. */
#ifndef BSP_ECW_WIFI_SCAN_CAPACITY
#define BSP_ECW_WIFI_SCAN_CAPACITY 16u
#endif
_Static_assert(BSP_ECW_WIFI_SCAN_CAPACITY > 0 && BSP_ECW_WIFI_SCAN_CAPACITY <= 256,
               "Wi-Fi scan storage must be explicitly bounded");
BspStatus ecw_wifi_scan_native(BspWifiAccessPoint *out, size_t capacity,
                               size_t *written, size_t *total);
#endif
