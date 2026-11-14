#ifndef ECW_BSP_WIFI_NET_H
#define ECW_BSP_WIFI_NET_H
#include "bsp_wifi.h"
/* Private single native STA ownership boundary, exactly four socket leases.
 * Lease objects are zero-initialized, stable socket-slot storage. No caller or
 * module receives native indexes. Copying a token does not transfer ownership. */
typedef struct
{
    unsigned netif_index;
    uint64_t operation, epoch, ticket;
    bool active;
} EcwBspWifiNetLease;

/* Wi-Fi lifecycle owner only. Publish only after native association/IP readiness
 * and a checked VIF->lwIP index lookup. The owner excludes external native netif
 * removal/reassignment; this table does not intercept SDK calls. operation must
 * not decrease; epoch must increase boot-wide on EVERY published link, including
 * a native reconnect within one operation. Reuse requires prior revoke, native
 * socket/job cleanup and lease retirement. Epochs identify BSP observations, not
 * authenticated native events or connection-command completion. */
BspStatus ecw_bsp_wifi_net_publish(BspRadio radio, unsigned netif_index, uint64_t operation,
                                   uint64_t epoch);
/* Immediate logical revocation on stop/link loss, before queuing cleanup. This
 * does not shut down SDK hardware or revoke storage from outstanding holders. */
BspStatus ecw_bsp_wifi_net_revoke(uint64_t operation, uint64_t epoch);
/* BUSY until revoked and every socket lease retired. Successful retirement is
 * permission for the owner to proceed with native cleanup, NOT its completion. */
BspStatus ecw_bsp_wifi_net_retire(uint64_t operation, uint64_t epoch);
/* Atomic release gate: BUSY leaves admission unchanged while sockets exist.
 * Otherwise withdraw publication before native service release. */
BspStatus ecw_bsp_wifi_net_withdraw(void);
BspStatus ecw_bsp_wifi_net_acquire(BspRadio radio, EcwBspWifiNetLease *lease);
BspStatus ecw_bsp_wifi_net_available(const EcwBspWifiNetLease *lease);
/* Only after native socket closed AND retained jobs stopped referencing it. */
BspStatus ecw_bsp_wifi_net_release(EcwBspWifiNetLease *lease);
#endif
