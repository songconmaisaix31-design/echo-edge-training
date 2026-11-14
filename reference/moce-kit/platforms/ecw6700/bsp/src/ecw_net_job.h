/* Private, one job per socket, platform-lifetime storage. Caller pins the radio
 * netif and reserves socket ownership until job_release; no concurrent SDK use.
 * Caller timeout is not SDK completion. Poll never stores caller RX pointers. */
#ifndef ECW_BSP_NET_JOB_H
#define ECW_BSP_NET_JOB_H
#include "ecw_net_socket.h"
#define ECW_BSP_NET_JOB_BYTES 2048u
typedef enum
{
    ECW_NET_CONNECT,
    ECW_NET_SEND,
    ECW_NET_RECEIVE,
    ECW_NET_SEND_TO,
    ECW_NET_RECEIVE_FROM,
    ECW_NET_CLOSE
} EcwBspNetCommand;
typedef struct
{
    bool active, done, timed_out;
    EcwBspNetCommand command;
    EcwBspNetSocket *socket;
    uint32_t started_ms, timeout_ms;
    size_t size;
    BspNetEndpoint peer;
    BspNetIo io;
    BspStatus result;
    BspError error;
    uint8_t buffer[ECW_BSP_NET_JOB_BYTES];
} EcwBspNetJob;
/* TX copied at admission. TCP uses <=2048 bytes per operation (short progress);
 * UDP retains one datagram, never splits it. Zero IO budget is UNSUPPORTED here:
 * internal SDK control wait cannot be proven immediately completable. CLOSE
 * can be queued with zero budget, but ownership remains until worker returns. */
BspStatus ecw_bsp_net_job_start(EcwBspNetJob *job, EcwBspNetSocket *socket,
                                EcwBspNetCommand command, const BspNetEndpoint *peer,
                                const void *tx, size_t size, uint32_t timeout, uint32_t stack_bytes,
                                unsigned priority);
/* BUSY means still executing within budget. TIMEOUT is frozen once observed,
 * and leaves RX/output unchanged even if the SDK later succeeds. Completed OK
 * copies RX only during poll; no application pointer remains in the job. */
BspStatus ecw_bsp_net_job_poll(EcwBspNetJob *job, void *rx, size_t capacity, BspNetEndpoint *peer,
                               BspNetIo *out);
bool ecw_bsp_net_job_quiescent(EcwBspNetJob *job);
/* BUSY while SDK may access storage; wipes owned data only after completion. */
BspStatus ecw_bsp_net_job_release(EcwBspNetJob *job);
#endif
