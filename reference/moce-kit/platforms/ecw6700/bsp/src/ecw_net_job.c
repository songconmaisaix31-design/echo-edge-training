#include "ecw_net_job.h"
#include "FreeRTOS.h"
#include "bsp_request_internal.h"
#include "task.h"
#include <string.h>
static bool receives(EcwBspNetCommand c)
{
    return c == ECW_NET_RECEIVE || c == ECW_NET_RECEIVE_FROM;
}
static bool transfers(EcwBspNetCommand c) { return c != ECW_NET_CONNECT && c != ECW_NET_CLOSE; }
static void wipe(EcwBspNetJob *j)
{
    volatile uint8_t *p = j->buffer;
    for (size_t i = 0; i < sizeof(j->buffer); ++i)
    {
        p[i] = 0;
    }
}
static void run(void *arg)
{
    EcwBspNetJob *j = arg;
    uintptr_t key = bsp_request_lock();
    uint32_t elapsed = bsp_time_ms() - j->started_ms;
    bool enter = j->command == ECW_NET_CLOSE || (!j->timed_out && elapsed < j->timeout_ms);
    uint32_t remaining = elapsed < j->timeout_ms ? j->timeout_ms - elapsed : 0;
    bsp_request_unlock(key);
    BspStatus status = BSP_TIMEOUT;
    if (enter)
    {
        switch (j->command)
        {
        case ECW_NET_CONNECT:
            status = ecw_bsp_net_socket_connect(j->socket, &j->peer, remaining);
            break;
        case ECW_NET_SEND:
            status = ecw_bsp_net_socket_send(j->socket, j->buffer, j->size, remaining, &j->io);
            break;
        case ECW_NET_RECEIVE:
            status = ecw_bsp_net_socket_receive(j->socket, j->buffer, j->size, remaining, &j->io);
            break;
        case ECW_NET_SEND_TO:
            status = ecw_bsp_net_socket_send_to(j->socket, &j->peer, j->buffer, j->size, remaining,
                                                &j->io);
            break;
        case ECW_NET_RECEIVE_FROM:
            status = ecw_bsp_net_socket_receive_from(j->socket, &j->peer, j->buffer, j->size,
                                                     remaining, &j->io);
            break;
        case ECW_NET_CLOSE:
            status = ecw_bsp_net_socket_close(j->socket);
            break;
        }
    }
    if (status == BSP_OK && transfers(j->command) && j->io.bytes > j->size)
    {
        status = BSP_IO;
        j->socket->fault = true;
    }
    key = bsp_request_lock();
    /* The native call has returned. A budget-expired receive with no data,
     * EOF or datagram consumed is safe to use again. Native errno timeouts,
     * late progress and send/connect uncertainty still require explicit close.
     * Never clear a fault established by the backend or release an active job. */
    bool empty_receive_timeout =
        receives(j->command) && status == BSP_TIMEOUT && j->socket->error.code == BSP_TIMEOUT &&
        !j->socket->error.native_valid && !j->io.bytes && !j->io.eof && !j->io.truncated;
    if (j->timed_out && enter && j->command != ECW_NET_CLOSE && !empty_receive_timeout)
    {
        j->socket->fault = true;
    }
    j->result = status;
    j->error = enter ? j->socket->error : (BspError){.code = status};
    if (j->error.code != status)
    {
        j->error = (BspError){.code = status};
    }
    j->done = true;
    j->active = false;
    bsp_request_unlock(key);
    /* No job or socket access after publication. Storage outlives task deletion. */
    vTaskDelete(NULL);
}
BspStatus ecw_bsp_net_job_start(EcwBspNetJob *j, EcwBspNetSocket *s, EcwBspNetCommand c,
                                const BspNetEndpoint *peer, const void *tx, size_t size,
                                uint32_t timeout, uint32_t stack_bytes, unsigned priority)
{
    if (!j || !s || c < ECW_NET_CONNECT || c > ECW_NET_CLOSE || timeout > INT32_MAX ||
        stack_bytes < 4096 || !priority || priority >= configMAX_PRIORITIES)
    {
        return BSP_INVALID_ARG;
    }
    if (stack_bytes % sizeof(StackType_t) ||
        (uint64_t)(configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)) !=
            stack_bytes / sizeof(StackType_t))
    {
        return BSP_INVALID_ARG;
    }
    if (c == ECW_NET_CONNECT || c == ECW_NET_SEND_TO)
    {
        if (!peer || !peer->port ||
            (peer->address.family != BSP_NET_IPV4 && peer->address.family != BSP_NET_IPV6))
        {
            return BSP_INVALID_ARG;
        }
        if (peer->address.family != s->family)
        {
            return BSP_UNSUPPORTED;
        }
    }
    if ((c == ECW_NET_SEND || c == ECW_NET_RECEIVE) && !size)
    {
        return BSP_INVALID_ARG;
    }
    if ((c == ECW_NET_SEND || c == ECW_NET_SEND_TO) && !tx && size)
    {
        return BSP_INVALID_ARG;
    }
    if (size > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (c == ECW_NET_SEND_TO && size > ECW_BSP_NET_DATAGRAM_MAX)
    {
        return BSP_UNSUPPORTED;
    }
    if (!timeout && c != ECW_NET_CLOSE)
    {
        return BSP_UNSUPPORTED;
    }
    uintptr_t key = bsp_request_lock();
    if (j->active)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    if (j->done || j->timed_out)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_STATE;
    }
    if (c != ECW_NET_CLOSE)
    {
        if (!s->owned || !s->configured)
        {
            bsp_request_unlock(key);
            return BSP_NOT_READY;
        }
        if (s->fault)
        {
            bsp_request_unlock(key);
            return BSP_IO;
        }
        bool udp = c == ECW_NET_SEND_TO || c == ECW_NET_RECEIVE_FROM;
        if (s->transport != (udp ? BSP_NET_UDP : BSP_NET_TCP))
        {
            bsp_request_unlock(key);
            return BSP_UNSUPPORTED;
        }
        if ((c == ECW_NET_SEND || c == ECW_NET_RECEIVE) && !s->connected)
        {
            bsp_request_unlock(key);
            return BSP_NOT_READY;
        }
        if (c == ECW_NET_CONNECT && s->connect_attempted)
        {
            bsp_request_unlock(key);
            return BSP_INVALID_STATE;
        }
    }
    j->active = true;
    j->command = c;
    j->socket = s;
    j->started_ms = bsp_time_ms();
    j->timeout_ms = timeout;
    j->size = size < sizeof(j->buffer) ? size : sizeof(j->buffer);
    j->io = (BspNetIo){0};
    j->error = (BspError){0};
    bsp_request_unlock(key);
    if (peer)
    {
        j->peer = *peer;
    }
    if ((c == ECW_NET_SEND || c == ECW_NET_SEND_TO) && j->size)
    {
        memcpy(j->buffer, tx, j->size);
    }
    /* P16 FreeRTOS stack depth is in StackType_t words; public configuration is bytes. */
    configSTACK_DEPTH_TYPE words = (configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t));
    BaseType_t created = xTaskCreate(run, "bsp_net_io", words, j, priority, NULL);
    if (created != pdPASS)
    {
        wipe(j);
        key = bsp_request_lock();
        j->active = false;
        j->socket = NULL;
        bsp_request_unlock(key);
        return BSP_NO_RESOURCE;
    }
    return BSP_OK;
}
BspStatus ecw_bsp_net_job_poll(EcwBspNetJob *j, void *rx, size_t capacity, BspNetEndpoint *peer,
                               BspNetIo *out)
{
    if (!j)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (!j->active && !j->done)
    {
        bsp_request_unlock(key);
        return BSP_NOT_READY;
    }
    if (j->timed_out)
    {
        bsp_request_unlock(key);
        return BSP_TIMEOUT;
    }
    if (!j->done)
    {
        if ((uint32_t)(bsp_time_ms() - j->started_ms) >= j->timeout_ms)
        {
            j->timed_out = true;
            bsp_request_unlock(key);
            return BSP_TIMEOUT;
        }
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    BspStatus status = j->result;
    if (status == BSP_OK)
    {
        if ((transfers(j->command) && !out) || (j->command == ECW_NET_RECEIVE_FROM && !peer) ||
            (receives(j->command) && (j->io.bytes > capacity || (!rx && j->io.bytes))))
        {
            bsp_request_unlock(key);
            return BSP_INVALID_ARG;
        }
        /* The completed job remains held until explicit release. Public owner is
         * serial; SDK no longer accesses buffer. Copy outside critical section. */
        bsp_request_unlock(key);
        if (receives(j->command) && j->io.bytes)
        {
            memcpy(rx, j->buffer, j->io.bytes);
        }
        if (j->command == ECW_NET_RECEIVE_FROM)
        {
            *peer = j->peer;
        }
        if (transfers(j->command))
        {
            *out = j->io;
        }
        return BSP_OK;
    }
    bsp_request_unlock(key);
    return status;
}
bool ecw_bsp_net_job_quiescent(EcwBspNetJob *j)
{
    if (!j)
    {
        return false;
    }
    uintptr_t key = bsp_request_lock();
    bool quiet = !j->active;
    bsp_request_unlock(key);
    return quiet;
}
BspStatus ecw_bsp_net_job_release(EcwBspNetJob *j)
{
    if (!j)
    {
        return BSP_INVALID_ARG;
    }
    uintptr_t key = bsp_request_lock();
    if (j->active)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    bsp_request_unlock(key);
    wipe(j);
    key = bsp_request_lock();
    j->done = false;
    j->timed_out = false;
    j->socket = NULL;
    bsp_request_unlock(key);
    return BSP_OK;
}
