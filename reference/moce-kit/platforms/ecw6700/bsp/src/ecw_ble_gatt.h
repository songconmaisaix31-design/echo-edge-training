#ifndef ECW_BSP_BLE_GATT_H
#define ECW_BSP_BLE_GATT_H
#include "bsp_ble.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#define ECW_BSP_BLE_SERVICES 4u
#define ECW_BSP_BLE_CHARACTERISTICS 8u
/* Private nonblocking event sink, called under the BSP state lock. It copies
 * data and accepts/rejects atomically; it must never call module/user code. */
typedef bool (*EcwBspBleWriteSink)(void *context, uint16_t connection, uint16_t id,
                                   const uint8_t *data, uint16_t n);
/* Called under the same lock for writes rejected before the event sink.
 * The write sink accounts for its own rejection; never count both paths. */
typedef void (*EcwBspBleWriteRejected)(void *context);
struct EcwBspBleGatt;
typedef struct
{
    struct EcwBspBleGatt *owner;
    uint16_t id, max_length, length, handle;
    uint32_t properties;
    ble_uuid128_t uuid;
    uint8_t value[BSP_BLE_EVENT_DATA_MAX];
} EcwBspBleAttribute;
typedef struct EcwBspBleGatt
{
    bool prepared, registration_attempted, registered, native_detached;
    bool activation_attempted, active;
    size_t count;
    EcwBspBleWriteSink write;
    EcwBspBleWriteRejected rejected;
    void *context;
    ble_uuid128_t uuid[ECW_BSP_BLE_SERVICES];
    struct ble_gatt_svc_def services[ECW_BSP_BLE_SERVICES + 1u];
    struct ble_gatt_chr_def characteristics[ECW_BSP_BLE_SERVICES][ECW_BSP_BLE_CHARACTERISTICS + 1u];
    EcwBspBleAttribute attributes[ECW_BSP_BLE_CHARACTERISTICS];
} EcwBspBleGatt;

/* Stable, zero-initialized storage; lifecycle operations require exclusive
 * platform ownership. No SDK access or retained caller definitions on prepare. */
BspStatus ecw_bsp_ble_gatt_prepare(EcwBspBleGatt *g, const BspBleService *services, size_t count,
                                   EcwBspBleWriteSink write, EcwBspBleWriteRejected rejected,
                                   void *context);
/* Native NimBLE result. Even a failed attempt can mutate host configuration;
 * do not overwrite/retry definitions until host reset and quiescence. */
int ecw_bsp_ble_gatt_register(EcwBspBleGatt *g);
/* Alternative to register-before-Host-start: activate a freshly prepared service
 * in an already synced persistent Host. Sole Host lifecycle owner only; it must
 * own the entire native GATT database and keep GAP admission closed. No BSP lock
 * across native calls. A previous generation must already have been detached
 * AND independently retired before prepare. Checks native mutability, resets the
 * empty ATT database, registers definitions, then calls ble_gatts_start once.
 * Failure after mutation retains the copied definitions for explicit detach;
 * no automatic retry. active is true only after successful ATT construction. */
int ecw_bsp_ble_gatt_activate_on_host(EcwBspBleGatt *g);
/* Exclusive native Host consumer/lifecycle owner only; never call concurrently
 * with a GAP start, GATT callback or another native service owner. Checks native
 * GAP/connection mutability before removing ATT/services via public P16 APIs.
 * Returns a native status; BUSY/error preserves definitions for explicit retry.
 * Success removes native GATT references, NOT controller/HCI/queued callbacks.
 * The copied store remains held; forget_quiescent still requires a separate
 * proven producer/callback retirement boundary. No SDK deinit or task deletion. */
int ecw_bsp_ble_gatt_detach_on_host(EcwBspBleGatt *g);
BspStatus ecw_bsp_ble_gatt_set(EcwBspBleGatt *g, uint16_t id, const void *data, size_t n);
/* Only after the platform has stopped callbacks AND removed/reset SDK service
 * references. Host stop alone is insufficient; this does not stop/reset SDK. */
void ecw_bsp_ble_gatt_forget_quiescent(EcwBspBleGatt *g);
#endif
