#ifndef ECW_BSP_BLE_SERVICE_H
#define ECW_BSP_BLE_SERVICE_H
#include "bsp_ble.h"
/* Existing SDK Host/controller remain owned by startup, not by modules.
 * Exclusive here means BSP alone starts/stops GAP scanning and owns advertising instance 0 during its service. */
BspStatus ecw_bsp_ble_service_configure(uint32_t stack_bytes, unsigned priority, bool exclusive);
BspStatus ecw_bsp_ble_check_radio(BspRadio id);
BspStatus ecw_bsp_ble_api_claim(BspRadio id);
BspStatus ecw_bsp_ble_api_release(BspError error);
void ecw_bsp_ble_service_step(void);
#ifdef BSP_ECW_NATIVE_GATT
void ecw_bsp_ble_service_fault(BspError error);
#endif
#endif
