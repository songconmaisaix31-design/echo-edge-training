#ifndef ECW_BSP_BLE_SCAN_H
#define ECW_BSP_BLE_SCAN_H
#include "bsp_ble.h"
uint32_t ecw_bsp_ble_scan_capability(void);
void ecw_bsp_ble_scan_reset_locked(void);
void ecw_bsp_ble_scan_retire(void);
/* Exclusive BSP API/service owner, never under the BSP critical section. */
BspError ecw_bsp_ble_scan_cancel(void);
#endif
