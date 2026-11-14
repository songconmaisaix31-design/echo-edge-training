/* Public IPv4 NET composition. Native status observations gate new operations;
 * application ownership of published sockets survives observed link loss. */
#include "ecw_net_service.h"
#include "ecw_net_job.h"
#include "ecw_net_dns.h"
#include "ecw_wifi_net.h"
#include "bsp_request_internal.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#ifdef BSP_ECW_NATIVE_NET
#include "ecw_wifi_service.h"
#endif
#define SOCKETS 4u
typedef struct
{
    bool used, published, api_busy, closing, close_failed, retired;
    uint64_t generation;
    BspRadio radio;
    BspError error;
    EcwBspNetSocket native;
    EcwBspNetJob job;
    EcwBspWifiNetLease lease;
} Slot;
static Slot slots[SOCKETS];
static uint64_t next_generation;
static uint32_t worker_stack;
static unsigned worker_priority;
static bool family_ok(BspNetFamily family)
{
    return family == BSP_NET_IPV4 || family == BSP_NET_IPV6;
}
static bool endpoint_ok(const BspNetEndpoint *p)
{
    return p && p->port && family_ok(p->address.family);
}
static BspStatus ready(void)
{
    return bsp_delay_ms(0);
}
static Slot *find(BspSocket h)
{
    if (h.slot >= SOCKETS || !h.generation)
    {
        return NULL;
    }
    Slot *s = &slots[h.slot];
    return s->used && s->published && s->generation == h.generation ? s : NULL;
}
static void release(Slot *s)
{
    uintptr_t key = bsp_request_lock();
    s->api_busy = false;
    bsp_request_unlock(key);
}
static BspStatus claim(BspSocket h, Slot **out, bool closing)
{
    BspStatus status = ready();
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    Slot *s = find(h);
    if (!s)
    {
        bsp_request_unlock(key);
        return BSP_INVALID_ARG;
    }
    if (s->api_busy || (!closing && (s->closing || s->job.active)))
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    s->api_busy = true;
    *out = s;
    bsp_request_unlock(key);
    return BSP_OK;
}
static void record(Slot *s, BspError error)
{
    uintptr_t key = bsp_request_lock();
    s->error = error;
    bsp_request_unlock(key);
}
BspStatus ecw_bsp_net_configure(uint32_t stack_bytes, unsigned priority)
{
    if ((stack_bytes || priority) &&
        (stack_bytes < 4096 || !priority || priority >= configMAX_PRIORITIES))
    {
        return BSP_INVALID_ARG;
    }
    if (stack_bytes && (stack_bytes % sizeof(StackType_t) ||
                        (uint64_t)(configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)) !=
                            stack_bytes / sizeof(StackType_t)))
    {
        return BSP_INVALID_ARG;
    }
    for (unsigned i = 0; i < SOCKETS; ++i)
    {
        if (slots[i].used)
        {
            return BSP_BUSY;
        }
    }
    worker_stack = stack_bytes;
    worker_priority = priority;
    return BSP_OK;
}
BspStatus bsp_net_capabilities(BspRadio radio, BspNetCapabilities *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspWifiCapabilities caps;
    BspStatus status = bsp_wifi_capabilities(radio, &caps);
    if (status != BSP_OK)
    {
        return status;
    }
    *out = (BspNetCapabilities){0};
    if (caps.operations & BSP_WIFI_STA)
    {
        *out = (BspNetCapabilities){.tcp = true,
                                    .dns = ecw_bsp_net_dns_supported(),
                                    .udp = true,
                                    .ipv4 = true,
                                    .ipv6 = ecw_bsp_net_socket_ipv6_supported(),
                                    .max_sockets = SOCKETS,
                                    .max_datagram = ECW_BSP_NET_DATAGRAM_MAX};
    }
    return BSP_OK;
}
/* api_busy is held by caller. Only retire a fully quiescent native object. */
static BspStatus retire(Slot *s)
{
    if (s->native.owned)
    {
        return BSP_BUSY;
    }
    BspStatus status = ecw_bsp_net_job_release(&s->job);
    if (status != BSP_OK)
    {
        return status;
    }
    if (s->lease.active)
    {
        status = ecw_bsp_wifi_net_release(&s->lease);
        if (status != BSP_OK)
        {
            return status;
        }
    }
    uintptr_t key = bsp_request_lock();
    s->used = false;
    s->published = false;
    s->retired = true;
    s->closing = false;
    bsp_request_unlock(key);
    return BSP_OK;
}
static BspStatus close_step(Slot *s)
{
    if (!ecw_bsp_net_job_quiescent(&s->job))
    {
        return BSP_BUSY;
    }
    if (s->job.done)
    {
        bool was_close = s->job.command == ECW_NET_CLOSE;
        BspStatus result = s->job.result;
        BspError error = s->job.error;
        (void)ecw_bsp_net_job_release(&s->job);
        if (was_close && result != BSP_OK)
        {
            record(s, error);
            s->close_failed = true;
            return result;
        }
    }
    if (!s->native.owned)
    {
        return retire(s);
    }
    if (s->close_failed)
    {
        return s->error.code == BSP_OK ? BSP_IO : s->error.code;
    }
    BspStatus status = ecw_bsp_net_job_start(&s->job, &s->native, ECW_NET_CLOSE, NULL, NULL, 0,
                                             INT32_MAX, worker_stack, worker_priority);
    if (status != BSP_OK)
    {
        record(s, (BspError){.code = status});
        s->close_failed = true;
        return status;
    }
    return BSP_BUSY;
}
BspStatus bsp_net_open(BspRadio radio, BspNetTransport transport, BspNetFamily family,
                       BspSocket *out)
{
    if (!out || !family_ok(family) || (transport != BSP_NET_TCP && transport != BSP_NET_UDP))
    {
        return BSP_INVALID_ARG;
    }
    BspNetCapabilities caps;
    BspStatus status = bsp_net_capabilities(radio, &caps);
    if (status != BSP_OK)
    {
        return status;
    }
    if ((family == BSP_NET_IPV4 && !caps.ipv4) || (family == BSP_NET_IPV6 && !caps.ipv6))
    {
        return BSP_UNSUPPORTED;
    }
    if (!worker_stack)
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = bsp_request_lock();
    unsigned i = 0;
    while (i < SOCKETS && (slots[i].used || slots[i].api_busy))
    {
        ++i;
    }
    if (i == SOCKETS || next_generation == UINT64_MAX)
    {
        bsp_request_unlock(key);
        return BSP_NO_RESOURCE;
    }
    Slot *s = &slots[i];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->api_busy = true;
    s->generation = ++next_generation;
    s->radio = radio;
    bsp_request_unlock(key);
    status = ecw_bsp_wifi_net_acquire(radio, &s->lease);
    if (status == BSP_OK)
    {
        status = ecw_bsp_net_socket_open(&s->native, s->lease.netif_index, transport, family);
    }
    if (status != BSP_OK)
    {
        record(s, s->native.error.code == status ? s->native.error : (BspError){.code = status});
        if (!s->native.owned)
        {
            BspStatus cleanup = retire(s);
            if (cleanup != BSP_OK)
            {
                s->closing = true;
            }
        }
        else
        {
            s->closing = true; /* Unpublished partial setup is serviced as an orphan. */
        }
        release(s);
#ifdef BSP_ECW_NATIVE_NET
        ecw_wifi_service_wake();
#endif
        return status;
    }
    key = bsp_request_lock();
    s->published = true;
    *out = (BspSocket){.slot = i, .generation = s->generation};
    bsp_request_unlock(key);
    release(s);
    return BSP_OK;
}
static BspStatus perform(BspSocket h, EcwBspNetCommand command, const BspNetEndpoint *destination,
                         const void *tx, void *rx, size_t size, uint32_t timeout,
                         BspNetEndpoint *from, BspNetIo *out)
{
    Slot *s;
    BspStatus status = claim(h, &s, false);
    if (status != BSP_OK)
    {
        return status;
    }
    if (s->job.done)
    {
        (void)ecw_bsp_net_job_release(&s->job);
    }
    status = ecw_bsp_wifi_net_available(&s->lease);
    if (status == BSP_OK)
    {
        status = ecw_bsp_net_job_start(&s->job, &s->native, command, destination, tx, size, timeout,
                                       worker_stack, worker_priority);
    }
    if (status != BSP_OK)
    {
        release(s);
        return status;
    }
    do
    {
        status = ecw_bsp_net_job_poll(&s->job, rx, size, from, out);
        if (status == BSP_BUSY)
        {
            vTaskDelay(1);
        }
    } while (status == BSP_BUSY);
    uintptr_t key = bsp_request_lock();
    BspError error =
        s->job.done && s->job.error.code == status ? s->job.error : (BspError){.code = status};
    bsp_request_unlock(key);
    record(s, error);
    if (ecw_bsp_net_job_quiescent(&s->job))
    {
        (void)ecw_bsp_net_job_release(&s->job);
    }
    release(s);
    return status;
}
BspStatus bsp_net_connect(BspSocket s, const BspNetEndpoint *peer, uint32_t timeout)
{
    if (!endpoint_ok(peer) || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return perform(s, ECW_NET_CONNECT, peer, NULL, NULL, 0, timeout, NULL, NULL);
}
BspStatus bsp_net_send(BspSocket s, const void *data, size_t n, uint32_t timeout, BspNetIo *out)
{
    if (!data || !n || n > INT32_MAX || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return perform(s, ECW_NET_SEND, NULL, data, NULL, n, timeout, NULL, out);
}
BspStatus bsp_net_receive(BspSocket s, void *data, size_t n, uint32_t timeout, BspNetIo *out)
{
    if (!data || !n || n > INT32_MAX || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return perform(s, ECW_NET_RECEIVE, NULL, NULL, data, n, timeout, NULL, out);
}
BspStatus bsp_net_send_to(BspSocket s, const BspNetEndpoint *peer, const void *data, size_t n,
                          uint32_t timeout, BspNetIo *out)
{
    if (!endpoint_ok(peer) || (!data && n) || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return perform(s, ECW_NET_SEND_TO, peer, data, NULL, n, timeout, NULL, out);
}
BspStatus bsp_net_receive_from(BspSocket s, BspNetEndpoint *peer, void *data, size_t n,
                               uint32_t timeout, BspNetIo *out)
{
    if (!peer || (!data && n) || n > INT32_MAX || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    return perform(s, ECW_NET_RECEIVE_FROM, NULL, NULL, data, n, timeout, peer, out);
}
BspStatus bsp_net_resolve(BspRadio radio, const char *name, BspNetFamily family, BspNetAddress *out,
                          uint32_t timeout)
{
    if (!name || !*name || !family_ok(family) || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspNetCapabilities caps;
    BspStatus status = bsp_net_capabilities(radio, &caps);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!caps.dns || (family == BSP_NET_IPV6 && !caps.ipv6) ||
        (family == BSP_NET_IPV4 && !caps.ipv4))
    {
        return BSP_UNSUPPORTED;
    }
    status = ready();
    if (status != BSP_OK)
    {
        return status;
    }
    return ecw_bsp_net_dns_resolve(radio, name, family, out, timeout, worker_stack,
                                   worker_priority);
}
BspStatus bsp_net_error(BspSocket h, BspError *out)
{
    if (!out)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ready();
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    Slot *s = find(h);
    if (s)
    {
        *out = s->error;
    }
    bsp_request_unlock(key);
    return s ? BSP_OK : BSP_INVALID_ARG;
}
BspStatus bsp_net_close(BspSocket h, uint32_t timeout)
{
    if (timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = ready();
    if (status != BSP_OK)
    {
        return status;
    }
    uintptr_t key = bsp_request_lock();
    bool retired = h.slot < SOCKETS && h.generation && slots[h.slot].generation == h.generation &&
                   slots[h.slot].retired;
    bsp_request_unlock(key);
    if (retired)
    {
        return BSP_OK;
    }
    Slot *s;
    status = claim(h, &s, true);
    if (status != BSP_OK)
    {
        return status;
    }
    s->closing = true;
    s->close_failed = false;
    uint32_t start = bsp_time_ms();
    for (;;)
    {
        status = close_step(s);
        if (status != BSP_BUSY)
        {
            break;
        }
        if ((uint32_t)(bsp_time_ms() - start) >= timeout)
        {
            status = BSP_TIMEOUT;
            record(s, (BspError){.code = status});
            break;
        }
        vTaskDelay(1);
    }
    release(s);
#ifdef BSP_ECW_NATIVE_NET
    ecw_wifi_service_wake();
#endif
    return status;
}
bool ecw_bsp_net_pending(void)
{
    uintptr_t key = bsp_request_lock();
    bool pending = false;
    for (unsigned i = 0; i < SOCKETS; ++i)
    {
        pending |= slots[i].used && slots[i].closing && !slots[i].close_failed;
    }
    bsp_request_unlock(key);
    return pending;
}
void ecw_bsp_net_service(void)
{
    for (unsigned i = 0; i < SOCKETS; ++i)
    {
        uintptr_t key = bsp_request_lock();
        Slot *s = &slots[i];
        bool take = s->used && !s->api_busy && s->closing && !s->close_failed;
        if (take)
        {
            s->api_busy = true;
        }
        bsp_request_unlock(key);
        if (take)
        {
            (void)close_step(s);
            release(s);
        }
    }
}
void ecw_bsp_net_retry_closes(uint64_t operation, uint64_t epoch)
{
    uintptr_t key = bsp_request_lock();
    for (unsigned i = 0; i < SOCKETS; ++i)
    {
        if (slots[i].used && !slots[i].api_busy && slots[i].lease.operation == operation &&
            slots[i].lease.epoch == epoch)
        {
            slots[i].close_failed = false;
        }
    }
    bsp_request_unlock(key);
}
BspStatus ecw_bsp_net_close_all(uint64_t operation, uint64_t epoch)
{
    BspStatus result = BSP_OK;
    for (unsigned i = 0; i < SOCKETS; ++i)
    {
        uintptr_t key = bsp_request_lock();
        Slot *s = &slots[i];
        bool target = s->used && s->lease.active && s->lease.operation == operation &&
                      s->lease.epoch == epoch;
        bool busy = target && s->api_busy;
        if (target && !busy)
        {
            s->api_busy = true;
            s->closing = true;
        }
        bsp_request_unlock(key);
        if (!target)
        {
            continue;
        }
        BspStatus status = busy ? BSP_BUSY : close_step(s);
        if (!busy)
        {
            release(s);
        }
        if (status != BSP_OK && (result == BSP_OK || result == BSP_BUSY))
        {
            result = status;
        }
    }
    return result;
}
