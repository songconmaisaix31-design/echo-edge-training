/* Native resolver with bounded caller wait. Static job storage outlives timeout.
 * Global lwIP routing/cache is intentional; this is not a radio-bound query. */
#include "ecw_net_dns.h"
#include "bsp_request_internal.h"
#include "bsp_wifi.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include <string.h>

bool ecw_bsp_net_dns_supported(void)
{
    return LWIP_DNS && LWIP_SOCKET;
}
#if LWIP_DNS && LWIP_SOCKET
static struct
{
    bool active, caller_busy;
    uint32_t started_ms, timeout_ms;
    char name[256];
    BspNetFamily family;
    BspNetAddress address;
    BspStatus result;
} job;

static BspStatus resolve_native(BspNetAddress *out)
{
    struct addrinfo hints = {0};
    struct addrinfo *answer = NULL;
    hints.ai_family = job.family == BSP_NET_IPV4 ? AF_INET : AF_INET6;
    int raw = lwip_getaddrinfo(job.name, NULL, &hints, &answer);
    BspStatus status = raw == EAI_MEMORY   ? BSP_NO_RESOURCE
                       : raw == EAI_FAMILY ? BSP_UNSUPPORTED
                                           : BSP_IO;
    if (raw == 0)
    {
        for (const struct addrinfo *entry = answer; entry; entry = entry->ai_next)
        {
            if (!entry->ai_addr || entry->ai_family != hints.ai_family)
            {
                continue;
            }
            if (entry->ai_family == AF_INET && entry->ai_addrlen >= sizeof(struct sockaddr_in))
            {
                const struct sockaddr_in *a = (const struct sockaddr_in *)entry->ai_addr;
                *out = (BspNetAddress){.family = BSP_NET_IPV4};
                memcpy(out->bytes, &a->sin_addr.s_addr, 4);
                status = BSP_OK;
                break;
            }
#if LWIP_IPV6
            if (entry->ai_family == AF_INET6 && entry->ai_addrlen >= sizeof(struct sockaddr_in6))
            {
                const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)entry->ai_addr;
                if (a->sin6_scope_id)
                {
                    status = BSP_UNSUPPORTED;
                    continue;
                }
                *out = (BspNetAddress){.family = BSP_NET_IPV6};
                memcpy(out->bytes, &a->sin6_addr, 16);
                status = BSP_OK;
                break;
            }
#endif
        }
    }
    if (answer)
    {
        lwip_freeaddrinfo(answer);
    }
    /* Free this one-shot task's native semaphore after the resolver returns. */
#if LWIP_NETCONN_SEM_PER_THREAD
    LWIP_NETCONN_THREAD_SEM_FREE();
#endif
    return status;
}
static void run(void *unused)
{
    (void)unused;
    BspNetAddress address = {0};
    BspStatus result = BSP_TIMEOUT;
    if ((uint32_t)(bsp_time_ms() - job.started_ms) < job.timeout_ms)
    {
        result = resolve_native(&address);
    }
    uintptr_t key = bsp_request_lock();
    if ((uint32_t)(bsp_time_ms() - job.started_ms) >= job.timeout_ms)
    {
        result = BSP_TIMEOUT;
    }
    job.result = result;
    job.address = address;
    job.active = false;
    bsp_request_unlock(key);
    /* No job access after publication; another caller may now reuse it. */
    vTaskDelete(NULL);
}
#endif
BspStatus ecw_bsp_net_dns_resolve(BspRadio radio, const char *name, BspNetFamily family,
                                  BspNetAddress *out, uint32_t timeout, uint32_t stack_bytes,
                                  unsigned priority)
{
    if (!name || !*name || !out || timeout > INT32_MAX ||
        (family != BSP_NET_IPV4 && family != BSP_NET_IPV6))
    {
        return BSP_INVALID_ARG;
    }
    size_t length = 0;
    while (length < 256 && name[length])
    {
        ++length;
    }
    if (length == 256)
    {
        return BSP_INVALID_ARG;
    }
#if LWIP_DNS && LWIP_SOCKET
    if ((family == BSP_NET_IPV6 && !LWIP_IPV6) || (family == BSP_NET_IPV4 && !LWIP_IPV4))
    {
        return BSP_UNSUPPORTED;
    }
    if (!stack_bytes)
    {
        return BSP_NOT_READY;
    }
    if (stack_bytes < 4096 || !priority || priority >= configMAX_PRIORITIES)
    {
        return BSP_INVALID_ARG;
    }
    if (stack_bytes % sizeof(StackType_t) ||
        (uint64_t)(configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)) !=
            stack_bytes / sizeof(StackType_t))
    {
        return BSP_INVALID_ARG;
    }
    if (!timeout)
    {
        return BSP_TIMEOUT;
    }
    BspWifiState state;
    BspStatus status = bsp_wifi_query(radio, &state);
    if (status != BSP_OK)
    {
        return status;
    }
    /* The SDK TCP/IP runtime must precede Wi-Fi service use. The global
     * resolver survives service stop; no socket lease or implicit init here. */
    if (state.phase == BSP_WIFI_OFF || state.phase == BSP_WIFI_STOPPING ||
        state.phase == BSP_WIFI_FAULT)
    {
        return BSP_NOT_READY;
    }
    uintptr_t key = bsp_request_lock();
    if (job.active || job.caller_busy)
    {
        bsp_request_unlock(key);
        return BSP_BUSY;
    }
    job.caller_busy = true;
    job.active = true;
    job.started_ms = bsp_time_ms();
    job.timeout_ms = timeout;
    job.family = family;
    bsp_request_unlock(key);
    memcpy(job.name, name, length + 1);
    BaseType_t created =
        xTaskCreate(run, "bsp_dns", (configSTACK_DEPTH_TYPE)(stack_bytes / sizeof(StackType_t)),
                    NULL, priority, NULL);
    if (created != pdPASS)
    {
        key = bsp_request_lock();
        job.active = false;
        job.caller_busy = false;
        bsp_request_unlock(key);
        return BSP_NO_RESOURCE;
    }
    for (;;)
    {
        key = bsp_request_lock();
        if (!job.active)
        {
            status = job.result;
            if (status == BSP_OK)
            {
                *out = job.address;
            }
            job.caller_busy = false;
            bsp_request_unlock(key);
            return status;
        }
        if ((uint32_t)(bsp_time_ms() - job.started_ms) >= timeout)
        {
            job.caller_busy = false;
            bsp_request_unlock(key);
            return BSP_TIMEOUT;
        }
        bsp_request_unlock(key);
        vTaskDelay(1);
    }
#else
    (void)radio;
    (void)stack_bytes;
    (void)priority;
    return BSP_UNSUPPORTED;
#endif
}
