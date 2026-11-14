#ifndef ECW_BLE_FENCE_H
#define ECW_BLE_FENCE_H
#include "bsp_core.h"
#include "nimble/nimble_npl.h"

/* Private ordered marker for a PERSISTENT native Host consumer. This does not
 * stop the consumer, controller, callouts or future HCI producers. Before using
 * it as part of service retirement, close admission and detach native GATT on
 * the Host owner, and establish which producers can still enqueue old work.
 * Static/retained storage; one lifecycle owner. Never retain caller buffers. */
typedef struct
{
    struct ble_npl_eventq *queue;
    struct ble_npl_event marker;
    bool prepared, held, posting, reached;
    uint64_t operation, last_operation;
} EcwBleFence;

/* Zero-initialized storage, owner only. No live SDK queue creation/destruction. */
BspStatus ecw_ble_fence_prepare(EcwBleFence *fence);
/* Retained worker ONLY, not Host consumer/ISR/finite-budget public caller.
 * Native enqueue may block indefinitely. operation must increase across reuse.
 * Public cleanup timeout must leave this worker and fence alive. */
BspStatus ecw_ble_fence_post(EcwBleFence *fence, uint64_t operation);
/* No wait. True only after marker callback AND enqueue call both finish.
 * It fences earlier default-queue callbacks, not subsequent producers. */
BspStatus ecw_ble_fence_query(EcwBleFence *fence, uint64_t operation, bool *reached);
/* Explicit owner acknowledgement. Cannot reuse while producer still in SDK. */
BspStatus ecw_ble_fence_release(EcwBleFence *fence, uint64_t operation);
#endif
