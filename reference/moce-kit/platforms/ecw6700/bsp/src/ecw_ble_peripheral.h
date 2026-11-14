#ifndef ECW_BLE_PERIPHERAL_H
#define ECW_BLE_PERIPHERAL_H
#include "bsp_ble.h"
#ifdef BSP_ECW_NATIVE_GATT
#include "host/ble_gap.h"
#ifndef BSP_ECW_BLE_CONNECTIONS
#define BSP_ECW_BLE_CONNECTIONS 4u
#endif
/* Project explicitly owns the native GATT database; persistent Host stays alive.
 * One existing service worker posts native mutations onto the native Host queue.
 * No new task, hardware timer, DMA channel or SDK/controller reset. */
BspStatus ecw_bsp_ble_peripheral_prepare(const BspBleService *services, size_t count);
BspError ecw_bsp_ble_peripheral_activate(void);
BspError ecw_bsp_ble_peripheral_retire(void);
void ecw_bsp_ble_peripheral_discard_unregistered(void);
void ecw_bsp_ble_peripheral_query(BspBleState *state);
void *ecw_bsp_ble_peripheral_cookie(void);
int ecw_bsp_ble_peripheral_gap(struct ble_gap_event *event, void *cookie);
#endif
#endif
