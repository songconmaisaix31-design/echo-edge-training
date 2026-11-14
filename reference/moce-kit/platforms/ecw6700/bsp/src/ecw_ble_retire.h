#ifndef ECW_BLE_RETIRE_H
#define ECW_BLE_RETIRE_H
#include "ecw_ble_gatt.h"

/* Candidate persistent-host logical stop, not the public finite-budget API.
 * Sole Host lifecycle consumer only, after new advertising/send admission closes.
 * Every GAP connect callback, including one racing advertising stop, must update
 * the owner's live handle set. No concurrent native GAP/GATT owner is permitted.
 * HCI calls may wait; run in retained Host work, never under BSP IRQ lock.
 * Native success = GATT references detached, NOT all transport/events retired.
 * The owner must keep g and old-generation context until its separate fence.
 * EBUSY = wait for actual native state change; don't spin/retry autonomously.
 * This slice permits up to 4 tracked links, without advertising that capability. */
#define ECW_BLE_RETIRE_LINKS 4u
/* Internal admission result: waiting is true only after owned termination commands
 * were accepted and native connections still exist. Resume on real GAP events;
 * no repeated terminate commands or autonomous EBUSY retries. */
int ecw_ble_retire_begin_on_host(EcwBspBleGatt *g, const uint16_t *handles, size_t count,
                               bool *waiting);
int ecw_ble_retire_on_host(EcwBspBleGatt *g, const uint16_t *handles, size_t count);
#endif
