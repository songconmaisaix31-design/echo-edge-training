#ifndef ECW_BSP_NETIF_P16_H
#define ECW_BSP_NETIF_P16_H
#include "bsp_core.h"
typedef struct
{
    unsigned index; /* Native lwIP index, never a Wi-Fi VIF. */
    bool link_up, ipv4_ready;
    uint8_t ipv4[4]; /* Network byte order. */
} EcwBspNetifSnapshot;
/* Private retained lifecycle worker ONLY, after SDK TCP/IP initialization.
 * Must not run in TCP/IP thread or from its callbacks. SDK wait can be unbounded;
 * never call directly from a public finite-budget API or delete this worker.
 * Snapshot OK is an observation, not a netif lease, association proof, event
 * generation fence or authority to publish a new Wi-Fi operation as ready.
 * out is unchanged on failure; error receives a native error when available. */
BspStatus ecw_bsp_netif_snapshot(EcwBspNetifSnapshot *out, BspError *error);
#endif
