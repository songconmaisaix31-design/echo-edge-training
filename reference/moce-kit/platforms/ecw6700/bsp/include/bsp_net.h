/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_NET_H
#define BSP_NET_H
#include "bsp_radio.h"
#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct
    {
        uint32_t slot;
        uint64_t generation;
    } BspSocket;
    typedef enum
    {
        BSP_NET_TCP,
        BSP_NET_UDP
    } BspNetTransport;
    typedef enum
    {
        BSP_NET_IPV4 = 4,
        BSP_NET_IPV6 = 6
    } BspNetFamily;
    typedef struct
    {
        BspNetFamily family;
        uint8_t bytes[16];
    } BspNetAddress;
    typedef struct
    {
        BspNetAddress address;
        uint16_t port;
    } BspNetEndpoint;
    typedef struct
    {
        bool tcp, udp, ipv4, ipv6, dns;
        size_t max_sockets, max_datagram;
    } BspNetCapabilities;
    typedef struct
    {
        size_t bytes;
        bool eof, truncated;
    } BspNetIo;
    /* IP bytes are network order; port is host numeric value. No SDK socket exposed.
 * TCP/UDP sockets bind to radio's IP interface; DNS follows native routing below.
 * Handles carry generations so stale handles cannot access recycled sockets.
 * IPv6 scoped addresses use the selected radio interface's scope; no OS scope ID
 * enters module code. Current adapters use IPv6-only sockets; IPv4-mapped IPv6
 * endpoints and mixing endpoint/socket families are UNSUPPORTED.
 * An observed link permits interface ownership without waiting for IPv4 DHCP.
 * Socket open does not prove address validity, DAD completion or remote reachability;
 * native connect/send errors retain their meaning. IPv6 requires SDK build/runtime
 * support. Address configuration/SLAAC/DHCPv6 follow the selected native stack. */
    BspStatus bsp_net_capabilities(BspRadio id, BspNetCapabilities *out);
    BspStatus bsp_net_open(BspRadio id, BspNetTransport transport, BspNetFamily family,
                           BspSocket *out);
    BspStatus bsp_net_connect(BspSocket socket, const BspNetEndpoint *peer, uint32_t timeout_ms);
    /* TCP only; positive length. Short progress is OK with bytes<n. recv EOF is
 * OK/bytes0/eof=true. No data at deadline is TIMEOUT, never EOF.
 * Return releases caller buffer; local acceptance is not peer delivery. */
    BspStatus bsp_net_send(BspSocket socket, const void *data, size_t n, uint32_t timeout_ms,
                           BspNetIo *out);
    BspStatus bsp_net_receive(BspSocket socket, void *data, size_t capacity, uint32_t timeout_ms,
                              BspNetIo *out);
    /* UDP one call=one datagram, including zero-length datagrams. Oversize send rejects
 * before effects, no fragmentation by wrapper. Receive truncation consumes the
 * whole datagram, reports copied bytes/truncated; never turns remainder into another. */
    BspStatus bsp_net_send_to(BspSocket socket, const BspNetEndpoint *peer, const void *data,
                              size_t n, uint32_t timeout_ms, BspNetIo *out);
    BspStatus bsp_net_receive_from(BspSocket socket, BspNetEndpoint *peer, void *data,
                                   size_t capacity, uint32_t timeout_ms, BspNetIo *out);
    /* Native DNS/cache lookup: radio selects a backend, not a query egress interface.
     * Native DNS servers, caching, routing and resolver retries apply. BSP adds no
     * retry, interface routing override, connection or socket ownership.
     * Nonempty NUL-terminated name <=255 bytes; returns one address of the requested
     * family in network byte order. No IPv4/IPv6 fallback or caller-supplied scope.
     * Output changes only on OK. Caller name/output are released on every return.
     * timeout_ms bounds local waiting, not native resolver cancellation: late native
     * work uses copied internal storage, and can keep the resolver BUSY until it ends.
     * Zero timeout admits no work and returns TIMEOUT on a supported, ready backend.
     * Native allocation failure => NO_RESOURCE; unavailable family => UNSUPPORTED;
     * name-not-found/other native resolution failures => IO (not a fabricated timeout).
     * Current native adapters require NET worker configuration and initialized Wi-Fi;
     * success does not prove server reachability. Native DNS errors have no socket
     * handle and are not available through bsp_net_error. Missing backends remain
     * UNSUPPORTED without native calls or borrowing buffers. */
    BspStatus bsp_net_resolve(BspRadio id, const char *hostname, BspNetFamily family,
                              BspNetAddress *out, uint32_t timeout_ms);
    /* Finite local close, no promise of peer ACK. Failure retains handle for cleanup.
     * Application explicitly closes each socket before releasing the Wi-Fi service.
     * Link loss does not transfer socket ownership to Wi-Fi; close remains required. */
    BspStatus bsp_net_close(BspSocket socket, uint32_t cleanup_timeout_ms);
    BspStatus bsp_net_error(BspSocket socket, BspError *out);

#ifdef __cplusplus
}
#endif
#endif
