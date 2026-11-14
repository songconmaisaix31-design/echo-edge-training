/* MOCE BSP 0.9.0 public contract; operation support and validation are platform-specific. */
#ifndef BSP_WIFI_H
#define BSP_WIFI_H
#include "bsp_radio.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        BSP_WIFI_STA = 1u,
        BSP_WIFI_AP = 2u,
        BSP_WIFI_SCAN = 4u,
        BSP_WIFI_STA_AP = 8u
    };
    typedef enum
    {
        BSP_WIFI_OFF,
        BSP_WIFI_IDLE,
        BSP_WIFI_CONNECTING,
        BSP_WIFI_LINK_UP,
        BSP_WIFI_IP_READY,
        BSP_WIFI_STOPPING,
        BSP_WIFI_FAULT,
        BSP_WIFI_DISCONNECTING,
        BSP_WIFI_DISCONNECTED
    } BspWifiPhase;
    typedef enum
    {
        BSP_WIFI_OPEN,
        BSP_WIFI_WPA2_PSK,
        BSP_WIFI_WPA3_SAE
    } BspWifiSecurity;
    typedef struct
    {
        uint8_t ssid[32], ssid_length;
        uint8_t secret[64], secret_length; /* Byte lengths, no implicit NUL; never logged. */
        BspWifiSecurity security;
    } BspWifiCredentials;
    typedef enum
    {
        BSP_WIFI_RECONNECT_DISABLED = 0,
        BSP_WIFI_RECONNECT_NATIVE = 1
    } BspWifiReconnectPolicy;
    typedef struct
    {
        bool disabled, native;
    } BspWifiReconnectCapabilities;
    typedef struct
    {
        BspWifiCredentials credentials;
        BspWifiReconnectPolicy reconnect_policy;
    } BspWifiConnectConfig;
    typedef struct
    {
        uint32_t operations;
        size_t scan_capacity;
    } BspWifiCapabilities;
    typedef struct
    {
        BspWifiPhase phase;
        uint64_t operation; /* Last admitted command, not the identity of phase. */
        BspError error;     /* Last connect-command result; use connect_result for new code. */
        bool
            settled; /* Latest command result fixed (native return, timeout, or explicit cancellation). */
        BspWifiPhase terminal_phase; /* Legacy diagnostic: CONNECTING=command OK, not IP ready. */
        uint32_t dropped_events;
    } BspWifiState;
    typedef struct
    {
        uint64_t operation; /* Zero for link observations: no native request ID. */
        BspWifiPhase phase;
        BspError error;
    } BspWifiEvent;
    typedef struct
    {
        uint8_t ssid[32], ssid_length, bssid[6];
        int16_t rssi_dbm;
        uint16_t channel;
        BspWifiSecurity security;
        bool security_known; /* false: security is not representable by this enum. */
    } BspWifiAccessPoint;
    typedef struct
    {
        uint64_t operation;
        bool completed; /* Result fixed, not permission to reclaim native job storage. */
        BspError error; /* Native command result; not authentication or IP proof. */
    } BspWifiConnectResult;
    /* Query the latest admitted connect command. OK means result was copied;
     * inspect completed and error. Unknown/replaced operation => INVALID_ARG.
     * Native completion is independent of cached link observations. Explicit
     * stop/disconnect before completion fixes INVALID_STATE (canceled), not OK. */
    BspStatus bsp_wifi_connect_result(BspRadio id, uint64_t operation, BspWifiConnectResult *out);
    typedef struct
    {
        BspWifiCredentials credentials;
        uint16_t channel; /* Requested native channel; 0 = native selection. */
        uint16_t
            max_clients; /* 0 = native maximum; positive limits must not be silently clamped. */
        bool hidden;
        bool concurrent_sta; /* Permit independent STA operation; does not connect STA. */
    } BspWifiApConfig;
    typedef struct
    {
        bool command_pending;   /* Native work still owns internal storage. */
        bool command_completed; /* Latest admitted command result fixed, possibly TIMEOUT. */
        BspError error;
        bool observation_valid;
        bool active; /* Latest native AP observation, not proof of command completion. */
    } BspWifiApState;
    /* Finite wait for AP command execution, not client association/DHCP readiness.
     * Copies config; never retains caller memory. TIMEOUT after admission may leave
     * native work running: query the retained state; no automatic retry/rollback.
     * Start does not disconnect STA or close sockets. AP-only with owned STA => BUSY.
     * Stop affects AP only. Full wifi_stop also releases an owned AP.
     * Concurrent mode uses native channel/coexistence policy; requested channel is
     * not a guarantee of actual RF channel. No implicit routing/NAT/portal or NV writes.
     * Reconfigure explicitly via AP stop/start. Zero timeout admits no new command.
     * query copies last command result and last processed native observation separately.
     * Known unsupported backends/parameter combinations reject before hardware effects.
     * Native failures after command admission retain acquired resources for explicit cleanup.
     * Existing NET sockets are STA-bound; AP bind/listen/DHCP configuration is not
     * provided by these operations. Native AP networking requires SDK integration. */
    BspStatus bsp_wifi_ap_start(BspRadio id, const BspWifiApConfig *config, uint32_t timeout_ms);
    BspStatus bsp_wifi_ap_query(BspRadio id, BspWifiApState *out);
    BspStatus bsp_wifi_ap_stop(BspRadio id, uint32_t timeout_ms);

    BspStatus bsp_wifi_capabilities(BspRadio id, BspWifiCapabilities *out);
    BspStatus bsp_wifi_init(BspRadio id);
    /* Copies credentials at acceptance. OK means command admitted, not connected.
     * timeout covers native command completion only; query connect_result separately.
     * Link phase is the SDK-reported observation and may lag the radio. A cached
     * IP_READY or a link event is not completion proof for this operation.
     * No BSP-issued retries or implicit persistence; default policy DISABLED.
     * operation is written only on OK; caller memory is never retained. */
    BspStatus bsp_wifi_connect(BspRadio id, const BspWifiCredentials *credentials,
                               uint32_t timeout_ms, uint64_t *operation);
    /* Query performs no initialization. A known unadapted radio returns OK with
     * both flags false. A policy is supported only when its full connect path is
     * available; chip/SDK support alone is insufficient. */
    BspStatus bsp_wifi_reconnect_capabilities(BspRadio id, BspWifiReconnectCapabilities *out);
    /* Zero-initialized policy is DISABLED, equivalent to bsp_wifi_connect.
     * Config/credentials are copied at acceptance, operation changes only on OK.
     * NATIVE explicitly permits SDK-managed retries/reconnection while owned;
     * intervals/counts are platform-specific, with no BSP-issued retry loop.
     * The deadline covers the native connect command, not association/DHCP.
     * The application decides how long to wait for observed connectivity and
     * when to disconnect; a command timeout does not secretly stop native Wi-Fi.
     * Native reconnect may continue until explicit disconnect/stop.
     * Link events carry operation=0 because native events have no command identity.
     * No credential persistence. Unsupported policy rejects before side effects.
     * Invalid policy/config => INVALID_ARG. Existing stop/buffer rules apply. */
    BspStatus bsp_wifi_connect_configured(BspRadio id, const BspWifiConnectConfig *config,
                                          uint32_t timeout_ms, uint64_t *operation);
    BspStatus bsp_wifi_query(BspRadio id, BspWifiState *out);
    /* Nonblocking copied events. Empty => OK/count0; loss reflected by query.dropped_events.
 * Terminal outcome remains queryable even when its event is dropped. */
    BspStatus bsp_wifi_events(BspRadio id, BspWifiEvent *out, size_t capacity, size_t *count);
    /* Bounded synchronous scan; no retained caller memory. On OK written<=capacity,
 * total is full SDK result count; written<total means explicitly truncated list. */
    BspStatus bsp_wifi_scan(BspRadio id, BspWifiAccessPoint *out, size_t capacity, size_t *written,
                            size_t *total, uint32_t timeout_ms);
    /* Disconnect STA without releasing the service or closing application sockets.
     * OK means the native disconnect call succeeded, not proof of link-down;
     * observe DISCONNECTED through query/events. The budget covers native-call
     * completion, not an RF/event retirement barrier. Zero budget admits no command;
     * an initialized service with no admitted connect returns OK. TIMEOUT after admission may finish later; no caller memory is held.
     * Explicit disconnect suppresses reconnect. A pending native command returns BUSY;
     * a cached DISCONNECTED phase never substitutes for issuing this command.
     * Native failure is reported as FAULT/event with its error; ownership remains.
     * Current adapters require explicit stop/init before a new connect operation.
     * Unsupported backends reject before side effects. */
    BspStatus bsp_wifi_disconnect(BspRadio id, uint32_t timeout_ms);
    /* Release the Wi-Fi service, also canceling an active connection.
     * Application must explicitly close its sockets first. Live socket ownership
     * returns BUSY before changing Wi-Fi state or retrying socket cleanup.
     * OK retires BSP-owned events/resources; it is not a mere disconnect ACK.
     * TIMEOUT/IO retains STOPPING/FAULT ownership; retry release explicitly.
     * No erase of persistent credentials, SDK storage or irreversible BT memory. */
    BspStatus bsp_wifi_stop(BspRadio id, uint32_t cleanup_timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
