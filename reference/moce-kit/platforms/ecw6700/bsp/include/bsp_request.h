/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_REQUEST_H
#define BSP_REQUEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Source API 0.5.1; not a released cross-platform ABI. */
#define BSP_REQUEST_API_REVISION 0x00000501u
    typedef enum
    {
        BSP_OK,
        BSP_INVALID_ARG,
        BSP_NOT_READY,
        BSP_INVALID_STATE,
        BSP_UNSUPPORTED,
        BSP_BUSY,
        BSP_NO_RESOURCE,
        BSP_IO,
        BSP_TIMEOUT
    } BspStatus;
    typedef enum
    {
        BSP_UNKNOWN,
        BSP_NO,
        BSP_YES
    } BspEvidence;
    typedef struct
    {
        BspStatus code;
        bool native_valid;
        uint32_t native_domain;
        int64_t native_code;
    } BspError;
    typedef struct
    {
        BspError error, cleanup_error;
        size_t tx_done, rx_done;
        bool tx_count_valid, rx_count_valid;
        BspEvidence hardware_started, wire_complete;
        bool buffers_returned;
    } BspResult;
    typedef enum
    {
        BSP_REQ_READY,
        BSP_REQ_RUNNING,
        BSP_REQ_FAULT_HELD,
        BSP_REQ_TERMINAL
    } BspRequestState;
    typedef enum
    {
        BSP_NOTIFY_NONE,
        BSP_NOTIFY_NATIVE,
        BSP_NOTIFY_DISPATCH
    } BspNotifyMode;
    typedef struct BspRequest BspRequest;
    typedef void (*BspDoneFn)(BspRequest *request, void *user);
    typedef struct
    {
        BspNotifyMode mode;
        BspDoneFn done;
        void *user;
    } BspNotify;
    typedef struct
    {
        BspRequestState state;
        BspResult result;
        uint64_t generation;
    } BspRequestSnapshot;

    /* Storage is public solely for static allocation. Never inspect/edit fields.
 * Initialize storage to {0}; never copy a live object or reinitialize it. */
    struct BspRequest
    {
        bool initialized;
        BspRequestSnapshot snapshot;
        BspNotify notify;
        bool submit_active, callback_pending, callback_running;
        void *owner;
        void (*retire)(void *owner);
    };

    BspStatus bsp_request_init(BspRequest *r, const BspNotify *notify);
    BspStatus bsp_request_query(const BspRequest *r, BspRequestSnapshot *out);
    BspStatus bsp_request_reset(BspRequest *r);
    /* Generic cancellation is not supported in this adapter; use documented per-port recovery/stream
 * stop. */
    BspStatus bsp_request_cancel(BspRequest *r);

#ifdef __cplusplus
}
#endif

#endif
