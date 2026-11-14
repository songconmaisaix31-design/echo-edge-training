#include "ecw_net_socket.h"
#include "lwip/sockets.h"
#include "lwip/netifapi.h"
#if !LWIP_NETIF_TX_SINGLE_PBUF && !defined(CONFIG_ECW6700)
#error "UDP return requires fixed P16 ECW copy-on-send or SINGLE_PBUF"
#endif
#include <errno.h>
#include <limits.h>
#include <string.h>
#define ERRNO_DOMAIN 0x45434e45u
static BspStatus error(EcwBspNetSocket *s, int raw)
{
    BspStatus status = raw == ENOMEM || raw == ENOBUFS || raw == ENFILE || raw == EMFILE
                           ? BSP_NO_RESOURCE
                       : raw == ETIMEDOUT ? BSP_TIMEOUT
                                          : BSP_IO;
    s->error = (BspError){
        .code = status, .native_valid = true, .native_domain = ERRNO_DOMAIN, .native_code = raw};
    return status;
}
static BspStatus plain(EcwBspNetSocket *s, BspStatus status)
{
    s->error = (BspError){.code = status};
    return status;
}
static bool would_block(int e)
{
    return e == EAGAIN || e == EWOULDBLOCK;
}
static BspStatus usable(EcwBspNetSocket *s, BspNetTransport kind)
{
    if (!s)
    {
        return BSP_INVALID_ARG;
    }
    if (!s->owned || !s->configured)
    {
        return BSP_NOT_READY;
    }
    if (s->transport != kind)
    {
        return BSP_UNSUPPORTED;
    }
    return s->fault ? BSP_IO : BSP_OK;
}
typedef union
{
    struct sockaddr_in v4;
#if LWIP_IPV6
    struct sockaddr_in6 v6;
#endif
} NativeAddress;
bool ecw_bsp_net_socket_ipv6_supported(void)
{
    return LWIP_IPV6 != 0;
}
#if LWIP_IPV6
static bool mapped_v4(const uint8_t *address)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(address, prefix, sizeof(prefix)) == 0;
}
static bool scoped_v6(const uint8_t *address)
{
    return (address[0] == 0xfe && (address[1] & 0xc0u) == 0x80u) ||
           (address[0] == 0xff && ((address[1] & 0x0fu) == 1u || (address[1] & 0x0fu) == 2u));
}
#endif
static BspStatus endpoint(const EcwBspNetSocket *socket, const BspNetEndpoint *peer,
                          NativeAddress *native, socklen_t *length)
{
    if (!peer || !peer->port ||
        (peer->address.family != BSP_NET_IPV4 && peer->address.family != BSP_NET_IPV6))
    {
        return BSP_INVALID_ARG;
    }
    if (peer->address.family != socket->family)
    {
        return BSP_UNSUPPORTED;
    }
    memset(native, 0, sizeof(*native));
    if (peer->address.family == BSP_NET_IPV4)
    {
        native->v4.sin_len = sizeof(native->v4);
        native->v4.sin_family = AF_INET;
        native->v4.sin_port = htons(peer->port);
        memcpy(&native->v4.sin_addr.s_addr, peer->address.bytes, 4);
        *length = sizeof(native->v4);
        return BSP_OK;
    }
#if LWIP_IPV6
    /* An IPv6 socket stays IPv6-only. No implicit IPv4-mapped fallback. */
    if (mapped_v4(peer->address.bytes))
    {
        return BSP_UNSUPPORTED;
    }
    native->v6.sin6_len = sizeof(native->v6);
    native->v6.sin6_family = AF_INET6;
    native->v6.sin6_port = htons(peer->port);
    memcpy(&native->v6.sin6_addr, peer->address.bytes, 16);
    native->v6.sin6_scope_id = scoped_v6(peer->address.bytes) ? socket->netif_index : 0;
    *length = sizeof(native->v6);
    return BSP_OK;
#else
    return BSP_UNSUPPORTED;
#endif
}
static BspStatus received_endpoint(const EcwBspNetSocket *socket, const NativeAddress *native,
                                   socklen_t length, BspNetEndpoint *out)
{
    BspNetEndpoint result = {0};
    if (socket->family == BSP_NET_IPV4 && length == sizeof(native->v4) &&
        native->v4.sin_family == AF_INET)
    {
        result.address.family = BSP_NET_IPV4;
        result.port = ntohs(native->v4.sin_port);
        memcpy(result.address.bytes, &native->v4.sin_addr.s_addr, 4);
    }
#if LWIP_IPV6
    else if (socket->family == BSP_NET_IPV6 && length == sizeof(native->v6) &&
             native->v6.sin6_family == AF_INET6)
    {
        result.address.family = BSP_NET_IPV6;
        result.port = ntohs(native->v6.sin6_port);
        memcpy(result.address.bytes, &native->v6.sin6_addr, 16);
        if (mapped_v4(result.address.bytes) ||
            (native->v6.sin6_scope_id && native->v6.sin6_scope_id != socket->netif_index))
        {
            return BSP_IO;
        }
    }
#endif
    else
    {
        return BSP_IO;
    }
    *out = result;
    return BSP_OK;
}
static BspStatus wait_ready(EcwBspNetSocket *s, bool write, uint32_t start, uint32_t timeout)
{
    for (;;)
    {
        uint32_t elapsed = bsp_time_ms() - start;
        if (elapsed >= timeout)
        {
            return plain(s, BSP_TIMEOUT);
        }
        uint32_t left = timeout - elapsed;
        struct timeval tv = {.tv_sec = (long)(left / 1000),
                             .tv_usec = (long)((left % 1000) * 1000)};
        fd_set ready, exception;
        FD_ZERO(&ready);
        FD_ZERO(&exception);
        FD_SET(s->fd, &ready);
        FD_SET(s->fd, &exception);
        int result =
            lwip_select(s->fd + 1, write ? NULL : &ready, write ? &ready : NULL, &exception, &tv);
        if (result > 0)
        {
            if ((uint32_t)(bsp_time_ms() - start) >= timeout)
            {
                return plain(s, BSP_TIMEOUT);
            }
            return BSP_OK;
        }
        if (!result)
        {
            return plain(s, BSP_TIMEOUT);
        }
        int raw = errno;
        if (raw != EINTR)
        {
            return error(s, raw);
        }
    }
}
BspStatus ecw_bsp_net_socket_open(EcwBspNetSocket *s, unsigned netif_index, BspNetTransport kind,
                                  BspNetFamily family)
{
    if (!s || !netif_index || netif_index > 255u || (kind != BSP_NET_TCP && kind != BSP_NET_UDP) ||
        (family != BSP_NET_IPV4 && family != BSP_NET_IPV6))
    {
        return BSP_INVALID_ARG;
    }
    if (family == BSP_NET_IPV6 && !ecw_bsp_net_socket_ipv6_supported())
    {
        return BSP_UNSUPPORTED;
    }
    if (s->owned)
    {
        return BSP_BUSY;
    }
    struct ifreq iface = {0};
    /* P16 builds netifapi.c but omits the POSIX if_api.c wrapper. Use the
     * same TCP/IP-thread operation; do not read the global netif list here. */
    if (netifapi_netif_index_to_name((uint8_t)netif_index, iface.ifr_name) != ERR_OK)
    {
        return error(s, ENXIO);
    }
    /* Empty name would remove binding and allow the default route. Refuse it. */
    if (!iface.ifr_name[0])
    {
        return plain(s, BSP_IO);
    }
    int domain = AF_INET;
#if LWIP_IPV6
    if (family == BSP_NET_IPV6)
    {
        domain = AF_INET6;
    }
#endif
    int fd = lwip_socket(domain, kind == BSP_NET_TCP ? SOCK_STREAM : SOCK_DGRAM,
                         kind == BSP_NET_TCP ? IPPROTO_TCP : IPPROTO_UDP);
    if (fd < 0)
    {
        return error(s, errno);
    }
    *s = (EcwBspNetSocket){
        .fd = fd, .owned = true, .transport = kind, .family = family, .netif_index = netif_index};
    unsigned long nonblocking = 1;
    if (lwip_ioctl(fd, FIONBIO, &nonblocking) < 0)
    {
        return error(s, errno);
    }
    if (lwip_setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &iface, sizeof(iface)) < 0)
    {
        return error(s, errno);
    }
#if LWIP_IPV6
    if (family == BSP_NET_IPV6)
    {
        int only = 1;
        if (lwip_setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only)) < 0)
        {
            return error(s, errno);
        }
    }
#endif
    s->configured = true;
    return plain(s, BSP_OK);
}
BspStatus ecw_bsp_net_socket_close(EcwBspNetSocket *s)
{
    if (!s)
    {
        return BSP_INVALID_ARG;
    }
    if (!s->owned)
    {
        return BSP_OK;
    }
    if (lwip_close(s->fd) < 0)
    {
        return error(s, errno);
    }
    s->owned = false;
    s->configured = false;
    s->connected = false;
    s->connect_attempted = false;
    s->fd = -1;
    return plain(s, BSP_OK);
}
BspStatus ecw_bsp_net_socket_connect(EcwBspNetSocket *s, const BspNetEndpoint *peer,
                                     uint32_t timeout)
{
    if (!s || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    NativeAddress target;
    socklen_t target_length;
    BspStatus status = endpoint(s, peer, &target, &target_length);
    if (status != BSP_OK)
    {
        return status;
    }
    status = usable(s, BSP_NET_TCP);
    if (status != BSP_OK)
    {
        return status;
    }
    if (s->connect_attempted)
    {
        return BSP_INVALID_STATE;
    }
    uint32_t start = bsp_time_ms();
    s->connect_attempted = true;
    int result = lwip_connect(s->fd, (struct sockaddr *)&target, target_length);
    if (result < 0)
    {
        int raw = errno;
        if (raw != EINPROGRESS && raw != EWOULDBLOCK)
        {
            s->fault = true;
            return error(s, raw);
        }
        status = wait_ready(s, true, start, timeout);
        if (status != BSP_OK)
        {
            s->fault = true;
            return status;
        }
        int pending = 0;
        socklen_t length = sizeof(pending);
        if (lwip_getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &pending, &length) < 0)
        {
            s->fault = true;
            return error(s, errno);
        }
        if (length != sizeof(pending))
        {
            s->fault = true;
            return plain(s, BSP_IO);
        }
        if (pending)
        {
            s->fault = true;
            return error(s, pending);
        }
    }
    s->connected = true;
    return plain(s, BSP_OK);
}
static BspStatus stream(EcwBspNetSocket *s, void *data, size_t size, uint32_t timeout,
                        BspNetIo *out, bool transmit)
{
    if (!s || !data || !size || size > INT32_MAX || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = usable(s, BSP_NET_TCP);
    if (status != BSP_OK)
    {
        return status;
    }
    if (!s->connected)
    {
        return BSP_NOT_READY;
    }
    uint32_t start = bsp_time_ms();
    for (;;)
    {
        ssize_t n = transmit ? lwip_send(s->fd, data, size, MSG_DONTWAIT)
                             : lwip_recv(s->fd, data, size, MSG_DONTWAIT);
        if (n >= 0)
        {
            if ((size_t)n > size || (transmit && !n))
            {
                s->fault = true;
                return plain(s, BSP_IO);
            }
            *out = (BspNetIo){.bytes = (size_t)n, .eof = !transmit && !n};
            return plain(s, BSP_OK);
        }
        int raw = errno;
        if (!would_block(raw))
        {
            s->fault = true;
            return error(s, raw);
        }
        status = wait_ready(s, transmit, start, timeout);
        if (status != BSP_OK)
        {
            return status;
        }
    }
}
BspStatus ecw_bsp_net_socket_send(EcwBspNetSocket *s, const void *data, size_t size,
                                  uint32_t timeout, BspNetIo *out)
{
    return stream(s, (void *)data, size, timeout, out, true);
}
BspStatus ecw_bsp_net_socket_receive(EcwBspNetSocket *s, void *data, size_t size, uint32_t timeout,
                                     BspNetIo *out)
{
    return stream(s, data, size, timeout, out, false);
}
BspStatus ecw_bsp_net_socket_send_to(EcwBspNetSocket *s, const BspNetEndpoint *peer,
                                     const void *data, size_t size, uint32_t timeout, BspNetIo *out)
{
    if (!s || (!data && size) || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    if (size > ECW_BSP_NET_DATAGRAM_MAX)
    {
        return BSP_UNSUPPORTED;
    }
    NativeAddress target;
    socklen_t target_length;
    BspStatus status = endpoint(s, peer, &target, &target_length);
    if (status != BSP_OK)
    {
        return status;
    }
    status = usable(s, BSP_NET_UDP);
    if (status != BSP_OK)
    {
        return status;
    }
    uint8_t dummy = 0;
    uint32_t start = bsp_time_ms();
    for (;;)
    {
        ssize_t n = lwip_sendto(s->fd, data ? data : &dummy, size, MSG_DONTWAIT,
                                (struct sockaddr *)&target, target_length);
        if (n >= 0)
        {
            if ((size_t)n != size)
            {
                return plain(s, BSP_IO);
            }
            *out = (BspNetIo){.bytes = size};
            return plain(s, BSP_OK);
        }
        int raw = errno;
        if (!would_block(raw))
        {
            return error(s, raw);
        }
        status = wait_ready(s, true, start, timeout);
        if (status != BSP_OK)
        {
            return status;
        }
    }
}
BspStatus ecw_bsp_net_socket_receive_from(EcwBspNetSocket *s, BspNetEndpoint *peer, void *data,
                                          size_t capacity, uint32_t timeout, BspNetIo *out)
{
    if (!s || !peer || (!data && capacity) || capacity > INT32_MAX || !out || timeout > INT32_MAX)
    {
        return BSP_INVALID_ARG;
    }
    BspStatus status = usable(s, BSP_NET_UDP);
    if (status != BSP_OK)
    {
        return status;
    }
    uint32_t start = bsp_time_ms();
    uint8_t discard;
    for (;;)
    {
        NativeAddress source = {0};
        struct iovec vec = {.iov_base = capacity ? data : &discard,
                            .iov_len = capacity ? capacity : 1};
        struct msghdr message = {
            .msg_name = &source, .msg_namelen = sizeof(source), .msg_iov = &vec, .msg_iovlen = 1};
        /* P16 lwIP rejects a zero-size iovec; a one-byte temporary consumes zero-
   * capacity receives safely. Its return value is the FULL datagram length. */
        ssize_t n = lwip_recvmsg(s->fd, &message, MSG_DONTWAIT);
        if (n >= 0)
        {
            BspNetEndpoint from;
            if (received_endpoint(s, &source, message.msg_namelen, &from) != BSP_OK)
            {
                return plain(s, BSP_IO);
            }
            *peer = from;
            *out = (BspNetIo){.bytes = (size_t)n < capacity ? (size_t)n : capacity,
                              .truncated =
                                  (size_t)n > capacity || (message.msg_flags & MSG_TRUNC) != 0};
            return plain(s, BSP_OK);
        }
        int raw = errno;
        if (!would_block(raw))
        {
            return error(s, raw);
        }
        status = wait_ready(s, false, start, timeout);
        if (status != BSP_OK)
        {
            return status;
        }
    }
}
