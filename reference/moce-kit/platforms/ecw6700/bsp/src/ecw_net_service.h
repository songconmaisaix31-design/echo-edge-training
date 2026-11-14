#ifndef ECW_BSP_NET_SERVICE_H
#define ECW_BSP_NET_SERVICE_H
#include "bsp_net.h"
BspStatus ecw_bsp_net_configure(uint32_t stack_bytes, unsigned priority);
/* Wi-Fi worker hooks: no socket SDK call executes directly in these hooks. */
void ecw_bsp_net_service(void);
bool ecw_bsp_net_pending(void);
/* A delayed cleanup from an old link must not close sockets of a new link in
 * the same native-reconnecting Wi-Fi operation. Both identifiers are required. */
void ecw_bsp_net_retry_closes(uint64_t operation, uint64_t epoch);
BspStatus ecw_bsp_net_close_all(uint64_t operation, uint64_t epoch);
#endif
