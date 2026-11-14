/**
 * @file system_wifi.h
 * @brief WiFi Control System - Public API Layer
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2018-2025 Eswin Computing. All rights reserved.
 */

#ifndef _SYSTEM_WIFI_H
#define _SYSTEM_WIFI_H

/* Keep one section per function; the linker selects RAM optimization. */
#ifndef WIFI_TX_HOT
#define WIFI_TX_HOT(name) __attribute__((section(".text.hot_wifi_tx." #name)))
#define WIFI_RX_HOT(name) __attribute__((section(".text.hot_wifi_rx." #name)))
#endif

#include "system_wifi_def.h"
#include "eswin_wifi_types.h"  /* WiFi type definitions for all layers */
#include "eswin_errno.h"


#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif

#ifdef __cplusplus
    extern "C" {
#endif

/****************************************************************************
 * MODULE DOCUMENTATION
 ****************************************************************************
 *
 * @brief WiFi Control System - Public API Layer
 *
 * OVERVIEW
 * ========
 * This module provides a unified, high-level WiFi control interface for
 * ECW6630/ECW6700 platforms. It abstracts WPA supplicant complexity and
 * provides type-safe, well-documented APIs for WiFi operations.
 *
 * ARCHITECTURE
 * ============
 *
 *   +------------------------------------------------------------------+
 *   |                     Application Layer                            |
 *   |  +------------------+  +------------------+  +------------------+ |
 *   |  | STA Operations   |  | AP Operations    |  | Monitor/Sniffer  | |
 *   |  | connect/scan     |  | start/stop       |  | capture packets  | |
 *   |  +--------+---------+  +--------+---------+  +--------+---------+ |
 *   |           |                     |                     |           |
 *   +-----------+---------------------+---------------------+-----------+
 *               |                     |                     |
 *               v                     v                     v
 *   +------------------------------------------------------------------+
 *   |           System WiFi Layer (this module) - YOU ARE HERE         |
 *   |  +------------------------------------------------------------+  |
 *   |  |  sys_wifi_*() APIs: Type conversion, validation, dispatch  |  |
 *   |  +------------------------------------------------------------+  |
 *   |                              |                                   |
 *   +------------------------------------------------------------------+
 *                                  | wpas_ctrl_*() / fhost_*()
 *               +------------------+------------------+
 *               |                                     |
 *   +-----------+-------------+       +---------------+---------------+
 *   | WPA Supplicant Layer    |       |       PS/Driver Layer         |
 *   | (ctrl_iface)            |       |       (ecw6280)       |
 *   | - Network management    |       | - Hardware abstraction        |
 *   | - Auth/encryption       |       | - MAC/PHY control             |
 *   +--------------------------+       +-------------------------------+
 *
 * KEY FEATURES
 * ============
 * - **Dual-Mode Support**: STA, AP, or concurrent STA+AP operation
 * - **NV Persistence**: Auto-save/restore credentials for fast reconnection
 * - **Type Conversion**: Bridges application types ↔ WPA supplicant types
 * - **Event System**: Async notifications for connection state changes
 * - **Fast Connect**: CONFIG_FAST_CONNECT optimization (3s → <500ms)
 * - **Channel Sync**: Automatic AP-follows-STA in concurrent mode
 *
 * FUNCTIONAL GROUPS
 * =================
 * 1. **Initialization**: sys_wifi_init(), mode management
 * 2. **STA Operations**: connect, disconnect, scan, get_info
 * 3. **AP Operations**: start, stop, get_sta_list, deauth_sta
 * 4. **NV Storage**: load/save credentials for persistence
 * 5. **Advanced Config**: country code, CSA, 802.11ax, WPS
 * 6. **Sniffer Mode**: Promiscuous packet capture (monitor mode)
 * 7. **MAC Management**: Read/write MAC addresses per VIF
 * 8. **Event Handling**: Subscribe to WiFi state change events
 *
 * USAGE GUIDELINES
 * ================
 * Initialization Sequence:
 *   1. sys_wifi_init()              // Load NV mode, register handlers
 *   2. sys_wifi_set_mode()          // Optional: override mode
 *   3. sys_wifi_sta_auto_connect_start() // Restore STA from NV
 *   4. sys_wifi_ap_auto_start()      // Restore AP from NV
 *
 * STA Connection:
 *   wifi_config_u config = {0};
 *   strcpy(config.sta.ssid, "MyAP");
 *   strcpy(config.sta.password, "password");
 *   sys_wifi_sta_start(&config);    // Async operation
 *   // Monitor events for connection result
 *
 * AP Creation:
 *   wifi_config_u config = {0};
 *   strcpy(config.ap.ssid, "MySoftAP");
 *   strcpy(config.ap.password, "12345678");
 *   config.ap.channel = 6;
 *   config.ap.authmode = AUTH_WPA2_PSK;
 *   sys_wifi_ap_start(&config);     // Channel may be overridden in concurrent mode
 *
 * IMPORTANT NOTES
 * ===============
 * - **Mode Switching**: sys_wifi_set_mode() stops interfaces excluded by the target mode, but never starts them
 * - **Concurrent Mode**: AP forced to STA's channel when STA connected
 * - **NV Policies**: auto_save_nv, ap_follow_sta control automatic behaviors
 * - **Async Operations**: Most connect/scan operations return immediately;
 *   use event callbacks for completion status
 *
 * THREAD SAFETY
 * =============
 * - sys_wifi_init()/sys_wifi_deinit(): Not thread-safe, call during initialization
 * - sys_wifi_channel_to_freq()/sys_wifi_freq_to_channel(): Thread-safe (pure computation)
 * - sys_wifi_if_get_mac()/sys_wifi_if_set_mac(): Thread-safe (internal synchronization)
 * - sys_wifi_sta_start()/sys_wifi_sta_stop(): Thread-safe (async via WPA supplicant)
 * - sys_wifi_ap_start()/sys_wifi_ap_stop(): Thread-safe (async via WPA supplicant)
 * - sys_wifi_scan_*(): Thread-safe (async operations with event callbacks)
 * - sys_wifi_nv_*(): Thread-safe (flash operations with internal mutex)
 * - sys_wifi_sniffer_*(): Not thread-safe, dedicated monitor mode
 * - Event callbacks: Executed in WiFi task context
 *
 * DEPENDENCIES
 * ============
 * - system_wifi_def.h: Basic type definitions (enums, constants)
 * - eswin_wifi_types.h: Unified type system across layers
 * - wpas_ctrl_api.h: WPA supplicant control interface
 * - fhost_cntrl.h: PS layer control functions
 *
 * PROTOCOL CONFIGURATION SYSTEM
 * =============================
 * ECW6630/ECW6700 support flexible WiFi protocol configuration with separate
 * 2.4G and 5G band settings for independent protocol and bandwidth control.
 *
 * Architecture:
 * - wifi_protocol_t: Structure with ghz_2g and ghz_5g fields (bitmap per band)
 * - wifi_bandwidth_t: Structure with ghz_2g and ghz_5g fields (enum per band)
 * - Band-aware connection: Uses corresponding band's configuration based on channel
 *
 * Capability Configuration APIs:
 * - sys_wifi_set_band_mode(): Configure 2.4G/5G/AUTO band operation
 * - sys_wifi_set_protocol(): Configure 802.11b/g/n/ac/ax independently per band
 * - sys_wifi_sta_set_bandwidth(): Configure 20/40/80MHz independently per band
 *
 * Protocol Bitmap Format (combinable via bitwise OR):
 * - WIFI_PROTOCOL_11B  = BIT(0)  // 2.4G only (DSSS/CCK)
 * - WIFI_PROTOCOL_11G  = BIT(1)  // Both bands (OFDM legacy)
 * - WIFI_PROTOCOL_11A  = WIFI_PROTOCOL_11G  // Alias for 5G OFDM
 * - WIFI_PROTOCOL_11N  = BIT(2)  // Both bands (HT)
 * - WIFI_PROTOCOL_11AC = BIT(3)  // 5G only (VHT)
 * - WIFI_PROTOCOL_11AX = BIT(4)  // Both bands (HE)
 *
 * Example Protocol Configurations:
 * - 2.4G: 11B+11G+11N+11AX (full support)
 * - 5G: 11A+11N+11AC+11AX (full support)
 * - 2.4G only 11B: WIFI_PROTOCOL_11B
 * - 5G only 11AC: WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC
 *
 * Validation Rules (enforced by sys_wifi_set_protocol):
 * - 5G band MUST NOT include 11B (DSSS/CCK not available on 5GHz)
 * - Protocol dependencies (11AX→11N→11G) are NOT auto-corrected by system
 * - User is responsible for providing valid protocol combinations
 * - Invalid configurations return SYS_ERR_INVALID_ARG
 *
 * Band Mode Behavior:
 * - WIFI_BAND_MODE_2G_ONLY: Only ghz_2g configuration active, ghz_5g cleared
 * - WIFI_BAND_MODE_5G_ONLY: Only ghz_5g configuration active, ghz_2g cleared
 * - WIFI_BAND_MODE_AUTO: Both ghz_2g and ghz_5g configurations active
 *
 ****************************************************************************/

#define CHANNEL_1_FREQ 2412
#define CHANNEL_14_FREQ 2484

#define CHANNEL_36_FREQ 5180
#define CHANNEL_165_FREQ 5825

#define CHANNEL_2G4_BANDWIDTH(channel) (channel - 1) * 5
#define CHANNEL_5G_BANDWIDTH(channel) (channel - 36) * 5

#define FREQ_TO_CHANNEL_2G4(freq) (freq - CHANNEL_1_FREQ) / 5
#define FREQ_TO_CHANNEL_5G(freq) (freq - CHANNEL_36_FREQ) / 5

#define BEACON_INT_MIN_MS 20
#define BEACON_INT_MAX_MS 1000

#if defined(CONFIG_ECW6700)
#define WIFI_SCAN_CHANNEL_MAX 42  /* 2.4G(14) + 5G(28) */
#else
#define WIFI_SCAN_CHANNEL_MAX 14  /* 2.4G(14)*/
#endif

/**
 * @brief WiFi AP information structure (scan result)
 */
typedef struct {
    unsigned char   ssid[WIFI_SSID_MAX_LEN];   /**< SSID */
    unsigned char   pwd[WIFI_PWD_MAX_LEN];     /**< Password (reserved) */
    unsigned char   ssid_len;                  /**< SSID length */
    unsigned char   pwd_len;                   /**< Password length (reserved) */
    unsigned char   auth;                      /**< Authentication mode */
    unsigned char   cipher;                    /**< Cipher type (pairwise) */
    unsigned char   group_cipher;              /**< Group cipher type */
    unsigned char   wifi_protocol;             /**< WiFi protocol bitmask (b/g/n/ax/a/ac) */
    unsigned char   wps;                       /**< WPS support flag (0/1) */
    unsigned char   channel;                   /**< WiFi channel */
    unsigned char   bssid[6];                  /**< BSSID (MAC address) */
    signed char     rssi;                      /**< Signal strength (dBm) */
} wifi_info_t;

/**
 * @brief SoftAP configuration structure
 */
typedef struct {
    unsigned char     ssid[WIFI_SSID_MAX_LEN];     /**< AP SSID */
    char              password[WIFI_PWD_MAX_LEN];  /**< AP passphrase or 64-character hexadecimal PSK */
    wifi_auth_mode_e  authmode;                    /**< Authentication mode */
    unsigned short    beacon_int;                  /**< Beacon interval (ms) */
    unsigned short    ap_inactive_time;            /**< AP client inactive timeout (seconds, 0=default 300s, range [10,3600]) */
    unsigned char     channel;                     /**< WiFi channel (ignored in concurrent mode if STA connected) */

    /**
     * @brief Maximum number of stations allowed to connect
     *
     * - 0: Auto (use hardware maximum, recommended)
     * - 1-N: Specific limit (N = hardware capability, obtainable via sys_wifi_ap_get_sta_max_capability())
     *
     * Note: Out-of-range values are auto-constrained to hardware maximum
     */
    unsigned char     max_connect;

    unsigned char     hidden_ssid;                 /**< Hide SSID (0=broadcast, 1=hidden) */
} wifi_ap_config_t;

/**
 * @brief STA configuration structure
 */
typedef enum {
    WIFI_STA_EAP_NONE = 0,                         /**< Non-Enterprise STA connection */
    WIFI_STA_EAP_TLS,                              /**< EAP-TLS */
    WIFI_STA_EAP_PEAP,                             /**< EAP-PEAP */
    WIFI_STA_EAP_TTLS,                             /**< EAP-TTLS */
    WIFI_STA_EAP_LEAP,                             /**< EAP-LEAP */
} wifi_sta_eap_method_e;

typedef struct {
    unsigned char     ssid[WIFI_SSID_MAX_LEN];     /**< SSID of target AP */
    char              password[WIFI_PWD_MAX_LEN];  /**< Passphrase or 64-character hexadecimal PSK of target AP */
    unsigned char     channel;                     /**< Preferred channel (0=auto) */
    unsigned char     candidate_channel;           /**< Optional second initial scan channel; 0=none, requires channel */
    unsigned char     bssid[6];                    /**< Target BSSID (optional, all zeros for any) */
    wifi_auth_mode_e  authmode;                    /**< Enterprise values ignored; Enterprise is auto-selected from scan */
    wifi_sta_eap_method_e eap_method;              /**< Explicit Enterprise EAP method, NONE for Open/PSK/WEP */
    const char       *identity;                    /**< Optional EAP identity for Enterprise auto-selection */
    size_t            identity_len;                /**< Length of identity, 0 means strlen(identity) for text input */
    const char       *eap_username;                /**< EAP username for PEAP/TTLS/LEAP, optional identity for TLS */
    size_t            eap_username_len;            /**< Length of EAP username, 0 means strlen(eap_username) */
    const char       *eap_password;                /**< EAP password for PEAP/TTLS/LEAP */
    size_t            eap_password_len;            /**< Length of EAP password, 0 means strlen(eap_password) */
} wifi_sta_config_t;

/**
 * @brief WiFi configuration union (AP or STA)
 */
typedef union {
    wifi_ap_config_t   ap;   /**< AP configuration */
    wifi_sta_config_t  sta;  /**< STA configuration */
} wifi_config_u;

typedef struct {
    unsigned char bssid[6];                    /**< Selected AP BSSID */
    unsigned char ssid[WIFI_SSID_MAX_LEN];     /**< Selected AP SSID */
    unsigned char ssid_len;                    /**< Selected AP SSID length */
    int freq;                                  /**< Selected AP frequency in MHz */
    int rssi;                                  /**< Selected AP RSSI from scan result */
} wifi_selected_bss_info_t;

typedef void (*wifi_selected_bss_cb_t)(const wifi_selected_bss_info_t *info,
                                       void *ctx);

/**
 * @brief NV storage Enterprise EAP extension
 */
typedef struct {
    wifi_sta_eap_method_e method;                  /**< Saved Enterprise EAP method */
    char           identity[WIFI_PWD_MAX_LEN];     /**< Saved EAP identity */
    char           username[WIFI_PWD_MAX_LEN];     /**< Saved EAP username */
    char           password[WIFI_PWD_MAX_LEN];     /**< Saved EAP password */
} wifi_sta_eap_nv_info_t;

/**
 * @brief NV storage information structure
 */
typedef struct {
    unsigned char  sync;                         /**< Sync flag */
    char           sta_ssid[WIFI_SSID_MAX_LEN];  /**< Saved STA SSID */
    char           sta_pwd[WIFI_PWD_MAX_LEN];    /**< Saved STA password */
    unsigned char  sta_bssid[6];                 /**< Saved STA BSSID */
    unsigned char  channel;                      /**< Saved channel */
    unsigned char  candidate_channel;            /**< Saved alternate-band channel; 0=none */
    unsigned char  auto_conn;                    /**< Auto-connect flag */
    wifi_sta_eap_nv_info_t eap;                  /**< Saved Enterprise EAP config */
} wifi_nv_info_t;

/**
 * @brief WiFi scan configuration structure
 */
typedef struct {
    unsigned char  *ssid;                                  /**< Target SSID (NULL for all) */
    unsigned char  *bssid;                                 /**< Target BSSID (NULL for all) */
    unsigned char  channel;                                /**< Specific channel (0 for all) */
    unsigned char  passive;                                /**< Passive scan mode */
    unsigned char  glance;                                 /**< Fast scan mode */
    unsigned char  max_item;                               /**< Maximum results */
    unsigned int   scan_time;                              /**< Scan time per channel (ms) */
    unsigned char  channel_list[WIFI_SCAN_CHANNEL_MAX];    /**< Channel list (supports 2.4G + 5G) */
    unsigned char  num_chan;                               /**< Number of channels in list */
} wifi_scan_config_t;

// Note: Scan synchronization is now managed internally via sys_wifi_context_t
// Use sys_wifi_wait_scan_done() and sys_wifi_event_scan_handler() APIs

/**
 * @brief SoftAP connected station information
 */
struct softap_sta_info
{
    unsigned char mac_addr[6];   /**< Station MAC address */
    unsigned short aid;          /**< Association ID */
};

/**
 * @brief Early WiFi initialization (two-phase init: phase 1)
 *
 * Performs minimal initialization safe for pre-scheduler context:
 * - Reads configuration from NV to memory cache
 * - Does NOT sync to driver (no mutex/semaphore/thread operations)
 * - Uses chip-specific defaults if NV unavailable
 *
 * Two-Phase Initialization:
 * - **Phase 1 (sys_wifi_init)**: Called before RTOS scheduler starts
 * - **Phase 2 (sys_wifi_init_late)**: Auto-triggered by WIFI_READY event after scheduler starts
 *
 * Initialization Sequence:
 * 1. Clear NV info cache (memset wifi_ctx->nv_info)
 * 2. Load operational mode from NV (CUSTOMER_NV_WIFI_OP_MODE)
 * 3. If NV read fails: Default to WIFI_MODE_AP_STA (concurrent mode)
 * 4. Register system reset handler
 * 5. Set late_init_done = false, waiting for phase 2
 *
 * @return Always returns SYS_OK (initialization cannot fail)
 *
 * @note Safe to call before RTOS scheduler starts
 * @note Driver sync happens later in sys_wifi_init_late()
 * @see sys_wifi_set_mode(), sys_wifi_get_mode()
 */
sys_err_t        sys_wifi_init(void);

/**
 * @brief Get current WiFi operational mode
 * @return Current WiFi mode (STA/AP/STA+AP)
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_set_mode()
 */
wifi_work_mode_e sys_wifi_get_mode(void);

/**
 * @brief Set WiFi operational mode
 * @param[in] opmode  WiFi mode:
 *                    - WIFI_MODE_STA (0): Station mode
 *                    - WIFI_MODE_AP (1): SoftAP mode
 *                    - WIFI_MODE_AP_STA (2): Station + SoftAP concurrent mode
 *
 * @par Behavior
 *      - Invalid mode values are silently ignored
 *      - If mode equals current mode, no action is taken
 *      - WIFI_MODE_STA stops SoftAP if running
 *      - WIFI_MODE_AP stops STA if running
 *      - WIFI_MODE_AP_STA does not start or stop either interface
 *      - If auto_save_nv policy is enabled, mode change is saved to NV
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @see sys_wifi_get_mode()
 */
void             sys_wifi_set_mode(wifi_work_mode_e opmode);

/**
 * @brief Check if WiFi is ready
 * @return true if WiFi initialization complete, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 */
bool             sys_wifi_is_ready(void);

/**
 * @brief Classify a channel number as 2.4GHz, 5GHz or unknown
 * @param[in] channel Channel number; 0 means unspecified
 * @return WIFI_BAND_2G for 1-14, WIFI_BAND_5G for 36-177, otherwise WIFI_BAND_UNKNOWN
 * @note Range classification only; does not validate channel spacing, country,
 *       hardware capability or configured band mode. Assumes 2.4G/5G channels.
 * @par Thread Safety
 *      Thread-safe (pure computation, no state access).
 */
wifi_band_e      sys_wifi_channel_to_band(unsigned int channel);

/**
 * @brief Convert WiFi channel number to frequency (MHz)
 * @param[in] channel  Channel number (1-14 for 2.4G, 36-165 for 5G)
 * @return Frequency in MHz, 0 if invalid channel
 *
 * @par Thread Safety
 *      Thread-safe (pure computation, no state access).
 *
 * @see sys_wifi_freq_to_channel()
 */
unsigned int     sys_wifi_channel_to_freq(unsigned int channel);

/**
 * @brief Convert frequency (MHz) to WiFi channel number
 * @param[in] freq  Frequency in MHz
 * @return Channel number, 0 if invalid frequency
 *
 * @par Thread Safety
 *      Thread-safe (pure computation, no state access).
 *
 * @see sys_wifi_channel_to_freq()
 */
unsigned int     sys_wifi_freq_to_channel(unsigned int freq);

/**
 * @brief Get NV info pointer (legacy compatibility)
 * @return Pointer to NV information structure
 * @deprecated Use context-based APIs instead
 *
 * @par Thread Safety
 *      Not thread-safe. Returns internal pointer, caller must not modify.
 */
wifi_nv_info_t*  sys_wifi_get_nv_info();

/**
 * @brief Get MAC address for specified virtual interface
 * @param[in] vif   Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[out] mac  Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_if_set_mac()
 */
sys_err_t   sys_wifi_if_get_mac(wifi_vif_e vif, unsigned char *mac);

/**
 * @brief Set MAC address for specified virtual interface
 *
 * Sets MAC for specified interface. The other interface's MAC is
 * automatically derived using HAL_SYSTEM_COMPUTE_SECONDARY_MAC.
 *
 * @param[in] vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[in] mac  New MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @see sys_wifi_if_get_mac(), sys_wifi_set_mac()
 */
sys_err_t   sys_wifi_if_set_mac(wifi_vif_e vif, unsigned char *mac);

/**
 * @brief Set MAC addresses for STA and/or SoftAP interfaces
 *
 * This function sets MAC addresses for both WiFi interfaces simultaneously.
 * It sends both addresses to PS layer in a single message, ensuring atomic
 * configuration of VIF MAC addresses.
 *
 * @param[in] sta_mac  STA interface MAC address (6 bytes), can be NULL
 * @param[in] ap_mac   AP interface MAC address (6 bytes), can be NULL
 * @return SYS_OK on success, error code otherwise
 *
 * @note At least one of sta_mac or ap_mac must be non-NULL
 * @note If only one MAC provided, the other is derived using HAL_SYSTEM_COMPUTE_SECONDARY_MAC
 * @note Message format to PS layer: [6 bytes STA MAC] + [6 bytes AP MAC]
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @see sys_wifi_if_set_mac(), sys_wifi_sta_set_mac(), sys_wifi_ap_set_mac()
 */
sys_err_t   sys_wifi_set_mac(const uint8_t *sta_mac, const uint8_t *ap_mac);

/**
 * @brief Set monitor channel
 * @param[in] channel  WiFi channel number
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state.
 *
 * @see sys_wifi_get_current_channel()
 */
sys_err_t      sys_wifi_sniffer_set_channel(unsigned char channel);

/**
 * @brief Get current WiFi channel
 * @return Current channel number
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_sniffer_set_channel()
 */
unsigned char  sys_wifi_get_current_channel(void);

/**
 * @brief Start WiFi sniffer/monitor mode
 *
 * Enables promiscuous mode to capture 802.11 frames on the current channel.
 * Captured frames are delivered via the registered callback function.
 *
 * @param[in] cb      Callback function for received packets (wifi_sniffer_cb_t)
 *                    Callback signature: void (*cb)(void *buf, int len, wifi_sniffer_pkt_type_t type)
 *                    - buf: Pointer to wifi_sniffer_packet_t (contains rx_info and payload)
 *                    - len: Total frame length in bytes
 *                    - type: Frame type (WIFI_PKT_MGMT/WIFI_PKT_CTRL/WIFI_PKT_DATA)
 * @param[in] filter  Packet filter configuration (NULL to capture all frames)
 *                    Use wifi_sniffer_filter_t.filter_mask with WIFI_SNIFFER_FILTER_* flags:
 *                    - WIFI_SNIFFER_FILTER_MGMT (0x01): Management frames (beacon, probe, auth)
 *                    - WIFI_SNIFFER_FILTER_CTRL (0x02): Control frames (RTS, CTS, ACK)
 *                    - WIFI_SNIFFER_FILTER_DATA (0x04): Data frames
 *                    - WIFI_SNIFFER_FILTER_ALL (0xFFFFFFFF): All frame types
 *                    Multiple flags can be OR'ed together.
 * @return SYS_OK on success, SYS_ERR_WIFI_IF_DOWN if WiFi interface is down
 *
 * @pre WiFi interface must be up (not RF-killed)
 * @pre Call sys_wifi_sniffer_set_channel() first to set monitoring channel
 *
 * @note Sniffer mode can run concurrently with STA/AP mode, but may impact
 *       normal WiFi performance due to channel switching and processing overhead.
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state.
 *
 * @see sys_wifi_sniffer_stop(), sys_wifi_sniffer_set_filter(), sys_wifi_sniffer_set_channel()
 * @see wifi_sniffer_filter_t, wifi_sniffer_packet_t, wifi_sniffer_pkt_type_t (eswin_wifi_types.h)
 */
sys_err_t      sys_wifi_sniffer_start(wifi_sniffer_cb_t cb, wifi_sniffer_filter_t *filter);

/**
 * @brief Stop WiFi sniffer/monitor mode
 *
 * Disables promiscuous mode and cleans up sniffer state:
 * - Stops hardware/firmware monitor mode
 * - Unregisters frame processing callback from PS layer
 * - Clears user callback
 * - Resets filter to default (WIFI_SNIFFER_FILTER_ALL)
 *
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state.
 *
 * @see sys_wifi_sniffer_start()
 */
sys_err_t      sys_wifi_sniffer_stop(void);

/**
 * @brief Set packet filter for sniffer mode
 *
 * Updates the frame type filter configuration. If sniffer mode is already
 * enabled, the new filter is applied immediately.
 *
 * @param[in] filter  Filter configuration containing filter_mask field
 *                    Available filter flags (can be OR'ed together):
 *                    - WIFI_SNIFFER_FILTER_MGMT (0x01): Management frames
 *                    - WIFI_SNIFFER_FILTER_CTRL (0x02): Control frames
 *                    - WIFI_SNIFFER_FILTER_DATA (0x04): Data frames
 *                    - WIFI_SNIFFER_FILTER_DATA_MPDU (0x08): Reserved, not implemented
 *                    - WIFI_SNIFFER_FILTER_DATA_AMPDU (0x10): Reserved, not implemented
 *                    - WIFI_SNIFFER_FILTER_OTHER_BSSID (0x20): Frames from other BSSIDs
 *                    - WIFI_SNIFFER_FILTER_ALL (0xFFFFFFFF): All frames
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if filter is NULL
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies sniffer configuration.
 *
 * @see sys_wifi_sniffer_get_filter(), sys_wifi_sniffer_start()
 */
sys_err_t      sys_wifi_sniffer_set_filter(const wifi_sniffer_filter_t *filter);

/**
 * @brief Get current packet filter configuration
 *
 * Retrieves the currently configured packet filter settings.
 *
 * @param[out] filter  Output buffer for filter configuration
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if filter is NULL
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sniffer_set_filter()
 */
sys_err_t      sys_wifi_sniffer_get_filter(wifi_sniffer_filter_t *filter);

/**
 * @brief Send raw 802.11 frame
 * @param[in] vif    Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[in] frame  Raw frame data (including 802.11 header)
 * @param[in] len    Frame length in bytes
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Direct driver transmission.
 */
sys_err_t      sys_wifi_send_raw_pkt(wifi_vif_e vif,const unsigned char *frame, const unsigned short len);

/**
 * @brief Start SoftAP with mode-dependent channel handling
 *
 * Mode-Specific Behaviors:
 *
 * **AP-Only Mode (WIFI_MODE_AP):**
 * - Uses config->ap.channel as-is
 *
 * **Concurrent Mode (WIFI_MODE_AP_STA):**
 * - If STA is connected, IGNORES config->ap.channel
 * - Forces AP to use STA's current channel (regulatory requirement)
 * - If STA not connected, uses config->ap.channel
 *
 * Implementation Flow:
 * 1. Validate mode (must be AP or AP_STA)
 * 2. Determine actual channel based on mode
 * 3. Convert config → wpa_network_config_t
 * 4. Call wpas_ctrl_network_start()
 *
 * Password Validation:
 * - OPEN: empty password
 * - WEP: 5-byte or 13-byte ASCII key
 * - WPA/WPA2: 8-63 byte passphrase, or 64 hexadecimal characters as a raw PSK
 * - A 64-character raw PSK is not a WPA3-SAE passphrase
 *
 * @param config  AP configuration (use config->ap union member)
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if config or the AP password is invalid
 * @retval SYS_ERR_WIFI_INVALID_MODE if mode is WIFI_MODE_STA
 * @retval SYS_ERR if WPA supplicant start fails
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_AP_START on success
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @note Concurrent mode overrides channel - config->ap.channel may be ignored
 * @note This function does not start or stop the STA interface
 * @see sys_wifi_ap_stop(), sys_wifi_set_mode()
 */
sys_err_t      sys_wifi_ap_start(wifi_config_u *config);

/**
 * @brief Stop SoftAP
 * @return SYS_OK on success, error code otherwise
 * @note If SoftAP is already stopped, returns SYS_OK without reporting another stop event
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_AP_STOP on success
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @see sys_wifi_ap_start(), sys_wifi_ap_is_started()
 */
sys_err_t      sys_wifi_ap_stop(void);

/**
 * @brief Start SoftAP using configuration from NV storage
 * @param nv_info  AP configuration (NULL to load from NV)
 * @return SYS_OK on success, SYS_ERR on failure
 * @see sys_wifi_ap_load_config(), sys_wifi_ap_start()
 */
sys_err_t      sys_wifi_ap_start_by_nv(wifi_ap_config_t *nv_info);

/**
 * @brief Get current AP configuration
 * @param[out] softap_info  Output buffer for AP configuration
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if softap_info is NULL
 * @retval SYS_ERR_WIFI_AP_NOT_STARTED if AP is not started
 *
 * @pre AP must be started (sys_wifi_ap_is_started() returns true)
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_get_ssid(), sys_wifi_ap_is_started()
 */
sys_err_t      sys_wifi_ap_get_info(wifi_ap_config_t *softap_info);

/**
 * @brief Get AP SSID
 * @param[out] ssid  Output buffer for SSID (at least WIFI_SSID_MAX_LEN bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_get_info()
 */
sys_err_t      sys_wifi_ap_get_ssid(char *ssid);

/**
 * @brief Get list of connected stations
 * @param[out] info     Output buffer for station info array
 * @param[in,out] num   Input: buffer size, Output: actual number of stations
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_ap_deauth_sta(), sys_wifi_ap_get_sta_max()
 */
sys_err_t      sys_wifi_ap_get_sta_list(struct softap_sta_info *info, unsigned int *num);

/**
 * @brief Deauthenticate a connected station
 * @param[in] sta_mac  Station MAC address (6 bytes)
 * @param[in] reason   Deauth reason code (IEEE 802.11)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Driver operation.
 *
 * @see sys_wifi_ap_get_sta_list()
 */
sys_err_t      sys_wifi_ap_deauth_sta(const unsigned char *sta_mac, unsigned short reason);

/**
 * @brief Get beacon interval
 * @return Beacon interval in milliseconds
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 */
unsigned int   sys_wifi_ap_get_beacon_interval(void);

/**
 * @brief Set beacon vendor specific IE (runtime)
 * @param[in] data  Vendor IE payload (may be NULL to clear)
 * @param[in] len   Vendor IE payload length
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies AP beacon and triggers beacon update.
 */
sys_err_t      sys_wifi_ap_set_vendor_elements(const unsigned char *data, unsigned int len);

/**
 * @brief Set beacon interval (runtime)
 * @param[in] interval  Beacon interval in milliseconds
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies AP beacon and triggers beacon update.
 */
sys_err_t      sys_wifi_ap_set_beacon_interval(unsigned int interval);

/**
 * @brief Set maximum number of stations
 * @param[in] max_sta  Maximum STA count (0=auto, 1-N where N is hardware capability)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies AP configuration.
 *
 * @see sys_wifi_ap_get_sta_max(), sys_wifi_ap_get_sta_max_capability()
 */
sys_err_t      sys_wifi_ap_set_sta_max(unsigned short max_sta);

/**
 * @brief Get current configured maximum STA count
 * @param[out] max_sta  Pointer to receive configured value
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_set_sta_max(), sys_wifi_ap_get_sta_max_capability()
 */
sys_err_t      sys_wifi_ap_get_sta_max(unsigned short *max_sta);

/**
 * @brief Get hardware capability maximum STA count
 * @return Hardware maximum
 *
 * @par Thread Safety
 *      Thread-safe (constant value).
 *
 * @see sys_wifi_ap_set_sta_max(), sys_wifi_ap_get_sta_max()
 */
unsigned char  sys_wifi_ap_get_sta_max_capability(void);

/**
 * @brief Set AP MAC address
 *
 * Sets AP interface MAC. STA MAC is automatically derived.
 *
 * @param[in] mac  New MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @see sys_wifi_ap_get_mac(), sys_wifi_set_mac()
 */
sys_err_t      sys_wifi_ap_set_mac(unsigned char *mac);

/**
 * @brief Get AP MAC address
 * @param[out] mac  Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_set_mac()
 */
sys_err_t      sys_wifi_ap_get_mac(unsigned char *mac);

/**
 * @brief Get AP status
 * @return AP status (AP_STATUS_STOPPED/AP_STATUS_STARTED)
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_is_started()
 */
wifi_status_e  sys_wifi_ap_get_status(void);

/**
 * @brief Check if AP is started
 * @return true if AP is running, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_ap_get_status(), sys_wifi_ap_start(), sys_wifi_ap_stop()
 */
bool           sys_wifi_ap_is_started(void);

/**
 * @brief Restore and start SoftAP from NV-stored configuration
 *
 * This function loads previously saved AP configuration from NV storage
 * and starts the SoftAP with those settings.
 *
 * Implementation Flow:
 * 1. Call sys_wifi_ap_load_config() to read NV credentials
 * 2. If load succeeds, call sys_wifi_ap_start() to activate AP
 * 3. Return error if NV load fails (incomplete credentials)
 *
 * NV Parameter Requirements:
 * - **Required** (all must exist): SSID, password, channel, authmode
 * - **Optional**: max_connect, hidden_ssid
 *
 * Failure Conditions:
 * - Returns SYS_ERR if any required parameter missing from NV
 * - Does not create AP with default values - strictly uses stored config
 *
 * Typical Usage:
 *   Called during system initialization to restore AP state from
 *   previous session (e.g., after reboot/power cycle).
 *
 * @return SYS_OK if AP started successfully
 * @retval SYS_ERR if NV load fails or AP start fails
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_AP_START on success
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Call only during initialization sequence.
 *
 * @note Unlike sys_wifi_sta_auto_connect_start(), this function returns
 *       error on missing NV data (not silent success)
 * @see sys_wifi_ap_load_config(), sys_wifi_ap_start(), sys_wifi_ap_save_config()
 */
sys_err_t      sys_wifi_ap_auto_start(void);

/**
 * @brief Trigger AP channel switch announcement (CSA)
 * @param[in] channel    Target channel number
 * @param[in] csa_count  CSA countdown (number of beacons before switch)
 * @param[in] blocktx    Block transmission during switch (0=no, 1=yes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Driver operation.
 *
 * @note Implements IEEE 802.11h channel switch mechanism
 */
sys_err_t      sys_wifi_ap_switch_channel(unsigned short channel, unsigned char csa_count, unsigned char blocktx);

/**
 * @brief Enable/disable AP auto-follow STA channel policy
 * @param[in] enable  true to enable, false to disable
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies policy configuration.
 *
 * @note When enabled, AP auto-switches to STA's channel on CSA events
 *
 * @see sys_wifi_ap_get_follow_sta(), sys_wifi_ap_sync_with_sta_channel()
 */
void           sys_wifi_ap_set_follow_sta(bool enable);

/**
 * @brief Get AP follow STA channel policy status
 * @return true if enabled, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to policy configuration).
 *
 * @see sys_wifi_ap_set_follow_sta()
 */
bool           sys_wifi_ap_get_follow_sta(void);

/**
 * @brief Synchronize AP channel with STA channel
 *
 * @par Thread Safety
 *      Not thread-safe. Driver operation.
 *
 * @note Called automatically when STA channel changes (if policy enabled)
 *
 * @see sys_wifi_ap_set_follow_sta(), sys_wifi_ap_switch_channel()
 */
void           sys_wifi_ap_sync_with_sta_channel(void);

/**
 * @brief Load AP configuration from NV storage
 * @param[out] softap_info  Output buffer for AP configuration
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. NV read operation.
 *
 * @see sys_wifi_ap_save_config(), sys_wifi_ap_auto_start()
 */
sys_err_t      sys_wifi_ap_load_config(wifi_ap_config_t *softap_info);

/**
 * @brief Save AP configuration to NV storage
 * @note Callers must validate field values before saving.
 *       hidden_ssid and ap_inactive_time are saved even when zero.
 *       Zero max_connect or beacon_int leaves the corresponding NV value unchanged.
 * @param[in] softap_info  AP configuration to save
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. NV write operation.
 *
 * @see sys_wifi_ap_load_config()
 */
sys_err_t      sys_wifi_ap_save_config(wifi_ap_config_t *softap_info);

/**
 * @brief Initiate STA connection
 *
 * Mode-Specific Behaviors:
 *
 * **STA-Only Mode (WIFI_MODE_STA):**
 * - Does not start or stop the AP interface
 *
 * **Concurrent Mode (WIFI_MODE_AP_STA):**
 * - Leaves AP running (no cleanup)
 * - AP may switch channel if STA connects to different channel
 *
 * Validation:
 * - Rejects if config is NULL
 * - Rejects if SSID is empty
 * - Rejects if password length or format is invalid:
 *   - Valid: 0 (OPEN), 5 (WEP40-ASCII), 8-63 (WPA passphrase)
 *   - 64 bytes are valid only as a hexadecimal WPA/WPA2 raw PSK
 * - Rejects if current mode is not STA or AP_STA
 * - Rejects if STA status is not STA_STATUS_STOP or STA_STATUS_DISCON
 * - Rejects if channel incompatible with band mode
 *
 * Connection Flow:
 * 1. Parameter validation (config, SSID, password)
 * 2. Mode and status validation
 * 3. If STA_STATUS_DISCON and config matches current network:
 *    fast reconnect via BSS cache (skip scan), return immediately
 * 4. If STA_STATUS_DISCON and config differs: stop old network, fall through
 * 5. Convert config->sta → wpa_network_config_t
 * 6. Call wpas_ctrl_network_start() (async operation)
 * 7. Set status to STA_STATUS_START
 * 8. Return immediately (connection completes asynchronously)
 *
 * @param config  STA configuration (use config->sta union member)
 * @return SYS_OK if connection initiated successfully
 * @retval SYS_ERR_INVALID_ARG if config is NULL or channel incompatible with band mode
 * @retval SYS_ERR_WIFI_NO_SSID if SSID is empty
 * @retval SYS_ERR_WIFI_INVALID_PASSWORD if password length or format is invalid
 * @retval SYS_ERR_WIFI_INVALID_MODE if mode is WIFI_MODE_AP
 * @retval SYS_ERR_WIFI_BUSY if STA is in a state other than STOP or DISCON
 * @retval SYS_ERR_WIFI_FAIL if WPA subsystem error (check logs for details)
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion Events:
 *        - SYSTEM_EVENT_STA_CONNECTED on success
 *        - SYSTEM_EVENT_STA_DISCONNECTED on failure
 *        - SYSTEM_EVENT_STA_4WAY_HS_FAIL on key mismatch
 *      - Callback Context: Event loop task
 *
 * @note Enterprise authentication is selected from scan results automatically.
 *       Explicit STA authmode Enterprise values are ignored. Selection priority:
 *       WPA3-Enterprise, WPA2-Enterprise, WPA3-Enterprise 192-bit. If no
 *       Enterprise BSS matches, the existing open/PSK fallback path is used.
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @note Mode transition cleanup is handled by sys_wifi_set_mode()
 * @see sys_wifi_sta_stop(), sys_wifi_set_mode()
 */
sys_err_t      sys_wifi_sta_start(wifi_config_u *config);

/**
 * @brief Register selected-BSS callback for STA connection.
 *
 * The callback is invoked after WPA selects a scan result and before the
 * association request is issued. The registration remains active until
 * sys_wifi_sta_unregister_selected_bss_callback() is called.
 *
 * @param[in] cb   Callback function
 * @param[in] ctx  User context passed back to callback
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t      sys_wifi_sta_register_selected_bss_callback(wifi_selected_bss_cb_t cb,
                                                           void *ctx);

/**
 * @brief Unregister selected-BSS callback for STA connection.
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t      sys_wifi_sta_unregister_selected_bss_callback(void);

/**
 * @brief Stop STA and disconnect from AP
 * @return SYS_OK on success, error code otherwise
 * @note If STA is already stopped, returns SYS_OK without reporting another stop event
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_STA_STOP / SYSTEM_EVENT_STA_DISCONNECTED
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @see sys_wifi_sta_start(), sys_wifi_sta_is_connected()
 */
sys_err_t      sys_wifi_sta_stop(void);

/**
 * @brief Disconnect STA from the current AP while keeping STA enabled
 * @return SYS_OK on success, error code otherwise
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_STA_DISCONNECTED
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @note Unlike sys_wifi_sta_stop(), this keeps the STA interface in a
 *       disconnected state instead of transitioning it to STOP.
 *
 * @see sys_wifi_sta_stop(), sys_wifi_sta_reconnect(), sys_wifi_sta_is_connected()
 */
sys_err_t      sys_wifi_sta_disconnect(void);

/**
 * @brief Reconnect STA to the previously configured network
 * @return SYS_OK on reconnect initiated, error code otherwise
 * @retval SYS_ERR_WIFI_INVALID_MODE if mode is not STA or AP_STA
 * @retval SYS_ERR_INVALID_STATE if STA is not in STA_STATUS_DISCON state
 * @retval SYS_ERR_WIFI_FAIL if no network configured or WPA reconnect failed
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_STA_CONNECTED / SYSTEM_EVENT_STA_DISCONNECTED
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @note Only works after sys_wifi_sta_disconnect() (STA_STATUS_DISCON state).
 *       Uses BSS cache for fast reconnection when available; falls back to
 *       scan automatically if cache expired.
 *
 * @see sys_wifi_sta_disconnect(), sys_wifi_sta_start()
 */
sys_err_t      sys_wifi_sta_reconnect(void);

/**
 * @brief Roam STA to a target BSSID in the current ESS
 * @param[in] bssid  Target BSSID (6 bytes)
 * @return SYS_OK on roam initiated, error code otherwise
 * @retval SYS_ERR_WIFI_INVALID_MODE if mode is not STA or AP_STA
 * @retval SYS_ERR_WIFI_NOT_CONNECTED if STA is not connected
 * @retval SYS_ERR_INVALID_ARG if bssid is NULL or invalid
 * @retval SYS_ERR_NOT_FOUND if target BSSID is not in BSS cache
 *
 * @par Async Behavior
 *      - Operation: Asynchronous
 *      - Completion: SYSTEM_EVENT_STA_CONNECTED / SYSTEM_EVENT_STA_DISCONNECTED
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Caller must ensure no concurrent WiFi operations.
 *
 * @note The target BSSID must already exist in the WPA BSS table. Run a scan
 *       first if the target AP has not been discovered.
 */
sys_err_t      sys_wifi_sta_roam(const unsigned char *bssid);

/**
 * @brief Get STA connection status
 * @return STA status (STA_STATUS_DISCONNECTED/STA_STATUS_CONNECTING/STA_STATUS_CONNECTED)
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_sta_is_connected()
 */
wifi_status_e  sys_wifi_sta_get_status(void);

/**
 * @brief Check if STA is connected to AP
 * @return true if connected, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_sta_get_status(), sys_wifi_sta_start(), sys_wifi_sta_stop()
 */
bool           sys_wifi_sta_is_connected(void);

/**
 * @brief Get connected AP information
 * @param[out] info  Output buffer for AP information (SSID, BSSID, RSSI, etc.)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_sta_get_ssid(), sys_wifi_sta_get_bssid(), sys_wifi_sta_get_rssi()
 */
sys_err_t      sys_wifi_sta_get_info(wifi_info_t *info);

/**
 * @brief Get connected AP SSID
 * @param[out] ssid  Output buffer for SSID (at least WIFI_SSID_MAX_LEN bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_sta_get_info()
 */
sys_err_t      sys_wifi_sta_get_ssid(char *ssid);

/**
 * @brief Get connected AP BSSID (MAC address)
 * @param[out] bssid  Output buffer for BSSID (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_sta_get_info()
 */
sys_err_t      sys_wifi_sta_get_bssid(unsigned char *bssid);

/**
 * @brief Get RSSI of the AP associated with STA
 * @param[out] rssi  Output buffer for associated AP RSSI value (dBm)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_sta_get_info()
 */
sys_err_t      sys_wifi_sta_get_rssi(signed char *rssi);

/**
 * @brief Send 802.11k RRM Neighbor Report Request from STA
 * @param[in] vif_id    Virtual interface ID (must be WIFI_VIF_STA)
 * @param[in] ssid      Optional SSID filter, NULL to use current ESS
 * @param[in] ssid_len  SSID length when ssid is not NULL
 * @return SYS_OK on request sent, error code otherwise
 *
 * @par Async Behavior
 *      Neighbor Report Response is received asynchronously through WPA action
 *      frame handling and logged by the WPA control layer.
 *
 * @par Thread Safety
 *      Not thread-safe. Sends a management action frame.
 */
sys_err_t sys_wifi_sta_rrm_neighbor_req(wifi_vif_e vif_id,
                                        const unsigned char *ssid,
                                        unsigned int ssid_len);

/**
 * @brief Send 802.11v WNM BSS Transition Management Query from STA
 * @param[in] vif_id     Virtual interface ID (must be WIFI_VIF_STA)
 * @param[in] reason     Query reason code
 * @param[in] cand_list  Non-zero to request candidate list
 * @return SYS_OK on request sent, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Sends a management action frame.
 */
sys_err_t sys_wifi_sta_wnm_bss_query(wifi_vif_e vif_id,
                                     unsigned char reason,
                                     int cand_list);

/**
 * @brief Set PCI mode for station (AT command specific)
 * @param[in] vif_id    Virtual interface ID (should be WIFI_VIF_STA)
 * @param[in] pci_mode  PCI mode (WIFI_PCI_DISABLE or WIFI_PCI_ENABLE)
 * @return sys_err_t SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @note This function is primarily used by AT commands.
 *       PCI mode controls whether to reject insecure networks (OPEN/WEP).
 *       - WIFI_PCI_DISABLE (0): Allow any encryption type
 *       - WIFI_PCI_ENABLE (1): Reject OPEN/WEP networks
 *
 * @see sys_wifi_sta_get_pci_mode()
 */
sys_err_t sys_wifi_sta_set_pci_mode(wifi_vif_e vif_id, wifi_pci_mode_e pci_mode);

/**
 * @brief Get PCI mode for station (AT command specific)
 * @param[in] vif_id      Virtual interface ID (should be WIFI_VIF_STA)
 * @param[out] pci_mode   Output: current PCI mode
 * @return sys_err_t SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sta_set_pci_mode()
 */
sys_err_t sys_wifi_sta_get_pci_mode(wifi_vif_e vif_id, wifi_pci_mode_e *pci_mode);

/**
 * @brief Set the weakest authentication mode accepted by STA
 * @param[in] vif_id              Virtual interface ID (must be WIFI_VIF_STA)
 * @param[in] authmode_threshold  Weakest accepted authentication mode
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if vif_id or authmode_threshold is invalid
 * @retval SYS_ERR_INVALID_STATE if the WPA interface is not initialized
 *
 * Authentication strength follows the Wi-Fi security ordering rather than
 * enum numeric ordering. Personal and Enterprise authentication modes form
 * separate strength families; use a threshold from the network's family.
 * AUTH_WPA_WPA2_PSK is equivalent to AUTH_WPA2_PSK, and
 * AUTH_WPA2_WPA3_PSK is equivalent to AUTH_WPA3_SAE.
 * AUTH_WPA_WPA2_ENTERPRISE_TLS is treated as the WPA-Enterprise floor.
 *
 * The default threshold is AUTH_OPEN. A new value applies to subsequent BSS
 * selection and reconnection attempts; it does not disconnect a current link.
 * Configuration persistence, when required, is owned by the CLI/AT caller.
 *
 * Existing PCI mode is evaluated independently, so the effective policy is
 * the combination of both settings and the stricter restriction wins.
 *
 * @note authmode_threshold uses wifi_auth_mode_e values. A CLI/AT protocol
 *       encoding must be mapped explicitly instead of casting its numeric value.
 * @note Enterprise thresholds require
 *       CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT for Enterprise connections.
 * @note This blocking API must not be called from an ISR or WPA event-loop
 *       callback.
 *
 * @par Thread Safety
 *      Thread-safe. The update is serialized through the WPA event loop.
 */
sys_err_t sys_wifi_sta_set_authmode_threshold(
    wifi_vif_e vif_id, wifi_auth_mode_e authmode_threshold);

/**
 * @brief Set PMF (Protected Management Frames) mode for STA (AT command internal use)
 * @param[in] vif_id    Virtual interface ID (should be WIFI_VIF_STA)
 * @param[in] pmf_mode  PMF mode: WIFI_PMF_DISABLED(0), WIFI_PMF_OPTIONAL(1), WIFI_PMF_REQUIRED(2)
 * @return sys_err_t SYS_OK on success, error code otherwise
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @note This function sets the global PMF policy for the STA interface.
 *       The actual PMF negotiation with AP is adaptive based on AP capabilities.
 *       - WIFI_PMF_DISABLED (0): Disable PMF, allow connections to all APs
 *       - WIFI_PMF_OPTIONAL (1): Enable PMF capability, prefer PMF but allow non-PMF APs
 *       - WIFI_PMF_REQUIRED (2): Require PMF, reject connections to non-PMF APs
 *
 * @see sys_wifi_sta_get_pmf()
 */
sys_err_t sys_wifi_sta_set_pmf(wifi_vif_e vif_id, wifi_pmf_mode_e pmf_mode);

/**
 * @brief Get current PMF mode for STA (AT command internal use)
 * @param[in] vif_id      Virtual interface ID (should be WIFI_VIF_STA)
 * @param[out] pmf_mode   Output: current PMF mode
 * @return sys_err_t SYS_OK on success, error code otherwise
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sta_set_pmf()
 */
sys_err_t sys_wifi_sta_get_pmf(wifi_vif_e vif_id, wifi_pmf_mode_e *pmf_mode);

/**
 * @brief Set STA reconnection policy (AT command internal use)
 *
 * Configures reconnection behavior when STA loses connection during runtime.
 * Can be called before sys_wifi_sta_start() to affect the upcoming connection,
 * or during an active connection to modify reconnection behavior.
 *
 * @param[in] vif_id        Virtual interface ID (should be WIFI_VIF_STA)
 * @param[in] interval_sec  Reconnection interval in seconds (0 = use default)
 * @param[in] repeat_count  Maximum reconnection attempts (0 = unlimited)
 * @return SYS_OK on success, error code otherwise
 *
 * @note Default behavior: STA automatically reconnects indefinitely after
 *       disconnection. This function is for AT command specific control only.
 * @note Calling timing: Can be called before sys_wifi_sta_start() to configure
 *       reconnection policy for the upcoming connection session.
 * @note This is for AT command internal use. General applications should
 *       rely on the default unlimited reconnection behavior.
 *
 * @par Parameters Detail
 *      - interval_sec: Time between reconnection attempts
 *        - 0 with repeat_count=0: Disable automatic reconnect
 *        - 0 with repeat_count>0: Use system default delay
 *        - >0: Use specified interval in seconds
 *      - repeat_count: Maximum reconnection attempts
 *        - 0 with interval_sec>0: Unlimited reconnection
 *        - 0 with interval_sec=0: Disable automatic reconnect
 *        - >0: Stop after this many attempts
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies connection policy.
 *
 * @see sys_wifi_sta_get_connect_policy()
 */
sys_err_t sys_wifi_sta_set_connect_policy(wifi_vif_e vif_id, int interval_sec, int repeat_count);

/**
 * @brief Get current STA reconnection policy (AT command internal use)
 *
 * Retrieves the current reconnection policy configuration.
 *
 * @param[in] vif_id         Virtual interface ID (should be WIFI_VIF_STA)
 * @param[out] interval_sec  Output: reconnection interval in seconds
 * @param[out] repeat_count  Output: maximum reconnection attempts (0 = unlimited)
 * @return SYS_OK on success, error code otherwise
 *
 * @note This is for AT command internal use. General applications should
 *       rely on the default unlimited reconnection behavior.
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sta_set_connect_policy()
 */
sys_err_t sys_wifi_sta_get_connect_policy(wifi_vif_e vif_id, int *interval_sec, int *repeat_count);

/**
 * @brief Start STA auto-connect from NV configuration
 *
 * Attempts to connect to the AP saved in NV storage if auto-connect
 * is enabled. Typically called during system boot.
 *
 * Conditions for connection:
 * - auto_conn flag is enabled (default: enabled, configurable via sys_wifi_sta_set_auto_connect)
 * - Valid SSID exists in NV storage
 *
 * @return SYS_OK on success, SYS_ERR if auto-connect disabled or no saved AP
 *
 * @par Async Behavior
 *      - Operation: Asynchronous (if connection initiated)
 *      - Completion: SYSTEM_EVENT_STA_CONNECTED / SYSTEM_EVENT_STA_DISCONNECTED
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Call only during initialization sequence.
 *
 * @note This function is typically called by system initialization.
 *       Use sys_wifi_sta_set_auto_connect() to enable/disable this behavior.
 *
 * @see sys_wifi_sta_set_auto_connect(), sys_wifi_sta_get_auto_connect()
 */
sys_err_t      sys_wifi_sta_auto_connect_start(void);

/**
 * @brief Enable/disable STA auto-connect on power-up
 *
 * Configures whether the device automatically connects to the last
 * successfully connected AP when powered on. This setting is saved
 * to NV storage and persists across reboots.
 *
 * @param[in] enable  true: Enable auto-connect on boot (default)
 *                    false: Disable auto-connect on boot
 * @return SYS_OK on success, error code otherwise
 *
 * @note Default behavior: Auto-connect is ENABLED by default.
 * @note NV write optimization: Setting is only written to NV if the value
 *       actually changes, to reduce flash wear.
 * @note Runtime reconnection: After disconnection during runtime, STA will
 *       automatically reconnect indefinitely by default. Use
 *       sys_wifi_sta_set_connect_policy() for AT command specific control.
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies NV storage.
 *
 * @see sys_wifi_sta_get_auto_connect(), sys_wifi_sta_auto_connect_start()
 */
sys_err_t      sys_wifi_sta_set_auto_connect(bool enable);

/**
 * @brief Get STA auto-connect on power-up setting
 *
 * Retrieves the current auto-connect configuration.
 *
 * @param[out] enable  Output: true if auto-connect enabled (default), false otherwise
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if enable is NULL
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sta_set_auto_connect()
 */
sys_err_t      sys_wifi_sta_get_auto_connect(bool *enable);

/**
 * @brief Set beacon loss detection threshold
 * @param[in] time_ms  Timeout in milliseconds (0 to disable)
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver configuration.
 *
 * @see sys_wifi_sta_get_beacon_loss_thd()
 */
void           sys_wifi_sta_set_beacon_loss_thd(unsigned int time_ms);

/**
 * @brief Get beacon loss detection threshold
 * @return Timeout in milliseconds
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to configuration).
 *
 * @see sys_wifi_sta_set_beacon_loss_thd()
 */
unsigned int   sys_wifi_sta_get_beacon_loss_thd(void);

/**
 * @brief Set STA MAC address
 *
 * Sets STA interface MAC. AP MAC is automatically derived.
 *
 * @param[in] mac  New MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies interface configuration.
 *
 * @see sys_wifi_sta_get_mac(), sys_wifi_set_mac()
 */
sys_err_t      sys_wifi_sta_set_mac(unsigned char *mac);

/**
 * @brief Get STA MAC address
 * @param[out] mac  Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_sta_set_mac()
 */
sys_err_t      sys_wifi_sta_get_mac(unsigned char *mac);

/**
 * @brief Start WiFi scan
 * @param[in] block   true to block until scan complete, false for async scan
 * @param[in] config  Scan configuration (NULL for default full-channel scan)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Async Behavior
 *      - Operation: Synchronous if block=true, Asynchronous if block=false
 *      - Completion: SYSTEM_EVENT_SCAN_DONE (async mode only)
 *      - Callback Context: Event loop task
 *
 * @par Thread Safety
 *      Not thread-safe. Only one scan can be active at a time.
 *
 * @see sys_wifi_scan_get_count(), sys_wifi_scan_get_result(), sys_wifi_scan_get_all_results()
 */
sys_err_t   sys_wifi_scan_start(bool block, const wifi_scan_config_t *config);
/* Strict admission; BUSY never schedules a delayed scan. */
sys_err_t   sys_wifi_scan_start_checked(bool block, const wifi_scan_config_t *config);

/**
 * @brief Stop an ongoing or pending WiFi scan
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Only one scan can be active at a time.
 *
 * @see sys_wifi_scan_start()
 */
sys_err_t   sys_wifi_scan_stop(void);

/**
 * @brief Get number of scan results
 * @return Number of APs found, -1 on error
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to scan results).
 *
 * @see sys_wifi_scan_start(), sys_wifi_scan_get_result()
 */
int         sys_wifi_scan_get_count(void);

/**
 * @brief Get scan result by index
 * @param[in] index   Result index (0 to sys_wifi_scan_get_count()-1)
 * @param[out] info   Caller-provided buffer to receive AP information
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to scan results).
 *
 * @see sys_wifi_scan_get_count(), sys_wifi_scan_get_all_results()
 */
sys_err_t   sys_wifi_scan_get_result(unsigned int index, wifi_info_t *info);

/**
 * @brief Get all scan results
 * @param[out] info_list   Array buffer to store scan results
 * @param[in,out] count    Input: array size; Output: actual number of results returned
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to scan results).
 *
 * @see sys_wifi_scan_start(), sys_wifi_scan_get_count(), sys_wifi_scan_get_result()
 */
sys_err_t   sys_wifi_scan_get_all_results(wifi_info_t *info_list, uint32_t *count);

/**
 * @brief Get scan result by SSID
 * @param[in] ssid   Target SSID to search for
 * @param[out] info  Output buffer for scan result information
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to scan results).
 *
 * @see sys_wifi_scan_start(), sys_wifi_scan_get_result()
 */
sys_err_t   sys_wifi_scan_get_by_ssid(const char *ssid, wifi_info_t *info);

/**
 * @brief Set scan result expiration time
 * @param[in] vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[in] age  Expiration age in seconds
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies scan configuration.
 *
 * @note Currently placeholder implementation
 */
sys_err_t   sys_wifi_scan_set_expire(int vif, unsigned int age);

/**
 * @brief Flush BSS scan results
 * @param[in] vif        Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[in] flush_age  Age in seconds (0 = flush all, >0 = flush entries older than this)
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if vif is invalid
 * @retval SYS_ERR_INVALID_STATE if WPA supplicant not initialized
 * @retval SYS_ERR if flush operation failed
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies scan results.
 */
sys_err_t   sys_wifi_flush_scan_results(wifi_vif_e vif, int flush_age);

/**
 * @brief Set 5G band scan priority
 * @param[in] enable  1 to prefer 5G, 0 for default behavior
 * @return 0 on success
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies scan configuration.
 */
int         sys_wifi_scan_set_5g_preferred(int enable);

/**
 * @brief Set scan method (all channel or fast scan)
 * @param[in] method  Scan method (WIFI_ALL_CHANNEL_SCAN / WIFI_FAST_SCAN)
 * @return 0 on success
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies scan configuration.
 */
int         sys_wifi_scan_set_method(wifi_scan_method_e method);

/**
 * @brief Load STA connection parameters from NV with default-value fallback
 *
 * This function reads persistent WiFi credentials from NV storage with
 * specific validation rules and default values.
 *
 * Parameter Loading Rules:
 * 1. **auto_conn** (optional): Defaults to 1 if not found in NV
 * 2. **sta_ssid, sta_bssid, channel** (required): ALL must succeed or SYS_ERR
 * 3. **sta_pwd** (optional): Failure is acceptable (for open networks)
 *
 * Default Values:
 * - auto_conn: 1 (auto-reconnect enabled)
 * - sta_pwd: Empty string (if not in NV)
 *
 * Validation Logic:
 *   if (ssid_ok && bssid_ok && channel_ok) → SYS_OK
 *   else → SYS_ERR (incomplete credentials)
 *
 * @param[out] nv_info  Output buffer (NULL = use wifi_ctx->nv_info)
 * @return SYS_OK if all required parameters loaded successfully
 * @retval SYS_ERR if SSID/BSSID/CHANNEL read fails
 *
 * @par Thread Safety
 *      Not thread-safe. NV read operation.
 *
 * @note Sets nv_info->sync = 1 on successful load
 * @warning Function logs password in plaintext (security concern)
 * @see sys_wifi_save_nv_info(), sys_wifi_clean_nv_info()
 */
sys_err_t sys_wifi_load_nv_info(wifi_nv_info_t *nv_info);

/**
 * @brief Atomically save STA credentials to NV with rollback on failure
 *
 * This function implements transactional NV write with all-or-nothing semantics.
 *
 * Write Strategy:
 * - Accumulates error flags across 5 hal_system_set_config() calls
 * - If ANY write fails → rollback ALL via hal_system_del_config()
 * - Ensures NV never contains partial/corrupted credentials
 *
 * Special Behaviors:
 * 1. **Empty SSID**: Returns SYS_OK without writing (silent no-op)
 * 2. **VNET Mode Side Effect** (CONFIG_VNET_SERVICE):
 *    - On successful STA save, DELETES all AP configuration from NV
 *    - Enforces single-mode policy (STA xor AP, not both)
 *    - Applies to VNET service architecture only
 *
 * Saved Fields:
 * - SSID, password, BSSID, channel, auto_conn flag
 *
 * @param nv_info  Configuration to save (must not be NULL)
 * @return SYS_OK if all writes succeed or SSID is empty
 * @retval SYS_ERR_INVALID_ARG if nv_info is NULL
 * @retval SYS_ERR if any hal_system_set_config() fails (after rollback)
 *
 * @par Thread Safety
 *      Not thread-safe. Flash write operation - caller must ensure exclusive access.
 *
 * @note Sets nv_info->sync = 1 on successful save
 * @warning VNET mode deletes AP config as side effect
 * @see sys_wifi_load_nv_info(), sys_wifi_clean_nv_info()
 */
sys_err_t sys_wifi_save_nv_info(wifi_nv_info_t *nv_info);

/**
 * @brief Clear WiFi configuration from NV storage
 * @note Only deletes NV entries; the current RAM cache and network remain active.
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. NV write operation.
 *
 * @see sys_wifi_load_nv_info(), sys_wifi_save_nv_info()
 */
sys_err_t sys_wifi_clean_nv_info(void);

/**
 * @brief Initiate STA connection using NV-stored credentials with fast-connect optimization
 *
 * This function restores a previous WiFi connection from NV storage, enabling
 * rapid reconnection without full-channel scanning.
 *
 * Implementation Details:
 * - If nv_info is NULL, uses context singleton (wifi_ctx->nv_info) as fallback
 * - Validates SSID presence (returns SYS_ERR if empty)
 * - Applies fast-connect optimization when CONFIG_FAST_CONNECT is enabled:
 *   * Uses cached channel from NV to skip full-channel scan
 *   * Reduces reconnection time from ~3s to <500ms
 * - Platform-specific: ECW6700 prioritizes 5GHz band when enabled
 *
 * Typical Usage:
 *   Called by sys_wifi_sta_auto_connect_start() during system boot
 *   to restore last-known-good connection.
 *
 * @param[in] nv_info  NV information to apply (NULL = use cached context)
 * @return SYS_OK on success
 * @retval SYS_ERR if SSID is empty or NV data invalid
 *
 * @par Thread Safety
 *      Not thread-safe. WiFi operation.
 *
 * @note Does NOT wait for connection completion - this is async operation
 * @see sys_wifi_sta_auto_connect_start(), sys_wifi_load_nv_info()
 */
sys_err_t sys_wifi_set_sta_by_nv(wifi_nv_info_t *nv_info);

/**
 * @brief Enable/disable auto-save NV on STA connect
 * @param[in] enable  true to enable, false to disable
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies policy configuration.
 *
 * @see sys_wifi_get_auto_save_nv(), sys_wifi_save_nv_info()
 */
void sys_wifi_set_auto_save_nv(bool enable);

/**
 * @brief Get auto-save NV policy status
 * @return true if enabled, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to policy configuration).
 *
 * @see sys_wifi_set_auto_save_nv()
 */
bool sys_wifi_get_auto_save_nv(void);

/**
 * @if ECW6700
 * @brief Set WiFi country information (supports 2.4G + 5G)
 * @else
 * @brief Set WiFi country information
 * @endif
 *
 * Configures WiFi regulatory domain and automatically saves to NV.
 * 0 values will be auto-filled from regulatory database by driver layer.
 *
 * Auto-Save Behavior:
 * - On success, configuration is automatically persisted to NV
 * - On next boot, sys_wifi_init() will restore this configuration
 * - No need to call separate save function
 *
 * @param info Country information structure
 * @return 0 on success, -1 if info is NULL, country_code is empty, or WPA layer configuration failed.
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state and writes to NV.
 *
 * @note Country code CANNOT be empty (mandatory validation)
 * @note Configuration is auto-saved to NV on success
 * @see sys_wifi_get_country_info()
 */
int sys_wifi_set_country_info(const wifi_country_info_t *info);

/**
 * @brief Get current WiFi country information
 *
 * Retrieves current country configuration from driver layer.
 *
 * @param[out] info  Output buffer for country information
 * @return 0 on success, -1 on failure
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @see sys_wifi_set_country_info()
 */
int sys_wifi_get_country_info(wifi_country_info_t *info);

/**
 * @brief Validate if a channel is allowed for AP operation under current country
 *
 * Checks both 2.4GHz (1-14) and 5GHz (36-177) channels against the
 * current country regulatory configuration.
 *
 * @param[in] channel  Channel number to validate
 * @return true if channel is valid for current country, false otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to country info).
 */
bool sys_wifi_is_valid_ap_channel(uint8_t channel);

/**
 * @brief Set WiFi regulatory country code (legacy interface, 2.4G only)
 *
 * Legacy compatibility wrapper. Internally calls sys_wifi_set_country_info()
 * with default parameters and auto-saves to NV.
 *
 * @param[in] country_code  ISO country code (2 characters, e.g., "US", "CN")
 * @return 0 on success, -1 on failure
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state and writes to NV.
 *
 * @note Kept for backward compatibility
 * @note Auto-fills all channel/power parameters from regulatory database
 * @note Auto-saves to NV on success
 * @see sys_wifi_set_country_info()
 */
int sys_wifi_set_country_code(const char *country_code);

/**
 * @brief Get WiFi regulatory country code (legacy interface)
 *
 * Legacy compatibility wrapper. Internally calls sys_wifi_get_country_info()
 * and extracts only the country code.
 *
 * @param[out] country_code  Output buffer (at least 3 bytes)
 * @return 0 on success, -1 on failure
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to driver state).
 *
 * @note Kept for backward compatibility
 * @see sys_wifi_get_country_info()
 */
int sys_wifi_get_country_code(char *country_code);

/**
 * @brief Set WPA supplicant log level
 * @param[in] level  Log level (0=ERROR, 1=WARNING, 2=INFO, 3=DEBUG)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies WPA configuration.
 */
sys_err_t sys_wifi_wpa_set_log_level(int level);

/**
 * @brief Dump WPA supplicant status for debugging
 * @param[in] vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only debug output).
 */
sys_err_t sys_wifi_wpa_dump_status(unsigned char vif);

/**
 * @brief Dump WPA supplicant network list for debugging
 * @param[in] vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe (read-only debug output).
 */
sys_err_t sys_wifi_wpa_dump_list_networks(unsigned char vif);

/**
 * @brief Clear WPA PMK cache for the selected interface
 * @param[in] vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Thread-safe. Operation is dispatched to the WPA supplicant task.
 */
sys_err_t sys_wifi_pmk_cache_clean(unsigned char vif);


/**
 * @brief Set WiFi band mode with protocol configuration adjustment
 *
 * Configure the operating WiFi band (2.4GHz, 5GHz, or AUTO). This function
 * adjusts VIF protocol configurations by clearing unused band settings based
 * on the selected mode.
 *
 * Band Modes:
 * -----------
 * - **WIFI_BAND_MODE_AUTO**: Dual-band operation (2.4G + 5G)
 * - **WIFI_BAND_MODE_2G_ONLY**: 2.4GHz only (channels 1-14)
 * - **WIFI_BAND_MODE_5G_ONLY**: 5GHz only (channels 36-165, ECW6700 only)
 *
 * Protocol Configuration Adjustment:
 * -----------------------------------
 * When band mode changes, the function adjusts protocol configurations for
 * ALL VIFs (STA and AP) by clearing inactive band settings:
 *
 * **Switching to 5G_ONLY:**
 * - Clears ghz_2g field in protocol_bitmap (sets to 0)
 * - Preserves ghz_5g field unchanged
 * - Example: protocol {.ghz_2g=0x07, .ghz_5g=0x0E} → {.ghz_2g=0x00, .ghz_5g=0x0E}
 *
 * **Switching to 2G_ONLY:**
 * - Clears ghz_5g field in protocol_bitmap (sets to 0)
 * - Preserves ghz_2g field unchanged
 * - Example: protocol {.ghz_2g=0x07, .ghz_5g=0x0E} → {.ghz_2g=0x07, .ghz_5g=0x00}
 *
 * **Switching to AUTO:**
 * - Preserves both ghz_2g and ghz_5g fields unchanged
 * - Both band configurations remain active
 *
 * Side Effects:
 * -------------
 * - VIF protocol configurations may be modified (unused bands cleared)
 * - Modified configurations synced to fhost layer via fhost_set_protocol()
 * - Logs warnings (SYS_LOGW) for each VIF that required adjustment
 *
 * @param mode Band mode to set
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if mode is invalid (>= WIFI_BAND_MODE_MAX)
 * @retval SYS_ERR_WIFI_FAIL if fhost layer sync fails
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state.
 *
 * @note Configuration is saved to context immediately
 * @note Takes effect on next connection attempt
 * @warning Changing band mode will clear protocol configuration for inactive bands
 *
 * @par Usage Example:
 * @code
 * // Switch to 2.4G only mode (clears 5G protocols)
 * sys_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
 *
 * // Enable dual-band operation (ECW6700, preserves both band protocols)
 * sys_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
 *
 * // Force 5G only (ECW6700, clears 2.4G protocols)
 * sys_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
 * @endcode
 *
 * @see sys_wifi_get_band_mode(), sys_wifi_set_protocol()
 */
sys_err_t sys_wifi_set_band_mode(wifi_band_mode_e mode);

/**
 * @brief Get current WiFi band mode configuration
 *
 * Retrieve the currently configured band mode from system context.
 *
 * @param[out] mode Pointer to store band mode
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if mode pointer is NULL
 *
 * @note Returns configured mode, not the actual operating band
 * @see sys_wifi_set_band_mode()
 */
sys_err_t sys_wifi_get_band_mode(wifi_band_mode_e *mode);

/**
 * @if ECW6700
 * @brief Set STA bandwidth configuration for 2.4G and 5G
 *
 * Configure the bandwidth for STA (Station) interface with separate 2.4G and 5G settings.
 * This function provides bandwidth control specifically for STA connections.
 *
 * @param bw  Bandwidth structure with separate ghz_2g and ghz_5g fields:
 *            - WIFI_BW_AUTO: Automatic bandwidth negotiation
 *            - WIFI_BW_HT20: Force 20MHz bandwidth only
 *            - WIFI_BW_HT40: 40MHz bandwidth (2.4G/5G)
 *            - WIFI_BW_VHT80: 80MHz bandwidth (5G only, NOT IMPLEMENTED - reserved for future)
 *            - WIFI_BW_VHT160: 160MHz bandwidth (5G only, NOT IMPLEMENTED - reserved for future)
 *
 * @par Example:
 * @code
 * wifi_bandwidth_t bw = WIFI_MAKE_BANDWIDTH(WIFI_BW_HT20, WIFI_BW_HT40);
 * sys_wifi_sta_set_bandwidth(bw);
 * @endcode
 * @else
 * @brief Set STA bandwidth configuration for 2.4G
 *
 * Configure the bandwidth for STA (Station) interface on 2.4G-only targets.
 * This function provides bandwidth control specifically for STA connections.
 *
 * @param bw  Bandwidth structure. Only ghz_2g is effective on this target:
 *            - WIFI_BW_AUTO: Automatic bandwidth negotiation
 *            - WIFI_BW_HT20: Force 20MHz bandwidth only
 *            - WIFI_BW_HT40: 40MHz bandwidth
 *            Set ghz_5g to WIFI_BW_AUTO.
 *
 * @par Example:
 * @code
 * wifi_bandwidth_t bw = WIFI_MAKE_BANDWIDTH(WIFI_BW_HT20, WIFI_BW_AUTO);
 * sys_wifi_sta_set_bandwidth(bw);
 * @endcode
 * @endif
 *
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if bandwidth parameter is invalid
 *
 * @note This function only configures STA interface bandwidth
 * @note SoftAP interface does NOT support 40MHz bandwidth due to software limitation (always 20MHz)
 * @note Configuration is saved to NV storage and takes effect on next connection
 * @warning WIFI_BW_VHT80 and WIFI_BW_VHT160 are reserved for future support, not currently implemented
 */
sys_err_t sys_wifi_sta_set_bandwidth(wifi_bandwidth_t bw);

/**
 * @brief Get STA bandwidth configuration (frequency-band separated)
 *
 * Retrieve the current bandwidth configuration for STA interface with separate
 * 2.4G and 5G settings.
 *
 * @param[out] bw  Pointer to store current bandwidth structure
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if bw pointer is NULL
 *
 * @note Returns the configured bandwidth, not the actual negotiated bandwidth
 */
sys_err_t sys_wifi_sta_get_bandwidth(wifi_bandwidth_t *bw);

/**
 * @if ECW6700
 * @brief Set WiFi protocol configuration per VIF for 2.4G and 5G
 *
 * Configure supported WiFi protocols (802.11b/g/n/ac/ax) for specified interface
 * with separate configuration for 2.4G and 5G bands. This allows independent
 * protocol control for each frequency band.
 *
 * Supported Protocol Combinations:
 * --------------------------------
 * **2.4G Band (ghz_2g field):**
 * - Pure legacy: b, bg
 * - With HT: bgn
 * - With HE: bgnax
 *
 * **5G Band (ghz_5g field):**
 * - Pure legacy: a
 * - With HT: an
 * - With VHT: anac
 * - With HE: anacax
 *
 * Validation Rules:
 * -----------------
 * - 5G band CANNOT have 11B (DSSS/CCK unavailable on 5GHz)
 * - No auto-correction is performed - invalid combinations are rejected
 *
 * @param vif      Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param protocol Protocol structure with separate ghz_2g and ghz_5g bitmaps
 * @retval SYS_ERR_INVALID_ARG if vif is invalid or 5G has 11B
 *
 * @par Usage Example:
 * @code
 * wifi_protocol_t proto = WIFI_MAKE_PROTOCOL(
 *     WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,  // 2.4G
 *     WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC  // 5G
 * );
 * sys_wifi_set_protocol(WIFI_VIF_STA, proto);
 * @endcode
 * @else
 * @brief Set WiFi protocol configuration per VIF for 2.4G
 *
 * Configure supported WiFi protocols (802.11b/g/n/ax) for specified interface
 * on 2.4G-only targets.
 *
 * Supported Protocol Combinations:
 * --------------------------------
 * **2.4G Band (ghz_2g field):**
 * - Pure legacy: b, bg
 * - With HT: bgn
 * - With HE: bgnax
 *
 * @param vif      Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param protocol Protocol structure. Only ghz_2g is effective on this target;
 *                 set ghz_5g to 0.
 * @retval SYS_ERR_INVALID_ARG if vif is invalid or protocol is invalid
 *
 * @par Usage Example:
 * @code
 * wifi_protocol_t proto = WIFI_MAKE_PROTOCOL(
 *     WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
 *     0
 * );
 * sys_wifi_set_protocol(WIFI_VIF_STA, proto);
 * @endcode
 * @endif
 *
 * @return SYS_OK on success, error code otherwise
 *
 * @par Thread Safety
 *      Not thread-safe. Modifies driver state.
 *
 * @note Configuration is saved to context and synced to fhost layer
 * @note Takes effect on next connection attempt
 *
 * @see sys_wifi_get_protocol(), sys_wifi_set_band_mode()
 */
sys_err_t sys_wifi_set_protocol(wifi_vif_e vif, wifi_protocol_t protocol);

/**
 * @if ECW6700
 * @brief Get current WiFi protocol configuration for specified VIF on 2.4G and 5G
 *
 * Retrieve the current protocol configuration from system context with separate
 * 2.4G and 5G bitmaps.
 *
 * @else
 * @brief Get current WiFi protocol configuration for specified VIF on 2.4G
 *
 * Retrieve the current protocol configuration from system context. Only the
 * ghz_2g bitmap is effective on this target.
 * @endif
 *
 * @param vif      Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param[out] protocol Pointer to store protocol structure
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if vif is invalid or protocol pointer is NULL
 *
 * @note Returns configured protocol, not the negotiated protocol with peer
 * @see sys_wifi_set_protocol()
 */
sys_err_t sys_wifi_get_protocol(wifi_vif_e vif, wifi_protocol_t *protocol);

/**
 * @brief Set the runtime CCK transmit preamble policy for STA or AP.
 * @param[in] vif WIFI_VIF_STA or WIFI_VIF_AP (connection is not required)
 * @param[in] preamble AUTO (0), LONG_ONLY (1), or SHORT_ONLY (2)
 * @return SYS_OK on success, error code otherwise
 * @note AUTO disables the override and preserves normal rate-control output.
 *       Forced modes apply after rate selection; 1 Mbps always stays long.
 *       Non-CCK frames are unchanged. The setting survives reconnect, resets
 *       on reboot, and does not write NV or modify RC sample tables.
 */
sys_err_t sys_wifi_set_preamble(wifi_vif_e vif, wifi_preamble_t preamble);

/**
 * @brief Get disconnect reason (simplified enum)
 * @return wifi_discon_reason_e Simplified disconnect reason
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_get_disconnect_reason_code()
 */
wifi_discon_reason_e sys_wifi_get_disconnect_reason(void);

/**
 * @brief Get disconnect reason code (raw IEEE 802.11 code)
 * @return uint16_t Raw IEEE 802.11 reason code
 *
 * @par Thread Safety
 *      Thread-safe (read-only access to internal state).
 *
 * @see sys_wifi_get_disconnect_reason()
 */
uint16_t sys_wifi_get_disconnect_reason_code(void);

/**
 * @brief Initialize WiFi event subscription system
 * @note Must be called during WiFi initialization
 *
 * @par Thread Safety
 *      Not thread-safe. Must be called once during system initialization.
 */
void sys_wifi_event_init(void);

/**
 * @brief Enable or disable the instrument signalling test data path
 *
 * Callers enable this only after the requested STA SSID is connected.
 * Disabling is idempotent and restores the normal RX and rate-control policy.
 *
 * @param[in] enable true to activate, false to restore normal operation
 * @return SYS_OK on success, SYS_ERR if the platform rejects the transition
 */
sys_err_t sys_wifi_signal_test_set_enabled(bool enable);

/**
 * @brief Set manual TX rate for raw frame injection
 *
 * Computes rate_info from standard and MCS index.
 *
 * @param[in] standard   Rate standard (inject_rate_standard_t)
 * @param[in] mcs_index  MCS index within the standard
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_tx_rate(int standard, int mcs_index);

/**
 * @brief Clear manual TX rate (revert to automatic rate control)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_clear_tx_rate(void);

/**
 * @brief Set manual ACK policy for TX frames
 * @param[in] no_ack  1=no ACK, 0=normal ACK
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_tx_ack_policy(int no_ack);

/**
 * @brief Enable/disable ACK response in monitor mode
 * @param[in] enable  true to keep ACK enabled, false to disable
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_monitor_rx_ack(bool enable);

/**
 * @brief Set/clear CFO tracking BSSID for monitor mode
 * @param[in] enable  1=set BSSID for CFO tracking, 0=clear
 * @param[in] bssid   6-byte BSSID (used when enable=1, can be NULL when enable=0)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_tx_cfo(int enable, const uint8_t *bssid);

/**
 * @brief Set global STA auth/assoc response retry policy
 *
 * This controls request-level timeout retries in the connection state machine.
 * It is global state and applies to later STA connection attempts.
 *
 * @param[in] auth_max_attempts   Total auth request attempts, including first TX.
 * @param[in] assoc_max_attempts  Total assoc request attempts, including first TX.
 * @param[in] rsp_timeout_us      Auth/assoc response timeout in microseconds.
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on invalid arguments, SYS_ERR otherwise.
 */
sys_err_t sys_wifi_sta_set_auth_assoc_retry_policy(uint8_t auth_max_attempts,
                                                   uint8_t assoc_max_attempts,
                                                   uint32_t rsp_timeout_us);

/* ============ Debug-only WiFi tuning APIs (NOT for production) ============ */

/**
 * @brief [DEBUG ONLY] Set 802.11 Short Retry Limit for mgmt/data frames.
 *
 * For debug / lab bring-up only. Bypasses the rate-control module's normal
 * retry policy management; must NOT be called from production code paths.
 *
 * Platform behavior:alid override range is 0..254 (incl.
 *      0 "transmit once, no retries"); 255 is the no-override sentinel that
 *      restores rate-control's normal SRL ownership.
 * @param[in] mgmt_srl  Management frame Short Retry Limit.
 * @param[in] data_srl  Data frame Short Retry Limit.
 * @return SYS_OK on success, SYS_ERR if not supported on the current platform.
 */
sys_err_t sys_wifi_dbg_set_retry_limit(uint8_t mgmt_srl, uint8_t data_srl);

#ifdef __cplusplus
}
#endif
#endif/*_SYSTEM_WIFI_H*/

