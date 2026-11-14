/**
 * @file wpas_struct_iface.h
 * @brief WPA Supplicant Core Processing Layer - Structure Definitions and Interface
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2025 Eswin Computing. All rights reserved.
 */

#ifndef WPAS_STRUCT_IFACE_H
#define WPAS_STRUCT_IFACE_H

#include <stdint.h>
#include "utils/common.h"
#include "utils/list.h"
#include "wpa_supplicant_i.h"
#include "bss.h"
#include "common/wpa_common.h"
#ifndef SYS_OK
#include "eswin_errno.h"
#endif

/* Forward declarations */
struct wpa_supplicant;


/**
 * @brief Authentication mode enumeration
 */
typedef enum {
    WPA_CTRL_AUTH_OPEN = 0,          /**< Open authentication (no security) */
    WPA_CTRL_AUTH_WEP,               /**< WEP authentication */
    WPA_CTRL_AUTH_WPA_PSK,           /**< WPA-PSK authentication */
    WPA_CTRL_AUTH_WPA2_PSK,          /**< WPA2-PSK authentication */
    WPA_CTRL_AUTH_WPA_WPA2_PSK,      /**< WPA/WPA2-PSK mixed mode */
    WPA_CTRL_AUTH_WPA3_SAE,          /**< WPA3-SAE authentication */
    WPA_CTRL_AUTH_WPA2_WPA3_PSK,     /**< WPA2/WPA3-PSK mixed mode */
    WPA_CTRL_AUTH_WPA2_ENTERPRISE,   /**< WPA2-Enterprise (802.1X) */
    WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE, /**< WPA2/WPA3-Enterprise mixed mode */
    WPA_CTRL_AUTH_WPA3_ENT_192,      /**< WPA3-Enterprise 192-bit (Suite B 192) */
    WPA_CTRL_AUTH_WPA3_ENTERPRISE,   /**< WPA3-Enterprise (Suite B) */
    WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE, /**< WPA/WPA2-Enterprise mixed mode */
    WPA_CTRL_AUTH_ENTERPRISE_AUTO,   /**< Enterprise auto mode, selected from scan results */
    WPA_CTRL_AUTH_WAPI_PSK,          /**< WAPI pre-shared key authentication */
    WPA_CTRL_AUTH_WAPI_CERT,         /**< WAPI certificate authentication */
    WPA_CTRL_AUTH_MAX                /**< Maximum value for boundary checking */
} wpa_auth_mode_e;

/**
 * @brief Enterprise EAP method selection
 */
typedef enum {
    WPA_CTRL_EAP_NONE = 0,           /**< No Enterprise EAP method */
    WPA_CTRL_EAP_TLS,                /**< EAP-TLS */
    WPA_CTRL_EAP_PEAP,               /**< EAP-PEAP */
    WPA_CTRL_EAP_TTLS,               /**< EAP-TTLS */
    WPA_CTRL_EAP_LEAP,               /**< EAP-LEAP */
} wpa_eap_method_e;

/**
 * @brief Cipher mode enumeration
 */
typedef enum {
    WPA_CTRL_CIPHER_NONE = 0,        /**< No encryption */
    WPA_CTRL_CIPHER_WEP40,           /**< WEP 40-bit encryption */
    WPA_CTRL_CIPHER_WEP104,          /**< WEP 104-bit encryption */
    WPA_CTRL_CIPHER_TKIP,            /**< TKIP encryption */
    WPA_CTRL_CIPHER_CCMP,            /**< CCMP (AES) encryption */
    WPA_CTRL_CIPHER_GCMP_256,        /**< GCMP-256 encryption (WPA3) */
    WPA_CTRL_CIPHER_SMS4,            /**< SMS4 encryption (WAPI) */
    WPA_CTRL_CIPHER_MAX              /**< Maximum value for boundary checking */
} wpa_cipher_e;

/**
 * @brief Complete network configuration structure
 *
 * This structure contains all parameters needed for network configuration,
 * passed as a single unit to avoid multiple function calls.
 */
typedef struct {
    /* Basic information */
    unsigned char  network_id;             /**< Network ID (typically 0) */
    unsigned char  ssid[32];               /**< SSID of the network */
    unsigned char  ssid_len;               /**< Length of SSID */

    /* Authentication and encryption */
    signed char    password[64];           /**< Network password/passphrase */
    unsigned char  password_len;           /**< Length of password */
    wpa_auth_mode_e auth_mode;             /**< Authentication mode */
    wpa_cipher_e    pairwise;              /**< Pairwise cipher */
    wpa_cipher_e    group;                 /**< Group cipher */
    wpa_eap_method_e eap_method;           /**< Explicit Enterprise EAP method */
    const char     *identity;              /**< EAP identity for enterprise network */
    size_t          identity_len;          /**< Length of identity */
    const char     *eap_password;          /**< EAP password for PEAP/TTLS/LEAP */
    size_t          eap_password_len;      /**< Length of EAP password */
    const char     *phase2;                /**< Phase 2 authentication parameters */
    const unsigned char *ca_cert;          /**< CA certificate content in memory */
    size_t          ca_cert_len;           /**< Length of CA certificate */
    const unsigned char *client_cert;      /**< Client certificate content in memory */
    size_t          client_cert_len;       /**< Length of client certificate */
    const unsigned char *private_key;      /**< Private key content in memory */
    size_t          private_key_len;       /**< Length of private key */
    const char     *private_key_passwd;    /**< Optional private key password */
    size_t          private_key_passwd_len;/**< Length of private key password */
    unsigned short  ap_inactive_time;      /**< AP client inactive timeout (seconds, 0=use WPA default) */

    /* Connection parameters */
    unsigned char  bssid[6];               /**< BSSID (optional, all zeros means any) */
    unsigned short frequency;              /**< Frequency in MHz */
    unsigned char  channel;                /**< Channel number (1-14 for 2.4GHz) */

    /* AP mode specific */
    unsigned char  mode;                   /**< Mode: 0=STA, 2=AP */
    unsigned char  hidden_ssid;            /**< Whether to hide SSID (AP mode) */

    /* Scan options */
    unsigned char  scan_ssid;              /**< Whether to scan for hidden SSID */

    unsigned short  beacon_int;            /**< Beacon interval (AP mode) */
    unsigned char   max_connect;           /**< Maximum number of connections (AP mode) */
    unsigned short candidate_frequency;   /**< Optional second initial scan frequency in MHz */
} wpa_network_config_t;

/* Per-band selection results and current network scan hints; never persisted. */
struct wpas_network_scan_result {
    unsigned short selected_freq_2g;
    unsigned short selected_freq_5g;
    unsigned short frequency;
    unsigned short candidate_frequency;
    unsigned char ssid[32];
    unsigned char ssid_len;
    unsigned char bssid[6];
    unsigned char bssid_set;
    unsigned char connected;
};

typedef struct {
    unsigned short frequency;
    unsigned short candidate_frequency;
} wpas_network_scan_hint_t;

/**
 * @brief Scan configuration structure
 */
typedef struct {
    unsigned char  ssid[33];               /**< Specific SSID to scan (optional, 32 bytes + \0) */
    unsigned char  bssid[6];               /**< Specific BSSID to scan (optional) */
    unsigned int   scan_time;              /**< Scan time per channel in milliseconds */
    unsigned char  max_item;               /**< Maximum number of scan results */
    unsigned char  passive;                /**< Passive scan mode flag */
    unsigned char  freq_count;             /**< Number of frequencies in list */
    unsigned short *freq_list;             /**< List of frequencies to scan */
} wpa_scan_config_t;

/**
 * @brief Channel Switch Announcement (CSA) configuration
 */
typedef struct {
    unsigned short freq;                   /**< Target frequency in MHz */
    unsigned char csa_count;               /**< CSA announcement count */
    unsigned char blocktx;                 /**< Block TX during channel switch */
} wpa_csa_config_t;

/**
 * @brief SoftAP station information
 */
typedef struct {
    uint8_t mac_addr[6];                   /**< MAC address of connected station */
    unsigned short aid;                    /**< Association ID */
} wpa_softap_sta_info_t;

/**
 * @brief WiFi information structure for WPA layer
 *
 * This is the WPA layer's WiFi information type, independent of
 * upper layer's wifi_info_t structure.
 */
typedef struct {
    uint8_t   ssid[32];                    /**< SSID of the network */
    uint8_t   ssid_len;                    /**< Length of SSID */
    uint8_t   bssid[6];                    /**< BSSID (MAC address of AP) */
    uint8_t   channel;                     /**< Channel number */
    int8_t    rssi;                        /**< Received Signal Strength Indicator */
    uint8_t   auth;                        /**< Authentication method */
    uint8_t   cipher;                      /**< Cipher type (pairwise) */
    uint8_t   group_cipher;               /**< Group cipher type */
    uint8_t   wifi_protocol;              /**< WiFi protocol bitmask (b/g/n/ax/a/ac) */
    uint8_t   wps;                        /**< WPS support flag (0/1) */
    uint8_t   pwd[64];                     /**< Password/passphrase */
    uint8_t   pwd_len;                     /**< Length of password */
} wpa_wifi_info_t;

/**
 * ============================================================================
 * Core Processing Function Declarations - Direct WPA Supplicant Operations
 * ============================================================================
 * Naming Convention: wpas_core_<module>_<action>_<object>
 *
 * Important Notes:
 * 1. These functions should NOT be called directly by system_wifi.c
 * 2. They should be called through wpas_ctrl_* functions in wpas_cntrl_api.h
 * 3. All functions accept struct wpa_supplicant* parameter (not vif_id)
 * 4. These functions are called within eloop handlers in wpas_cntrl_api.c
 * 5. All functions return sys_err_t type uniformly
 */

/* ============================================================================
 * Interface Control
 * ============================================================================ */

/**
 * @brief Dump interface status to console
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param params Optional parameters (can be NULL)
 * @param buf Output buffer for status string
 * @param buflen Size of output buffer
 * @return int Number of bytes written to buffer
 */
int wpas_core_iface_dump_status(struct wpa_supplicant *wpa_s,const char *params, char *buf, size_t buflen);

/**
 * @brief Update interface MAC address
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param mac MAC address string (format: "xx:xx:xx:xx:xx:xx")
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_iface_update_mac(struct wpa_supplicant *wpa_s, const char *mac);

/**
 * @brief Dump configured network list to console (for debugging)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param buf Output buffer for network list
 * @param buflen Size of output buffer
 * @return int Number of bytes written to buffer
 */
int wpas_core_iface_dump_list_networks(struct wpa_supplicant *wpa_s,
                                       char *buf, size_t buflen);

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

/**
 * @brief Configure network (STA/AP)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config Network configuration parameters
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_network_config(struct wpa_supplicant *wpa_s,
                                    const wpa_network_config_t *config);

/**
 * @brief Apply Enterprise authentication parameters to an existing STA network
 * @param ssid WPA network profile prepared with EAP identity/methods
 * @param auth_mode Enterprise authentication mode selected from scan result
 * @return sys_err_t Execution result
 */
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
sys_err_t wpas_core_apply_enterprise_auth(struct wpa_ssid *ssid,
                                          wpa_auth_mode_e auth_mode);
sys_err_t wpas_core_apply_enterprise_auth_ie(struct wpa_ssid *ssid,
                                             wpa_auth_mode_e auth_mode,
                                             const struct wpa_ie_data *ie);
int wpas_core_enterprise_pmksa_snapshot_rebind(struct wpa_supplicant *wpa_s,
                                               struct wpa_ssid *ssid,
                                               const u8 *bssid);
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

/**
 * @brief Remove all network configurations
 * @param wpa_s Pointer to wpa_supplicant instance
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_network_remove(struct wpa_supplicant *wpa_s);

/**
 * @brief Disconnect from the current network while preserving configuration
 * @param wpa_s Pointer to wpa_supplicant instance
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_network_disconnect(struct wpa_supplicant *wpa_s);

/**
 * @brief Reconnect to the configured network, optionally comparing config
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config If non-NULL, compare with current network; if mismatch return error.
 *               If NULL, skip comparison and reconnect directly.
 * @return SYS_OK on reconnect initiated, SYS_ERR on mismatch or no network
 */
sys_err_t wpas_core_network_reconnect(struct wpa_supplicant *wpa_s,
                                       const wpa_network_config_t *config);

/**
 * @brief Check whether the current network has a matching PMKSA cache entry
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config If non-NULL, compare with current network before checking PMKSA.
 * @return SYS_OK when a matching PMKSA exists, error code otherwise
 */
sys_err_t wpas_core_network_has_pmksa(struct wpa_supplicant *wpa_s,
                                       const wpa_network_config_t *config);

/**
 * @brief Clear cached PMK state for STA reconnect/authentication
 * @param wpa_s Pointer to wpa_supplicant instance
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_pmk_cache_clean(struct wpa_supplicant *wpa_s);

/* ============================================================================
 * Scan Operations
 * ============================================================================ */

/**
 * @brief Start WiFi scan
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config Scan configuration parameters (optional, can be NULL)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_scan_start_checked(struct wpa_supplicant *wpa_s,
                                      const wpa_scan_config_t *config);
sys_err_t wpas_core_scan_start(struct wpa_supplicant *wpa_s,
                                const wpa_scan_config_t *config);

/**
 * @brief Stop an ongoing or pending scan
 * @param wpa_s Pointer to wpa_supplicant instance
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_scan_stop(struct wpa_supplicant *wpa_s);

/**
 * @brief Flush BSS scan results
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param flush_age Age in seconds (0 = flush all, >0 = flush entries older than this)
 * @return 0 on success, -1 on invalid argument
 */
int wpas_core_scan_flush_bss(struct wpa_supplicant *wpa_s, int flush_age);

/* ============================================================================
 * Station Information
 * ============================================================================ */

/**
 * @brief Get WiFi station connection information
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param user_ctx Output buffer for WiFi info (wpa_wifi_info_t*)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_station_get_info(struct wpa_supplicant *wpa_s, void *user_ctx);

/**
 * @brief Get BSS mode (authentication and cipher methods)
 * @param bss Pointer to BSS structure
 * @param auth Output: authentication method
 * @param cipher Output: cipher type
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_bss_get_mode(struct wpa_bss *bss, unsigned char *auth, unsigned char *cipher);

/**
 * @brief Get BSS extended info (group cipher, wifi protocol, WPS)
 * @param bss Pointer to BSS structure
 * @param group_cipher Output: group cipher type
 * @param wifi_protocol Output: wifi protocol bitmask
 * @param wps_flag Output: WPS support flag
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_bss_get_extra_info(struct wpa_bss *bss, uint8_t *group_cipher,
                                       uint8_t *wifi_protocol, uint8_t *wps_flag);

/* ============================================================================
 * SoftAP Management
 * ============================================================================ */

/**
 * @brief Get SoftAP configuration
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param user_ctx Output buffer for configuration (wpa_network_config_t*)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_softap_get_config(struct wpa_supplicant *wpa_s, void *user_ctx);

/**
 * @brief Configure Channel Switch Announcement (CSA)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config CSA configuration parameters
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_softap_config_csa(struct wpa_supplicant *wpa_s,
                                const wpa_csa_config_t *config);

/**
 * @brief Set SoftAP beacon vendor specific IE (runtime)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param data Vendor IE payload (may be NULL to clear)
 * @param len Vendor IE payload length
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_softap_set_vendor_elements(struct wpa_supplicant *wpa_s,
                                                const unsigned char *data,
                                                unsigned int len);

/**
 * @brief Set SoftAP beacon interval (runtime)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param interval Beacon interval in ms
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_softap_set_beacon_interval(struct wpa_supplicant *wpa_s,
                                                unsigned int interval);

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
wpa_auth_mode_e wpas_core_classify_enterprise_auth(int key_mgmt,
                                                   int capabilities,
                                                   int proto);
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

#endif
