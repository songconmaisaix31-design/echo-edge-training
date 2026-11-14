#include "ecw_netif_p16.h"
#include "system_network.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <string.h>
#define LWIP_DOMAIN 0x4c574950u
typedef struct
{
    EcwBspNetifSnapshot snapshot;
    BspStatus status;
} Observation;
static void observe(void *context)
{
    Observation *o = context;
    struct netif *n = sys_netif_get_by_index(WIFI_VIF_STA);
    if (!n)
    {
        o->status = BSP_NOT_READY;
        return;
    }
    unsigned index = netif_get_index(n);
    if (!index || netif_get_by_index((u8_t)index) != n)
    {
        o->status = BSP_NOT_READY;
        return;
    }
    o->snapshot.index = index;
    o->snapshot.link_up = netif_is_up(n) && netif_is_link_up(n);
    o->snapshot.ipv4_ready = o->snapshot.link_up && !ip4_addr_isany(netif_ip4_addr(n));
    uint32_t address = ip4_addr_get_u32(netif_ip4_addr(n));
    memcpy(o->snapshot.ipv4, &address, sizeof(address));
    o->status = BSP_OK;
}
BspStatus ecw_bsp_netif_snapshot(EcwBspNetifSnapshot *out, BspError *error)
{
    if (!out || !error)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = bsp_delay_ms(0);
    if (status != BSP_OK)
    {
        *error = (BspError){.code = status};
        return status;
    }
    Observation o = {.status = BSP_NOT_READY};
    /* Original P16 callback_wait returns only after callback completion, or fails
  * before admission. Stack data therefore remains live throughout native use. */
    err_t native = tcpip_callback_wait(observe, &o);
    if (native != ERR_OK)
    {
        status = native == ERR_MEM ? BSP_NO_RESOURCE : BSP_IO;
        *error = (BspError){.code = status,
                            .native_valid = true,
                            .native_domain = LWIP_DOMAIN,
                            .native_code = native};
        return status;
    }
    *error = (BspError){.code = o.status};
    if (o.status == BSP_OK)
    {
        *out = o.snapshot;
    }
    return o.status;
}
