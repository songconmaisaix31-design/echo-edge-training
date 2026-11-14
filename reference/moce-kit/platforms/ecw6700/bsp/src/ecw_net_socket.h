/* Private lwIP socket backend. Caller pins netif and excludes concurrent use/
 * close of this object. Public generation table and radio-stop integration own
 * that exclusion. No SDK descriptor escapes through bsp_net.h.
 * IMPORTANT: select timeouts below bound peer/readiness waiting only. Fixed P16
 * netconn_apimsg still waits indefinitely for its TCP/IP control message. Do not
 * expose this backend as finite public NET until that boundary is adapted. */
#ifndef ECW_BSP_NET_SOCKET_H
#define ECW_BSP_NET_SOCKET_H
#include "bsp_net.h"
#define ECW_BSP_NET_DATAGRAM_MAX 1472u
typedef struct
{
    int fd;
    bool owned, configured, fault, connect_attempted, connected;
    BspNetTransport transport;
    BspNetFamily family;
    unsigned netif_index;
    BspError error;
} EcwBspNetSocket;
/* IPv4/IPv6; caller pins this native netif index (1..255) against removal/reuse
 * until socket close and all retained jobs finish. No index comes from modules.
 * Caller retains partially created owned socket on setup failure. */
bool ecw_bsp_net_socket_ipv6_supported(void);
BspStatus ecw_bsp_net_socket_open(EcwBspNetSocket *socket, unsigned netif_index,
                                  BspNetTransport transport, BspNetFamily family);
BspStatus ecw_bsp_net_socket_connect(EcwBspNetSocket *socket, const BspNetEndpoint *peer,
                                     uint32_t timeout);
BspStatus ecw_bsp_net_socket_send(EcwBspNetSocket *socket, const void *data, size_t size,
                                  uint32_t timeout, BspNetIo *out);
BspStatus ecw_bsp_net_socket_receive(EcwBspNetSocket *socket, void *data, size_t capacity,
                                     uint32_t timeout, BspNetIo *out);
BspStatus ecw_bsp_net_socket_send_to(EcwBspNetSocket *socket, const BspNetEndpoint *peer,
                                     const void *data, size_t size, uint32_t timeout,
                                     BspNetIo *out);
BspStatus ecw_bsp_net_socket_receive_from(EcwBspNetSocket *socket, BspNetEndpoint *peer, void *data,
                                          size_t capacity, uint32_t timeout, BspNetIo *out);
/* Worker-only close; may wait for TCP/IP control acknowledgement. It is NOT a
 * finite public close. Caller must retain socket/netif until this returns OK. */
BspStatus ecw_bsp_net_socket_close(EcwBspNetSocket *socket);
#endif
