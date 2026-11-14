#ifndef BSP_SPP_H
#define BSP_SPP_H
#include "bsp_radio.h"
#ifdef __cplusplus
extern "C"
{
#endif
    /* Classic Bluetooth Serial Port Profile; NOT BLE GATT or Bluetooth audio.
 * Task context only. Same-protocol portability, no automatic BLE conversion. */
    enum
    {
        BSP_SPP_LISTEN = 1u,
        BSP_SPP_CONNECT = 2u,
        BSP_SPP_DISCOVER = 4u,
        BSP_SPP_FIXED_PIN = 8u,
        BSP_SPP_JUST_WORKS = 16u
    };
    typedef struct
    {
        uint32_t operations;
        uint16_t max_connections, max_write_bytes, rx_buffer_bytes;
    } BspSppCapabilities;
    typedef enum
    {
        BSP_SPP_OFF,
        BSP_SPP_STARTING,
        BSP_SPP_READY,
        BSP_SPP_STOPPING,
        BSP_SPP_FAULT
    } BspSppPhase;
    typedef enum
    {
        BSP_SPP_NONE,
        BSP_SPP_INIT,
        BSP_SPP_SERVER,
        BSP_SPP_DISCOVERY,
        BSP_SPP_CONNECTION,
        BSP_SPP_DISCONNECTION,
        BSP_SPP_STOP
    } BspSppCommand;
    typedef enum
    {
        BSP_SPP_SECURITY_NONE,
        BSP_SPP_SECURITY_ENCRYPTED
    } BspSppSecurity;
    typedef enum
    {
        BSP_SPP_RECONNECT_DISABLED,
        /* One native reconnect command is submitted after an unsolicited
         * link close. This is an opt-in event response, not an unbounded
         * retry loop. Requires an idle outgoing-client channel: pending TX,
         * unconsumed TX results, other commands or explicit disconnect suppress
         * this attempt; it is not deferred. Application owns further retries. */
        BSP_SPP_RECONNECT_ON_LINK_LOSS
    } BspSppReconnectPolicy;
    typedef struct
    {
        const char *device_name; /* NUL-terminated, 1..31 bytes, copied on init. */
        const uint8_t *pin;      /* Optional fixed legacy PIN; copied, not persisted by BSP. */
        size_t pin_length;       /* 0 rejects legacy PIN pairing; otherwise 1..16 bytes. */
        bool allow_just_works;   /* Explicit permission for SSP without MITM authentication. */
        BspSppReconnectPolicy reconnect_policy;
    } BspSppConfig;
    typedef struct
    {
        const char *service_name; /* NUL-terminated, 1..31 bytes. */
        uint8_t channel;          /* 0: native selects; otherwise 1..30. */
        BspSppSecurity security;
    } BspSppListenConfig;
    typedef struct
    {
        uint8_t address[6]; /* Display order, most significant byte first. */
        uint8_t channel;    /* 1..30, optionally obtained from discovery. */
        BspSppSecurity security;
    } BspSppConnectConfig;
    typedef struct
    {
        uint64_t generation;
    } BspSppConnection;
#define BSP_SPP_DISCOVERY_CHANNELS 31u
    typedef struct
    {
        BspSppPhase phase;
        BspSppCommand command;
        bool command_pending, listening, connected, congested, write_pending, write_result_ready;
        BspSppConnection connection;
        uint8_t peer[6], server_channel;
        uint8_t discovery_count, discovery_channels[BSP_SPP_DISCOVERY_CHANNELS];
        size_t rx_available;
        uint32_t dropped_rx_bytes;
        BspError error, data_error; /* Control command and latest stream error, respectively. */
    } BspSppState;
    typedef struct
    {
        uint64_t operation;
        BspError error;
        size_t bytes;
        bool count_valid;
    } BspSppWriteResult;

    BspStatus bsp_spp_capabilities(BspRadio radio, BspSppCapabilities *out);
    /* Init/listen/discover/connect/disconnect/stop OK means native command accepted.
 * Query reports observed native completion and errors, not a peer application ACK.
 * Reconnect policy is explicit. DISABLED performs no reconnect. ON_LINK_LOSS
 * submits at most one native reconnect command after an unsolicited close;
 * it does not schedule retries or hide a failed command. One command at a time;
 * delays are native-driven.
 * No caller configuration pointer survives a return. No per-command hard deadline
 * or forced cancellation is promised; application may request stop after admission. */
    BspStatus bsp_spp_init(BspRadio radio, const BspSppConfig *config);
    BspStatus bsp_spp_query(BspRadio radio, BspSppState *out);
    BspStatus bsp_spp_listen(BspRadio radio, const BspSppListenConfig *config);
    BspStatus bsp_spp_discover(BspRadio radio, const uint8_t address[6]);
    BspStatus bsp_spp_connect(BspRadio radio, const BspSppConnectConfig *config);
    BspStatus bsp_spp_disconnect(BspRadio radio, BspSppConnection connection);
    /* Byte stream, not messages. Write copies data; caller memory reusable on EVERY
 * return. OK publishes operation; consume exactly one result before next write.
 * Congested/pending/unconsumed result: BUSY, no native write or borrowing.
 * Native write completion is NOT peer consumption. An error may have sent bytes. */
    BspStatus bsp_spp_write(BspRadio radio, BspSppConnection connection, const void *data,
                            size_t length, uint64_t *operation);
    BspStatus bsp_spp_write_result(BspRadio radio, BspSppWriteResult *out);
    /* Zero-wait buffered read: valid empty cache -> OK/count=0. NULL data/count or
 * capacity=0 -> INVALID_ARG, no consumption and outputs unchanged. Cached bytes
 * remain readable after disconnect for the same connection generation. A newly
 * accepted connection replaces the old cache. Overflow drops newest bytes and
 * increments dropped_rx_bytes (saturating); no lossless reception guarantee. */
    BspStatus bsp_spp_read(BspRadio radio, BspSppConnection connection, void *data, size_t capacity,
                           size_t *count);
    /* Stops this SPP profile and its connections. OFF follows successful native
 * profile retirement. Shared controller/host storage may remain initialized;
 * this is not RF power-off, pairing-store erasure or other-profile shutdown.
 * Pending write finishes by native WRITE or profile retirement; no fake success.
 * Completed write result remains queryable. Re-init needs that result consumed. */
    BspStatus bsp_spp_stop(BspRadio radio);
#ifdef __cplusplus
}
#endif
#endif
