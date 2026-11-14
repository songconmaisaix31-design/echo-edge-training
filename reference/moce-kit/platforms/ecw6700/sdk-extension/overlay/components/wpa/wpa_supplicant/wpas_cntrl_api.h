/**
 * @file wpas_cntrl_api.h
 * @brief WPA Supplicant Control API - System WiFi Layer Interface
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2025 Eswin Computing. All rights reserved.
 */

#ifndef __WPAS_CTRL_H__
#define __WPAS_CTRL_H__

/**
 * ============================================================================
 * WPA Supplicant Control Interface - Structure-Based Communication
 * ============================================================================
 *
 * Architecture Overview:
 *
 * [THREE-LAYER ARCHITECTURE]
 * 1. system_wifi.c (Application Layer)
 *    - Calls wpas_ctrl_* APIs defined in this file
 *    - Passes vif_id and structure parameters
 *
 * 2. wpas_cntrl_api.c (Control Layer)
 *    - Implements wpas_ctrl_* APIs
 *    - Converts vif_id to wpa_supplicant* pointer
 *    - Registers handlers via schedule_eloop_work()
 *    - Handlers invoke lower-layer functions and handle synchronization
 *
 * 3. wpas_struct_iface.c (Core Processing Layer)
 *    - Implements pure processing logic
 *    - Accepts wpa_supplicant* parameter
 *    - Directly operates on wpa_supplicant structures
 *
 * [UNIFIED HANDLER PATTERN]
 * static void handler_xxx(void *eloop_data, void *user_ctx)
 * {
 *     struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
 *     struct ctrl_iface_priv *priv = wpa_get_ctrl_iface_priv(wpa_s);
 *
 *     if (!priv) return;
 *
 *     priv->status = wpa_xxx_struct(wpa_s, (const xxx_config_t *)user_ctx);
 *     os_sem_post(priv->finish);
 * }
 *
 * [USAGE EXAMPLE]
 * // In system_wifi.c:
 * wpa_network_config_t config = {...};
 * sys_err_t ret = wpas_ctrl_network_start(WIFI_VIF_STA, &config);
 * ============================================================================
 */

#include <stdint.h>
#include "wpas_struct_iface.h"
#include "eswin_errno.h"
#include "eswin_wifi_types.h"

#define ESWIN_WIFI_IF_NUM     			(2)
#define ESWIN_WIFI_IF_NAME_0	            	("wl0")
#define ESWIN_WIFI_IF_NAME_1	            	("wl1")
#define WPAS_CTRL_BSSID_LEN			(6)
#define WPAS_CTRL_SSID_MAX_LEN			SSID_MAX_LEN

/*
 * ============================================================================
 * WPA Control Interface - Using WPA Internal Types Only
 * ============================================================================
 * Important Principles:
 * 1. WPA layer should NOT know about system_wifi types (wifi_sta_config_t, etc.)
 * 2. This interface only exposes WPA types (wpa_network_config_t, etc.)
 * 3. system_wifi.c is responsible for converting its types to WPA types
 * 4. Follows Dependency Inversion Principle: lower layer defines interface, upper layer adapts to it
 */
/* ============================================================================
 * Naming Convention: wpas_ctrl_<module>_<action>_<object>
 * ============================================================================ */

/* ============================================================================
 * System Configuration
 * ============================================================================ */

/**
 * @brief Get country code
 * @param country_code Pointer to country code buffer
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_system_get_country(char *country_code);

/**
 * @brief Set country code
 * @param country_code Pointer to country code string
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_system_set_country(const char *country_code);

/**
 * @brief Set WiFi country information (supports 2.4G + 5G)
 * @param info Pointer to country information structure
 * @return 0 on success, -1 on failure
 */
int wpas_set_country_info(const wifi_country_info_t *info);

/**
 * @brief Apply AUTO country policy and refresh channel caches in the WPA eloop.
 * @param ap_country Connected AP country, or NULL to restore on disconnect.
 * @return 0 on success, nonzero on configuration or cache refresh failure.
 * @note Synchronous; call from outside the WPA eloop.
 */
int wpas_ctrl_update_country_on_sta_event(const wifi_country_info_t *ap_country);

/**
 * @brief Get WiFi country information
 * @param info Pointer to output buffer for country information
 * @return 0 on success, -1 on failure
 */
int wpas_get_country_info(wifi_country_info_t *info);

/**
 * @brief Set WPA log level
 * @param level Log level
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_system_set_log_level(int level);

/* ============================================================================
 * Interface Control
 * ============================================================================ */

/**
 * @brief Dump WPA status to console
 * @param vif_id Virtual interface ID
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_iface_dump_status(unsigned char vif_id);

/**
 * @brief Update interface MAC address
 * @param vif_id Virtual interface ID
 * @param mac MAC address
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_iface_update_mac(unsigned char vif_id, unsigned char * mac);

/**
 * @brief Dump configured network list to console (for debugging)
 * @param vif_id Virtual interface ID
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_iface_dump_list_networks(unsigned char vif_id);

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

/**
 * @brief Configure network (STA/AP)
 * @param vif_id Virtual interface ID
 * @param config WPA network configuration structure
 * @return sys_err_t Execution result
 *
 * Note: Upper layer system_wifi.c needs to convert wifi_sta_config_t to wpa_network_config_t
 */
sys_err_t wpas_ctrl_network_start(unsigned char vif_id, const wpa_network_config_t *config);

/**
 * @brief Stop network (remove all configurations)
 * @param vif_id Virtual interface ID
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_network_stop(unsigned char vif_id);

/**
 * @brief Disconnect from current network while keeping configuration
 * @param vif_id Virtual interface ID
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_network_disconnect(unsigned char vif_id);

/**
 * @brief Reconnect to previously configured network
 * @param vif_id Virtual interface ID
 * @param config If non-NULL, compare with current network config before reconnecting.
 *               If NULL, reconnect directly without comparison.
 * @return SYS_OK on reconnect initiated, error code on mismatch or failure
 */
sys_err_t wpas_ctrl_network_reconnect(unsigned char vif_id,
                                       const wpa_network_config_t *config);

/**
 * @brief Check whether current network has a matching PMKSA cache entry
 * @param vif_id Virtual interface ID
 * @param config If non-NULL, compare with current network config before checking.
 * @return SYS_OK when a matching PMKSA exists, error code otherwise
 */
sys_err_t wpas_ctrl_network_has_pmksa(unsigned char vif_id,
                                       const wpa_network_config_t *config);

/* Copy per-band selection results and current hints; check connected before use. */
sys_err_t wpas_ctrl_network_get_scan_result(unsigned char vif_id,
                        struct wpas_network_scan_result *result);
/* Update only the indicated live network; never starts/restarts a scan. */
sys_err_t wpas_ctrl_network_update_scan_hint(unsigned char vif_id,
                        const wpas_network_scan_hint_t *hint);

typedef struct {
    unsigned char bssid[WPAS_CTRL_BSSID_LEN];
    unsigned char ssid[WPAS_CTRL_SSID_MAX_LEN];
    unsigned char ssid_len;
    int freq;
    int rssi;
} wpas_ctrl_selected_bss_info_t;

typedef void (*wpas_ctrl_selected_bss_cb_t)(
    const wpas_ctrl_selected_bss_info_t *info, void *ctx);

/**
 * @brief Register selected-BSS notification callback.
 *
 * The callback is invoked after scan result selection and before association.
 * Upper layers can keep the registration active and implement their own
 * per-connection one-shot policy.
 */
sys_err_t wpas_ctrl_sta_register_selected_bss_cb(wpas_ctrl_selected_bss_cb_t cb,
                                                 void *ctx);

/**
 * @brief Unregister selected-BSS notification callback.
 */
sys_err_t wpas_ctrl_sta_unregister_selected_bss_cb(void);

void wpas_ctrl_sta_notify_selected_bss(const unsigned char *bssid,
                                       const unsigned char *ssid,
                                       unsigned char ssid_len,
                                       int freq,
                                       int rssi);

/**
 * @brief Clear cached PMK state for the specified interface
 * @param vif_id Virtual interface ID
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t wpas_ctrl_pmk_cache_clean(unsigned char vif_id);

/* ============================================================================
 * Scan Operations
 * ============================================================================ */

/**
 * @brief Configure and start scan
 * @param vif_id Virtual interface ID
 * @param config WPA scan configuration
 * @return sys_err_t Execution result
 *
 * Note: Upper layer system_wifi.c needs to convert wifi_scan_config_t to wpa_scan_config_t
 */
sys_err_t wpas_ctrl_scan_start_checked(uint8_t vif_id, const wpa_scan_config_t *config);
sys_err_t wpas_ctrl_scan_start(unsigned char vif_id, const wpa_scan_config_t *config);

/**
 * @brief Stop ongoing or pending scan
 * @param vif_id Virtual interface ID
 * @return sys_err_t Execution result
 * @retval SYS_OK Success
 * @retval SYS_ERR_INVALID_ARG if vif_id invalid
 * @retval SYS_ERR_INVALID_STATE if WPA supplicant not initialized
 * @retval SYS_ERR if abort operation failed
 */
sys_err_t wpas_ctrl_scan_stop(unsigned char vif_id);

/**
 * @brief Get scan result count
 * @param vif_id Virtual interface ID
 * @return int Scan result count
 */
int wpas_ctrl_scan_get_count(unsigned char vif_id);

/**
 * @brief Get scan result by index (runtime scan data)
 * @param vif_id Virtual interface ID
 * @param index Index
 * @param info Output: WPA scan result information (wpa_wifi_info_t*) containing:
 *             - Scan result: ssid, bssid, channel, rssi
 *             - Security info: auth, cipher
 * @return sys_err_t Execution result
 *
 * Note: Returns wpa_wifi_info_t (runtime scan data with rssi).
 *       This represents the current state of scanned APs at the time of scan.
 */
sys_err_t wpas_ctrl_scan_get_result_by_index(unsigned char vif_id, unsigned int index,
                                     wpa_wifi_info_t *info);

/**
 * @brief Get scan result by SSID (runtime scan data)
 * @param vif_id Virtual interface ID
 * @param ssid SSID string
 * @param info Output: WPA scan result information (wpa_wifi_info_t*) containing:
 *             - Scan result: ssid, bssid, channel, rssi
 *             - Security info: auth, cipher
 * @return sys_err_t Execution result
 *
 * Note: Returns wpa_wifi_info_t (runtime scan data with rssi).
 *       This represents the current state of the specified AP at the time of scan.
 */
sys_err_t wpas_ctrl_scan_get_result_by_ssid(unsigned char vif_id, const char *ssid,
                                      wpa_wifi_info_t *info);

/**
 * @brief Set scan result expiration age
 * @param vif_id Virtual interface ID
 * @param age Expiration age in seconds (minimum 10 seconds)
 * @return sys_err_t Execution result
 * @retval SYS_OK Success
 * @retval SYS_ERR_INVALID_ARG if vif_id invalid or age < 10
 * @retval SYS_ERR_INVALID_STATE if WPA supplicant not initialized
 * @retval SYS_ERR if WPA internal operation failed
 */
sys_err_t wpas_ctrl_scan_set_expire(unsigned char vif_id, unsigned int age);

/**
 * @brief Flush BSS scan results
 * @param vif_id Virtual interface ID
 * @param flush_age Age in seconds (0 = flush all, >0 = flush entries older than this)
 * @return sys_err_t Execution result
 * @retval SYS_OK Success
 * @retval SYS_ERR_INVALID_ARG if vif_id invalid
 * @retval SYS_ERR_INVALID_STATE if WPA supplicant not initialized
 * @retval SYS_ERR if flush operation failed
 */
sys_err_t wpas_ctrl_scan_flush_bss(unsigned char vif_id, int flush_age);

/* ============================================================================
 * Station Information
 * ============================================================================ */

/**
 * @brief Get WiFi connection runtime state
 * @param vif_id Virtual interface ID
 * @param info Output: WPA WiFi runtime information (wpa_wifi_info_t*) containing:
 *             - Connection state: ssid, bssid, channel, rssi
 *             - Security info: auth, cipher, pwd
 * @return sys_err_t Execution result
 *
 * Note: Returns wpa_wifi_info_t (runtime state with rssi) not wpa_network_config_t (configuration).
 *       Use wpas_ctrl_softap_get_config() for AP configuration parameters.
 */
sys_err_t wpas_ctrl_station_get_info(unsigned char vif_id, wpa_wifi_info_t *info);
sys_err_t wpas_ctrl_station_get_ap_country_info(unsigned char vif_id, wifi_country_info_t *info);

/**
 * @brief Send 802.11k RRM Neighbor Report Request
 * @param vif_id Virtual interface ID (STA)
 * @param ssid Optional SSID filter, NULL for current ESS
 * @param ssid_len SSID length when ssid is not NULL
 * @return sys_err_t Execution result
 * @retval SYS_ERR_NOT_SUPPORTED if CONFIG_WPA_KVR_SUPPORT is disabled
 */
sys_err_t wpas_ctrl_rrm_neighbor_req(unsigned char vif_id,
                                     const unsigned char *ssid,
                                     unsigned int ssid_len);

/**
 * @brief Send 802.11v WNM BSS Transition Management Query
 * @param vif_id Virtual interface ID (STA)
 * @param reason BSS transition query reason code
 * @param cand_list Whether to request candidate list
 * @return sys_err_t Execution result
 * @retval SYS_ERR_NOT_SUPPORTED if CONFIG_WPA_KVR_SUPPORT is disabled
 */
sys_err_t wpas_ctrl_wnm_bss_query(unsigned char vif_id,
                                  unsigned char reason,
                                  int cand_list);

/**
 * @brief Roam STA to a target BSSID
 * @param vif_id Virtual interface ID (STA)
 * @param bssid Target BSSID (6 bytes)
 * @return sys_err_t Execution result
 * @retval SYS_ERR_NOT_SUPPORTED if CONFIG_WPA_KVR_SUPPORT is disabled
 */
sys_err_t wpas_ctrl_sta_roam(unsigned char vif_id,
                             const unsigned char *bssid);

/* ============================================================================
 * Station PCI Mode (AT command specific)
 * ============================================================================ */

/**
 * @brief Set PCI mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pci_enable PCI mode: 0=allow OPEN/WEP, 1=reject OPEN/WEP
 * @return sys_err_t Execution result
 *
 * @note PCI (PCI Certification) mode is an AT command specific feature.
 *       When enabled, the station will reject connection attempts to
 *       insecure networks (OPEN and WEP).
 */
sys_err_t wpas_ctrl_sta_set_pci_mode(uint8_t vif_id, int pci_enable);

/**
 * @brief Get PCI mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pci_enable Output: current PCI mode
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_sta_get_pci_mode(uint8_t vif_id, int *pci_enable);

/* ============================================================================
 * Station Authentication Mode Threshold
 * ============================================================================ */

/**
 * @brief Set the weakest authentication mode accepted by station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param authmode_threshold Weakest accepted authentication mode
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_sta_set_authmode_threshold(
    uint8_t vif_id, wpa_auth_mode_e authmode_threshold);

/* ============================================================================
 * Station PMF Mode (AT command specific)
 * ============================================================================ */

/**
 * @brief Set PMF (Protected Management Frames) mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pmf_mode PMF mode: 0=disabled, 1=optional/capable, 2=required
 * @return sys_err_t Execution result
 *
 * @note PMF (Protected Management Frames / IEEE 802.11w) is an AT command specific feature.
 *       - 0: Disable PMF, allow connections to all APs
 *       - 1: Enable PMF capability, prefer PMF but allow non-PMF APs
 *       - 2: Require PMF, reject connections to non-PMF APs
 */
sys_err_t wpas_ctrl_sta_set_pmf(uint8_t vif_id, int pmf_mode);

/**
 * @brief Get PMF mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pmf_mode Output: current PMF mode (0/1/2)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_sta_get_pmf(uint8_t vif_id, int *pmf_mode);

/* ============================================================================
 * SoftAP Management
 * ============================================================================ */

/**
 * @brief Get AP configuration (SSID/password/channel/hidden_ssid/beacon_int/max_connect)
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param info Output: WPA network configuration (wpa_network_config_t*) containing:
 *             - Basic info: ssid, password, channel
 *             - AP-specific config: hidden_ssid, beacon_int, max_connect, mode
 *             - Authentication: auth_mode, pairwise, group
 * @return sys_err_t Execution result
 *
 * Note: Returns wpa_network_config_t (configuration parameters) not wpa_wifi_info_t (runtime state).
 *       This structure includes AP-specific configuration fields not present in wpa_wifi_info_t.
 */
sys_err_t wpas_ctrl_softap_get_config(unsigned char vif_id, wpa_network_config_t *info);

/**
 * @brief Set maximum STA connection count
 * @param vif_id Virtual interface ID
 * @param max_sta Maximum STA count
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_set_max_sta(unsigned short max_sta);

/**
 * @brief Get SoftAP beacon interval
 * @return unsigned int Beacon interval in milliseconds
 */
unsigned int wpas_ctrl_softap_get_beacon_interval(void);

/**
 * @brief Set SoftAP beacon vendor specific IE (runtime)
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param data Vendor IE payload (may be NULL to clear)
 * @param len Vendor IE payload length
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_set_vendor_elements(unsigned char vif_id,
                                               const unsigned char *data,
                                               unsigned int len);

/**
 * @brief Set SoftAP beacon interval (runtime)
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param interval Beacon interval in ms
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_set_beacon_interval(unsigned char vif_id,
                                               unsigned int interval);

/**
 * @brief Deauthenticate STA
 * @param vif_id Virtual interface ID
 * @param sta_mac STA MAC address
 * @param reason Reason code
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_deauth_sta(unsigned char vif_id, const unsigned char *sta_mac, unsigned short reason);

/**
 * @brief Configure Channel Switch Announcement (CSA)
 * @param vif_id Virtual interface ID
 * @param config CSA configuration parameters
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_config_csa(unsigned char vif_id, const wpa_csa_config_t *config);

/**
 * @brief Get connected STA list
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param info Output: STA information array (wpa_softap_sta_info_t*) containing:
 *             - Only MAC address: mac_addr[6]
 * @param num Input/Output: array size / actual STA count
 * @return sys_err_t Execution result
 *
 * Note: Returns wpa_softap_sta_info_t (minimal STA info - only MAC addresses).
 *       Unlike wpa_wifi_info_t (with rssi) or wpa_network_config_t (with configuration),
 *       this type contains only essential MAC address information.
 */
sys_err_t wpas_ctrl_softap_get_sta_list(unsigned char vif_id, wpa_softap_sta_info_t *info,
                                  unsigned int *num);

/* ============================================================================
 * STA Connection Policy
 * ============================================================================ */

/**
 * @brief Set STA reconnection policy
 * @param vif_id Virtual interface ID
 * @param interval_sec Reconnection interval in seconds
 * @param repeat_count Maximum reconnection attempts
 *
 * Configure automatic reconnection behavior when STA loses connection.
 *
 * Parameters:
 * - interval_sec: Time to wait between each reconnection attempt
 *   - 0 with repeat_count=0: Disable automatic reconnect
 *   - 0 with repeat_count>0: Use system default delay
 *   - >0: Use specified interval in seconds
 *
 * - repeat_count: Maximum number of reconnection attempts
 *   - 0 with interval_sec>0: Unlimited reconnection
 *   - 0 with interval_sec=0: Disable automatic reconnect
 *   - >0: Stop after this many attempts
 *
 * Each attempt counts from scan phase through connection phase.
 * Both "scan failure" and "connection failure" count as one attempt.
 *
 * Counter is reset when connection completes successfully.
 *
 * @return 0 on success, -1 on failure
 */
int wpas_ctrl_sta_set_connect_policy(unsigned char vif_id,
                                      int interval_sec,
                                      int repeat_count);

/**
 * @brief Get current STA reconnection policy
 * @param vif_id Virtual interface ID
 * @param interval_sec Output parameter for reconnection interval (seconds)
 * @param repeat_count Output parameter for maximum reconnection attempts
 *
 * Retrieve the currently configured reconnection policy.
 *
 * @return 0 on success, -1 on failure
 */
int wpas_ctrl_sta_get_connect_policy(unsigned char vif_id,
                                      int *interval_sec,
                                      int *repeat_count);

/* ============================================================================
 * Station Disconnect Reason
 * ============================================================================ */

/**
 * @brief Get disconnect reason (simplified enum)
 * @return wifi_discon_reason_e Simplified disconnect reason
 */
uint16_t wpas_ctrl_sta_get_disconnect_reason(void);

/**
 * @brief Get disconnect reason code (raw IEEE 802.11 code)
 * @return uint16_t Raw IEEE 802.11 reason code
 */
uint16_t wpas_ctrl_sta_get_disconnect_reason_code(void);

/* ============================================================================
 * Hardware Feature Management
 * ============================================================================ */

/**
 * @brief Refresh hardware feature cache for specific VIF
 * @param vif_id Virtual interface ID (0=STA, 1=AP)
 *
 * This function updates the cached hardware feature data (wpa_s->hw.modes)
 * after runtime configuration changes (band_mode, protocol_bitmap, bandwidth).
 * Must be called when user changes protocol configuration via wifi_set_protocol.
 *
 * Purpose:
 * - Hardware feature data (hw_modes) is cached during initialization
 * - Runtime protocol changes don't automatically update the cache
 * - This function forces re-reading protocol config and updating cache
 * - Changes take effect on next AP startup
 *
 * Call sequence:
 * 1. User: wifi_set_protocol 1 0x7 (set AP to BGN)
 * 2. system_wifi.c: Updates protocol_bitmap[1] in fhost layer
 * 3. system_wifi.c: Calls wpas_ctrl_refresh_hw_feature(1)
 * 4. This function: Re-reads protocol_bitmap and updates wpa_s->hw.modes
 * 5. Next AP start: Uses updated hw_modes with new protocol config
 *
 * @return 0 on success, negative on error
 */
int wpas_ctrl_refresh_hw_feature(unsigned char vif_id);

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize WPA supplicant control layer
 *
 * This function initializes the WPA supplicant, including creating the
 * WPA supplicant task for both STA and AP interfaces. It should be called
 * during WiFi initialization (from wifi_config.c).
 *
 * The WPA task will:
 * - Initialize WPA supplicant global context
 * - Create virtual interfaces (wl0, wl1)
 * - Enter the event loop to process WPA events
 *
 * @return 0 on success, -1 on failure (e.g., task already created)
 */
int wpas_ctrl_init(void);

/* ============================================================================
 * L2 Packet Interface
 * ============================================================================ */

/**
 * l2_tx_callback_t - Callback type for L2 packet transmission
 * @vif_idx: Virtual interface index
 * @payload: Packet payload (without ethernet header)
 * @payload_len: Payload length
 * @ethertype: Ethernet type (e.g., 0x888e for EAPOL)
 * @dst_mac: Destination MAC address
 * @src_mac: Source MAC address
 * Returns: 0 on success, -1 on failure
 */
typedef int (*l2_tx_callback_t)(int vif_idx,
                                const uint8_t *payload,
                                int payload_len,
                                uint16_t ethertype,
                                const uint8_t *dst_mac,
                                const uint8_t *src_mac);

/**
 * wpas_l2_register_tx_callback - Register TX callback for L2 packet sending
 * @callback: Callback function to handle L2 packet transmission
 *
 * This function registers a callback that will be called when l2_packet_send()
 * is invoked. The callback receives payload and metadata, allowing upper layer
 * to construct ethernet frame.
 */
void wpas_l2_register_tx_callback(l2_tx_callback_t callback);

/**
 * wpas_l2_receive_packet - Receive and process L2 packet (EAPOL/WAPI)
 * @vif_idx: Virtual interface index
 * @packet: Packet buffer (including Ethernet header)
 * @len: Packet length
 * Returns: 0 on success (packet consumed), negative on error
 *
 * This function is called from the network stack RX path to filter and
 * process EAPOL/WAPI frames. It is the unified entry point for ECR6600 platform.
 */
int wpas_l2_receive_packet(int vif_idx, void *packet, int len);

/* Additive checked query: distinguish empty scan results from native failure.
 * Legacy count API keeps its original zero-on-error behavior. */
sys_err_t wpas_ctrl_scan_get_count_checked(uint8_t vif_id, unsigned int *count);

#endif
