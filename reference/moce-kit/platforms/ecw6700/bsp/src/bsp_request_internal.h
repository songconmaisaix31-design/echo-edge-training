#ifndef BSP_REQUEST_INTERNAL_H
#define BSP_REQUEST_INTERNAL_H
#include "bsp_request.h"

/* Platform-only hooks. Must serialize task/ISR state, preserve nesting and
 * restore prior state. No hardware implementation or no-op default supplied. */
typedef struct
{
    uintptr_t (*enter)(void *context);
    void (*leave)(void *context, uintptr_t previous);
    void *context;
} BspRequestSync;
BspStatus bsp_request_runtime_init(const BspRequestSync *sync);
uintptr_t bsp_request_lock(void);
void bsp_request_unlock(uintptr_t previous);
BspStatus bsp_request_begin(BspRequest *r, void *owner, void (*retire)(void *),
                            uint64_t *generation);
void bsp_request_end_submit(BspRequest *r);
BspStatus bsp_request_reject(BspRequest *r, uint64_t generation);
BspStatus bsp_request_publish(BspRequest *r, uint64_t generation, const BspResult *result);
BspStatus bsp_request_dispatch_one(BspRequest *r, bool *delivered);
#endif
