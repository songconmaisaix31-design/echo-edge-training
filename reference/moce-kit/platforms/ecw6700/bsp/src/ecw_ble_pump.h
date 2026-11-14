#ifndef ECW_BLE_PUMP_H
#define ECW_BLE_PUMP_H
#include "bsp_core.h"
#include "nimble/nimble_npl.h"

/* Private, stable storage owned by the native BLE lifecycle. Not a public
 * capability or proof that controller/transport callbacks have stopped. */
typedef struct
{
    struct ble_npl_eventq *queue;
    struct ble_npl_event exit_event;
    bool prepared, running, requested, posting, returned;
    BspStatus result;
} EcwBlePump;

/* Initially zero-initialized stable storage. Lifecycle owner only, before launching the sole Host task. Previous run and
 * posting must be quiet and SDK references retired before re-preparation. */
BspStatus ecw_ble_pump_prepare(EcwBlePump *pump);
/* Sole Host task; processes the native default event queue. Must not coexist
 * with nimble_port_run or the vendor-created Host task. */
BspStatus ecw_ble_pump_run(EcwBlePump *pump);
/* Retained lifecycle worker ONLY: SDK enqueue may wait forever. Caller must
 * first prove native procedures/connections stopped and prevent new admission.
 * The marker fences earlier queue work; it does not stop later producers. */
BspStatus ecw_ble_pump_request_exit(EcwBlePump *pump);
/* Both event consumer and marker producer have stopped referencing queue/event.
 * Does not prove native Host/controller shutdown or permission to free GATT. */
bool ecw_ble_pump_quiet(EcwBlePump *pump, BspStatus *result);
#endif
