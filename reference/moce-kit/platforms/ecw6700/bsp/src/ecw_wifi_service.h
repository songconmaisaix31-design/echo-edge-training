#ifndef ECW_WIFI_SERVICE_H
#define ECW_WIFI_SERVICE_H
#include "bsp_wifi.h"
/* Platform configuration, not a Board capability or a module-facing SDK hook. */
BspStatus ecw_wifi_service_configure(uint32_t worker_bytes, uint32_t control_bytes,
                                     unsigned priority, bool exclusive_control);
void ecw_wifi_service_step(void);
void ecw_wifi_service_wake(void);
#endif
