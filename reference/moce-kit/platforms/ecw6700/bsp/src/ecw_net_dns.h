/* Private native DNS adapter. No borrowed caller storage after return. */
#ifndef ECW_BSP_NET_DNS_H
#define ECW_BSP_NET_DNS_H
#include "bsp_net.h"
bool ecw_bsp_net_dns_supported(void);
BspStatus ecw_bsp_net_dns_resolve(BspRadio radio, const char *name, BspNetFamily family,
                                  BspNetAddress *out, uint32_t timeout, uint32_t stack_bytes,
                                  unsigned priority);
#endif
