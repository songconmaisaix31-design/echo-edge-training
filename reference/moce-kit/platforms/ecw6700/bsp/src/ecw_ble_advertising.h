#ifndef ECW_BSP_BLE_ADVERTISING_H
#define ECW_BSP_BLE_ADVERTISING_H
#include "bsp_ble.h"
/* Service serializes all operations. Instance 0 belongs exclusively to this
 * adapter; application composition excludes competing SDK advertising users. */
uint32_t ecw_bsp_ble_advertising_capability(void);
bool ecw_bsp_ble_advertising_available(void);
BspError ecw_bsp_ble_advertising_cancel(void);
#endif
