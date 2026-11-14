/**
 * @file system_wifi.c
 * @brief WiFi System Control Layer Implementation
 *
 * This module implements the WiFi system control layer, which sits between
 * the application layer and the WPA supplicant layer. It provides:
 * - WiFi connection management (STA/AP modes)
 * - Network configuration and persistence
 * - Scan operations
 * - Event handling and state management
 * - Type conversion between application and WPA types
 *
 * Architecture:
 * - Application Layer → system_wifi.h (public API)
 * - System WiFi Layer → system_wifi.c (THIS FILE)
 * - WPA Supplicant Layer → wpas_cntrl_api.h
 * - PS/Driver Layer → eswin_fhost_cntrl.h
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2018-2025 Eswin Computing. All rights reserved.
 */

/****************************************************************************
 *                                Include Files
 ****************************************************************************/
#define WIFI_CTRL_INTERNAL  /**< Enable access to wifi_sniffer.h internal APIs */

#include <string.h>
#include "system_wifi.h"
#include "system_network.h"
#include "system_event_internal.h"
#include "lwip/netif.h"
#include "system_log.h"
#include "system_config.h"
#include "format_conversion.h"
#include "reset.h"
#include "system_lwip.h"
#include "eswin_fhost_cntrl.h"
#include "wpas_cntrl_api.h"
#if defined(CONFIG_NV) && defined(CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT)
#include "gen_cert_header.h"
#include "system_wifi_eap_tls_cred.h"
#if defined(EAP_TLS_TEST_CRED_HAS_WPA2) || \
    defined(EAP_TLS_TEST_CRED_HAS_WPA3_ENT) || \
    defined(EAP_TLS_TEST_CRED_HAS_WPA3_ENT_192)
#define EAP_TLS_TEST_CRED_HAS_ANY 1
#endif
#endif
#include "wifi_sniffer.h"
#include "wifi_regulatory_config.h"

static wifi_selected_bss_cb_t s_selected_bss_cb = NULL;
static void *s_selected_bss_cb_ctx = NULL;

/****************************************************************************
 *                                Local Macros
 ****************************************************************************/
#define WIFI_PASSWD_LEN 64                  /**< WiFi password maximum length */
#define WIFI_CMD_LEN 128                    /**< WiFi command buffer length */
#define CHIP_TYPE_ADDR_EFUSE 4              /**< Chip type address in EFUSE */
#define MAC_OTP_ADDR_GD25Q80E_FLASH 0x1030  /**< MAC OTP address in flash */
#define read_mac_valid 0x0                  /**< MAC validity flag */
#define SCAN_DELAY_TIMEMS (1500)            /**< Scan delay time in milliseconds */
#define WEP40_PASSWD_LEN 5                    /**< WEP-40 ASCII password length */
#define WEP40_HEX_LEN 10                      /**< WEP-40 HEX password length */
#define WEP104_PASSWD_LEN 13                  /**< WEP-104 ASCII password length */
#define WEP104_HEX_LEN 26                     /**< WEP-104 HEX password length */
#define WPA_PASSWD_LEN_MIN 8                  /**< WPA passphrase minimum length */
#define WIFI_VIF_CFG_MAX  (WIFI_VIF_AP + 1) /**< Configurable VIF count (STA + AP) */
#define WIFI_STA_DEFAULT_EAP_IDENTITY "6700_iot"

/****************************************************************************
 *                         Internal Type Definitions
 ****************************************************************************/

/**
 * @brief WiFi virtual interface state
 *
 * Stores the WiFi-related state for each virtual interface (STA/AP).
 * Network-related states are managed by system_network module.
 */
typedef struct {
    wifi_status_e status;       /**< WiFi connection status */
    unsigned char channel;      /**< Operating WiFi channel */
    unsigned char bssid[6];     /**< Connected AP BSSID (STA mode) or own BSSID (AP mode) */
    signed char rssi;           /**< Signal strength (RSSI) */
} sys_wifi_vif_state_t;

/**
 * @brief WiFi policy configuration
 *
 * Controls WiFi system behavior policies.
 */
typedef struct {
    bool auto_save_nv;          /**< Auto-save NV configuration on STA connect */
    bool ap_follow_sta;         /**< AP follows STA channel (formerly softap_follow_sta_enabled) */
} sys_wifi_policy_t;

/**
 * @brief WiFi capability configuration (internal use only)
 *
 * Controls WiFi band, bandwidth, and protocol capabilities.
 * Configuration takes effect on next connection.
 */
typedef struct {
    wifi_band_mode_e band_mode;                  /**< Global band mode (AUTO/2G_ONLY/5G_ONLY) */
    wifi_bandwidth_t bandwidth[WIFI_VIF_CFG_MAX];   /**< Bandwidth per VIF [STA, AP], each contains 2.4G/5G config */
    wifi_protocol_t protocol_bitmap[WIFI_VIF_CFG_MAX];  /**< Protocol bitmap per VIF [STA, AP], each contains 2.4G/5G config */
} wifi_capability_config_t;

/**
 * @brief WiFi system context
 *
 * Central state structure replacing scattered global variables.
 * Manages all WiFi system states including policy, mode, interface states,
 * and scan synchronization.
 */
typedef struct {
    sys_wifi_policy_t policy;                       /**< Policy configuration */
    wifi_work_mode_e mode;                          /**< Work mode (STA/AP/STA+AP) */
    sys_wifi_vif_state_t interfaces[WIFI_VIF_MAX];  /**< WiFi interface states */
    wifi_nv_info_t nv_info;                         /**< NV storage information */
    bool sta_eap_nv_dirty;                          /**< Whether pending EAP NV config must be saved */
    bool init_done;                                 /**< Initialization complete flag */

    /* Scan synchronization (merged from sync_scan_db_t) */
    void *scan_sem;                                 /**< Scan completion semaphore */
    bool scan_waiting;                              /**< Scan waiting flag */

    /* WiFi capability configuration */
    wifi_capability_config_t capability_config;     /**< Band/bandwidth/protocol configuration */

    /* Late initialization state (for delayed driver sync) */
    bool late_init_done;                            /**< Whether late init (driver sync) completed */
    bool early_init_done;                           /**< Whether early init (sys_wifi_init) completed, prevents duplicate calls */
} sys_wifi_context_t;

/****************************************************************************
 *                         Internal Global Variables
 ****************************************************************************/

/**
 * @brief WiFi system context (replaces network_db and scattered globals)
 */
static sys_wifi_context_t s_wifi_ctx = {
    .policy = {
        .auto_save_nv = true,
        .ap_follow_sta = false
    },
    .mode = WIFI_MODE_STA,
    .init_done = false,
    .scan_sem = NULL,
    .scan_waiting = false,
    .capability_config = {
        .band_mode = WIFI_BAND_MODE_AUTO,
        .bandwidth = {
            WIFI_MAKE_BANDWIDTH(WIFI_BW_AUTO, WIFI_BW_AUTO),  /* STA: 2.4G=AUTO, 5G=AUTO */
            WIFI_MAKE_BANDWIDTH(WIFI_BW_AUTO, WIFI_BW_AUTO)   /* AP:  2.4G=AUTO, 5G=AUTO */
        },
        .protocol_bitmap = {
            WIFI_MAKE_PROTOCOL(WIFI_PROTOCOL_2G_BGNAX, WIFI_PROTOCOL_5G_ANACAX),  /* STA: full protocols */
            WIFI_MAKE_PROTOCOL(WIFI_PROTOCOL_2G_BGNAX, WIFI_PROTOCOL_5G_ANACAX)   /* AP:  full protocols */
        }
    }
};

/** @brief Forward declaration: Update NV with STA connection info */
static sys_err_t nv_update_sta_connected(void);

/** @brief Forward declaration: Set country info (internal implementation with auto-save control) */
static int set_country_info_internal(const wifi_country_info_t *info, bool auto_save);

#if defined(CONFIG_NV) && defined(CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT) && defined(EAP_TLS_TEST_CRED_HAS_ANY)
static sys_err_t sys_wifi_eap_tls_test_cred_provision_one(wifi_eap_tls_cred_profile_t profile,
        const unsigned char *ca_cert, size_t ca_cert_len,
        const char *client_cert, size_t client_cert_len,
        const char *private_key, size_t private_key_len,
        const char *private_key_passwd, size_t private_key_passwd_len)
{
    sys_err_t ret;
    bool encrypted_key;

    encrypted_key = strcmp(wifi_eap_tls_cred_private_key_format((const uint8_t *)private_key,
                                                                private_key_len),
                           "PEM_ENCRYPTED_PKCS8") == 0;

    if (encrypted_key &&
        (!private_key_passwd || !private_key_passwd_len)) {
        SYS_LOGE("EAP-TLS test credential profile=%s encrypted key missing password",
                 wifi_eap_tls_cred_profile_name(profile));
        return SYS_ERR_INVALID_ARG;
    }

    if (wifi_eap_tls_cred_is_complete_by_profile(profile)) {
        if (encrypted_key) {
            ret = wifi_eap_tls_cred_get_private_key_passwd_len_by_profile(profile,
                                                                          &private_key_passwd_len);
            if (ret == SYS_OK) {
                SYS_LOGI("EAP-TLS test credential provision skip profile=%s: MFG complete passwd_present=1",
                         wifi_eap_tls_cred_profile_name(profile));
                return SYS_OK;
            }

            ret = wifi_eap_tls_cred_set_private_key_passwd_by_profile(profile,
                                                                      private_key_passwd,
                                                                      private_key_passwd_len);
            SYS_LOGI("EAP-TLS test credential profile=%s MFG complete password provision ret=0x%x",
                     wifi_eap_tls_cred_profile_name(profile), (unsigned int)ret);
            return ret;
        }

        wifi_eap_tls_cred_delete_private_key_passwd_by_profile(profile);
        SYS_LOGI("EAP-TLS test credential provision skip profile=%s: MFG complete",
                 wifi_eap_tls_cred_profile_name(profile));
        return SYS_OK;
    }

    wifi_eap_tls_cred_set_current_profile(profile);

    ret = wifi_eap_tls_cred_set_by_profile(profile,
                                           WIFI_EAP_TLS_CRED_CA_CERT,
                                           ca_cert, ca_cert_len);
    if (ret != SYS_OK) {
        return ret;
    }

    ret = wifi_eap_tls_cred_set_by_profile(profile,
                                           WIFI_EAP_TLS_CRED_CLIENT_CERT,
                                           (const uint8_t *)client_cert, client_cert_len);
    if (ret != SYS_OK) {
        return ret;
    }

    ret = wifi_eap_tls_cred_set_by_profile(profile,
                                           WIFI_EAP_TLS_CRED_PRIVATE_KEY,
                                           (const uint8_t *)private_key, private_key_len);
    if (ret != SYS_OK) {
        return ret;
    }

    if (encrypted_key) {
        ret = wifi_eap_tls_cred_set_private_key_passwd_by_profile(profile,
                                                                  private_key_passwd,
                                                                  private_key_passwd_len);
    } else {
        wifi_eap_tls_cred_delete_private_key_passwd_by_profile(profile);
    }

    return ret;
}

static sys_err_t sys_wifi_eap_tls_test_cred_provision(void)
{
    sys_err_t ret = SYS_OK;

#ifdef EAP_TLS_TEST_CRED_HAS_WPA2
    ret = sys_wifi_eap_tls_test_cred_provision_one(
            WIFI_EAP_TLS_CRED_PROFILE_WPA2,
            eap_tls_wpa2_ca_cert_pem,       sizeof(eap_tls_wpa2_ca_cert_pem) - 1,
            eap_tls_wpa2_client_cert_pem,   sizeof(eap_tls_wpa2_client_cert_pem) - 1,
            eap_tls_wpa2_private_key_pem,   sizeof(eap_tls_wpa2_private_key_pem) - 1,
            eap_tls_test_cred_default_private_key_passwd,
            sizeof(eap_tls_test_cred_default_private_key_passwd) - 1);
    if (ret != SYS_OK) {
        return ret;
    }
#endif

#ifdef EAP_TLS_TEST_CRED_HAS_WPA3_ENT
    ret = sys_wifi_eap_tls_test_cred_provision_one(
            WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT,
            eap_tls_wpa3_ent_ca_cert_pem,       sizeof(eap_tls_wpa3_ent_ca_cert_pem) - 1,
            eap_tls_wpa3_ent_client_cert_pem,   sizeof(eap_tls_wpa3_ent_client_cert_pem) - 1,
            eap_tls_wpa3_ent_private_key_pem,   sizeof(eap_tls_wpa3_ent_private_key_pem) - 1,
            eap_tls_test_cred_default_private_key_passwd,
            sizeof(eap_tls_test_cred_default_private_key_passwd) - 1);
    if (ret != SYS_OK) {
        return ret;
    }
#endif

#ifdef EAP_TLS_TEST_CRED_HAS_WPA3_ENT_192
    ret = sys_wifi_eap_tls_test_cred_provision_one(
            WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT_192,
            eap_tls_wpa3_ent_192_ca_cert_pem,       sizeof(eap_tls_wpa3_ent_192_ca_cert_pem) - 1,
            eap_tls_wpa3_ent_192_client_cert_pem,   sizeof(eap_tls_wpa3_ent_192_client_cert_pem) - 1,
            eap_tls_wpa3_ent_192_private_key_pem,   sizeof(eap_tls_wpa3_ent_192_private_key_pem) - 1,
            eap_tls_test_cred_default_private_key_passwd,
            sizeof(eap_tls_test_cred_default_private_key_passwd) - 1);
    if (ret != SYS_OK) {
        return ret;
    }
#endif

    return SYS_OK;
}
#endif

/****************************************************************************
 *                            Function Definitions
 ****************************************************************************/

/****************************************************************************
 *                         WiFi Context Management
 ****************************************************************************/

/**
 * @brief Get WiFi system context
 *
 * @return Pointer to WiFi system context
 */
static sys_wifi_context_t* get_context(void)
{
    return &s_wifi_ctx;
}

/**
 * @brief Validate channel compatibility with band mode configuration
 *
 * @param channel   Channel number to validate (0 = auto/all channels, allow any band)
 * @param band_mode Current band mode configuration
 * @return true if channel is compatible with band_mode, false otherwise
 *
 * @note Channel ranges: 2.4G (1-14), 5G (36-165)
 * @note AUTO mode allows all channels; 2G_ONLY rejects 5G channels; 5G_ONLY rejects 2.4G channels
 */
static bool validate_channel_band_compatibility(unsigned char channel,
                                                 wifi_band_mode_e band_mode)
{
    /* Channel 0 means auto/all channels - always valid */
    if (channel == 0) {
        return true;
    }

    /* AUTO mode allows all channels */
    if (band_mode == WIFI_BAND_MODE_AUTO) {
        return true;
    }

    /* Determine if channel is 5G (36-165) or 2.4G (1-14) */
    bool is_5g_channel = (channel >= 36);

    /* Validate 2G_ONLY mode */
    if (band_mode == WIFI_BAND_MODE_2G_ONLY && is_5g_channel) {
        SYS_LOGE("Channel %d (5G) incompatible with 2G_ONLY band mode", channel);
        return false;
    }

    /* Validate 5G_ONLY mode */
    if (band_mode == WIFI_BAND_MODE_5G_ONLY && !is_5g_channel) {
        SYS_LOGE("Channel %d (2.4G) incompatible with 5G_ONLY band mode", channel);
        return false;
    }

    return true;
}

/**
 * @brief Initialize capability config with chip-specific defaults
 *
 * Sets default band/bandwidth/protocol configuration based on chip capabilities.
 * Used when NV config is unavailable or invalid.
 *
 * @param config  Pointer to capability config structure to initialize
 */
static void init_capability_defaults(wifi_capability_config_t *config)
{
    if (!config) {
        return;
    }

#if defined(CONFIG_ECW6700)
    /* ECW6700: Dual-band (2.4G + 5G), VHT + HE support */
    config->band_mode = WIFI_BAND_MODE_AUTO;
    config->protocol_bitmap[WIFI_VIF_STA] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX  // 5G
    );
    config->protocol_bitmap[WIFI_VIF_AP] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX  // 5G
    );
#elif defined(CONFIG_ECW6280_WIFI)
    /* ECW6280: Dual-band (2.4G + 5G), HT only (no VHT/HE) */
    config->band_mode = WIFI_BAND_MODE_AUTO;
    config->protocol_bitmap[WIFI_VIF_STA] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,  // 2.4G
        WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N  // 5G
    );
    config->protocol_bitmap[WIFI_VIF_AP] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,  // 2.4G
        WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N  // 5G
    );
#elif defined(CONFIG_ECR6630)
    /* ECR6630: 2.4G only, HT + HE support (no VHT) */
    config->band_mode = WIFI_BAND_MODE_2G_ONLY;
    config->protocol_bitmap[WIFI_VIF_STA] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        0  // 5G not supported
    );
    config->protocol_bitmap[WIFI_VIF_AP] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        0  // 5G not supported
    );
#else
    /* Default fallback: 2.4G, HT + HE */
    config->band_mode = WIFI_BAND_MODE_2G_ONLY;
    config->protocol_bitmap[WIFI_VIF_STA] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        0  // 5G not supported
    );
    config->protocol_bitmap[WIFI_VIF_AP] = WIFI_MAKE_PROTOCOL(
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX,  // 2.4G
        0  // 5G not supported
    );
#endif

    /* Bandwidth defaults (both STA and AP) */
    config->bandwidth[WIFI_VIF_STA] = WIFI_MAKE_BANDWIDTH(WIFI_BW_AUTO, WIFI_BW_AUTO);
    config->bandwidth[WIFI_VIF_AP] = WIFI_MAKE_BANDWIDTH(WIFI_BW_AUTO, WIFI_BW_AUTO);
}

/**
 * @brief Load capability config from NV to context (no driver sync)
 *
 * Reads capability configuration from NV and stores in context.
 * Does NOT call fhost_set_*() - safe for early init.
 *
 * @return SYS_OK if loaded successfully, SYS_ERR if using defaults
 */
static sys_err_t load_capability_from_nv_to_cache(void)
{
    sys_wifi_context_t *ctx = get_context();
    int ret;
    wifi_capability_config_t loaded_config;

    /* Read from NV (safe - just Flash read) */
    ret = hal_system_get_config(CUSTOMER_NV_WIFI_CAPABILITY_CONFIG,
                                 &loaded_config,
                                 sizeof(wifi_capability_config_t));

    if (ret > 0 && ret != 0xffffffff) {
        /* NV read success, use loaded config */
        ctx->capability_config = loaded_config;

        /* Validate and correct AP bandwidth (software limitation: SoftAP only supports 20MHz or AUTO) */
        wifi_bandwidth_t *ap_bw = &ctx->capability_config.bandwidth[WIFI_VIF_AP];

        if (ap_bw->ghz_2g != WIFI_BW_HT20 && ap_bw->ghz_2g != WIFI_BW_AUTO) {
            SYS_LOGW("NV contains invalid AP 2.4G bandwidth %d, correcting to WIFI_BW_AUTO", ap_bw->ghz_2g);
            ap_bw->ghz_2g = WIFI_BW_AUTO;
        }

        if (ap_bw->ghz_5g != WIFI_BW_HT20 && ap_bw->ghz_5g != WIFI_BW_AUTO) {
            SYS_LOGW("NV contains invalid AP 5G bandwidth %d, correcting to WIFI_BW_AUTO", ap_bw->ghz_5g);
            ap_bw->ghz_5g = WIFI_BW_AUTO;
        }

        SYS_LOGD("Capability loaded from NV to cache: band=%d, STA_bw(2g=%d,5g=%d), AP_bw(2g=%d,5g=%d)",
                 ctx->capability_config.band_mode,
                 ctx->capability_config.bandwidth[WIFI_VIF_STA].ghz_2g,
                 ctx->capability_config.bandwidth[WIFI_VIF_STA].ghz_5g,
                 ctx->capability_config.bandwidth[WIFI_VIF_AP].ghz_2g,
                 ctx->capability_config.bandwidth[WIFI_VIF_AP].ghz_5g);
        SYS_LOGD("Protocol bitmap: STA(2g=0x%02x,5g=0x%02x), AP(2g=0x%02x,5g=0x%02x)",
                ctx->capability_config.protocol_bitmap[WIFI_VIF_STA].ghz_2g,
                ctx->capability_config.protocol_bitmap[WIFI_VIF_STA].ghz_5g,
                ctx->capability_config.protocol_bitmap[WIFI_VIF_AP].ghz_2g,
                ctx->capability_config.protocol_bitmap[WIFI_VIF_AP].ghz_5g);
        return SYS_OK;
    } else {
        /* NV read failed or empty, use chip defaults */
        init_capability_defaults(&ctx->capability_config);
        SYS_LOGD("Using chip-specific default capability config");
        return SYS_ERR;
    }
}

/****************************************************************************
 *                     Legacy Compatibility Functions
 ****************************************************************************/

/**
 * @brief Get NV information (legacy compatibility)
 *
 * @return Pointer to NV information structure
 */
wifi_nv_info_t* sys_wifi_get_nv_info()
{
    return &s_wifi_ctx.nv_info;
}

/**
 * @brief Set WiFi operational mode
 *
 * @param[in] opmode  WiFi mode (WIFI_MODE_STA/WIFI_MODE_AP/WIFI_MODE_AP_STA)
 *
 * @note Invalid mode values are silently ignored
 * @note If mode equals current mode, no action is taken
 * @note STA/AP modes stop the excluded interface but do not start interfaces
 * @note If auto_save_nv is enabled, mode change is saved to NV
 */
void sys_wifi_set_mode(wifi_work_mode_e opmode)
{
    sys_wifi_context_t *ctx = get_context();

    if ((opmode != WIFI_MODE_STA) && (opmode != WIFI_MODE_AP) && (opmode != WIFI_MODE_AP_STA)) {
        return;
    }

    if (ctx->mode == opmode) {
        return;
    }

    ctx->mode = opmode;

    if (opmode == WIFI_MODE_STA) {
        sys_wifi_ap_stop();
    } else if (opmode == WIFI_MODE_AP) {
        sys_wifi_sta_stop();
    }

    if (ctx->policy.auto_save_nv) {
        hal_system_set_config(CUSTOMER_NV_WIFI_OP_MODE, &opmode, sizeof(opmode));
    }
}

/**
 * @brief Get current WiFi work mode
 *
 * @return WiFi work mode (STA/AP/STA+AP)
 */
wifi_work_mode_e sys_wifi_get_mode(void)
{
    return s_wifi_ctx.mode;
}

/**
 * @brief Set WiFi ready status
 *
 * @param ready true if WiFi is ready, false otherwise
 */
void sys_wifi_set_ready(bool ready)
{
    s_wifi_ctx.init_done = ready;
}

/**
 * @brief Check if WiFi is ready
 *
 * @return true if WiFi is initialized and ready, false otherwise
 */
bool sys_wifi_is_ready(void)
{
    return s_wifi_ctx.init_done;
}

/**
 * @brief Send deauthentication frame
 *
 * @param sa Source MAC address
 * @param da Destination MAC address
 * @param bssid BSSID
 */
static void send_deauth(unsigned char *sa, unsigned char *da, unsigned char *bssid)
{
    struct wifi_ieee80211_mgmt mgmt;

    memset((void*)&mgmt, 0, sizeof(struct wifi_ieee80211_mgmt));

    mgmt.frame_control = WIFI_IEEE80211_FC(WIFI_FC_TYPE_MGMT,
        WIFI_FC_STYPE_DEAUTH);
    mgmt.frame_control |= WIFI_FC_ISWEP;
    memcpy(mgmt.da, da, ETH_ALEN);
    memcpy(mgmt.sa, sa, ETH_ALEN);
    memcpy(mgmt.bssid, bssid, ETH_ALEN);
    mgmt.u.deauth.reason_code = host_to_le16(1);

    sys_net_packet_send(WIFI_VIF_STA, (u8 *)&mgmt, WIFI_IEEE80211_HDRLEN + sizeof(mgmt.u.deauth), 1, 1);
}

/**
 * @brief System reset handler - sends deauth before reset
 *
 * @param arg Unused argument
 */
static void reset_handler(void *arg)
{
    unsigned char src_mac[NETIF_MAX_HWADDR_LEN];
    sys_wifi_context_t *wifi_ctx = get_context();

    if(arch_irq_context()) {
        return;
    }

    if(sys_wifi_sta_get_status() != STA_STATUS_CONNECTED) {
        return;
    }

    if (sys_wifi_sta_get_mac(src_mac) != SYS_OK) {
        return;
    }

    send_deauth(src_mac, wifi_ctx->nv_info.sta_bssid, wifi_ctx->nv_info.sta_bssid);
    os_msleep(50);
}

/**
 * @brief Print WiFi init summary (mode, band, protocol, NV)
 */
static void wifi_print_init_summary(sys_wifi_context_t *ctx)
{
    static const char *mode_str[] = {"STA", "AP", "AP+STA"};
    static const char *band_str[] = {"2G+5G", "2G", "5G"};
    static const char *vif_str[]  = {"STA", "AP"};
    int i;

    const char *mstr = (ctx->mode < WIFI_MODE_MAX) ? mode_str[ctx->mode] : "?";
    const char *bstr = (ctx->capability_config.band_mode < WIFI_BAND_MODE_MAX) ?
        band_str[ctx->capability_config.band_mode] : "?";
    SYS_LOGI("WiFi: %s %s", mstr, bstr);

    for (i = 0; i < WIFI_VIF_CFG_MAX; i++) {
        uint16_t p2g = ctx->capability_config.protocol_bitmap[i].ghz_2g;
        uint16_t p5g = ctx->capability_config.protocol_bitmap[i].ghz_5g;
        SYS_LOGI("Wifi: %s 2G(B=%d,G=%d,N=%d,AC=%d,AX=%d) 5G(A=%d,N=%d,AC=%d,AX=%d)",
                 vif_str[i],
                 !!(p2g & 0x01), !!(p2g & 0x02), !!(p2g & 0x04), !!(p2g & 0x08), !!(p2g & 0x10),
                 !!(p5g & 0x02), !!(p5g & 0x04), !!(p5g & 0x08), !!(p5g & 0x10));
    }

    if (ctx->nv_info.sync) {
        SYS_LOGI("WiFi NV: ssid[%s],has_pwd=%d,ch=%d,auto=%d",
                 ctx->nv_info.sta_ssid, ctx->nv_info.sta_pwd[0] != '\0',
                 ctx->nv_info.channel, ctx->nv_info.auto_conn);
    }
}

/**
 * @brief Early WiFi initialization (before RTOS scheduler starts)
 *
 * Performs minimal initialization safe for pre-scheduler context:
 * - Reads configuration from NV to memory cache
 * - Does NOT sync to driver (no mutex/semaphore/thread operations)
 * - Uses chip-specific defaults if NV unavailable
 *
 * MUST call sys_wifi_init_late() after RTOS scheduler starts to sync config to driver.
 * Late init is automatically called in evt_on_wifi_ready() callback.
 *
 * @return SYS_OK on success
 *
 * @note Safe to call before RTOS scheduler starts
 * @note Driver sync happens later in sys_wifi_init_late()
 */
sys_err_t sys_wifi_init(void)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_work_mode_e wifi_mode = WIFI_MODE_AP_STA;

    /* Check for duplicate early init calls */
    if (wifi_ctx->early_init_done) {
        SYS_LOGW("sys_wifi_init already called, skipping");
        return SYS_OK;
    }

    memset(&wifi_ctx->nv_info, 0, sizeof(wifi_ctx->nv_info));
    // Network context initialization is done in system_network.c
    
    /* Initialize interface status to correct defaults.
     * Static zero-init sets status=0 (AP_STATUS_STOP) for all VIFs,
     * but STA VIF must start at STA_STATUS_STOP (enum value 2). */
    wifi_ctx->interfaces[WIFI_VIF_STA].status = STA_STATUS_STOP;
    wifi_ctx->interfaces[WIFI_VIF_AP].status  = AP_STATUS_STOP;

#ifdef CONFIG_NV
    /* ✅ SAFE: Read work mode from NV (just Flash read, no sync) */
    int len = hal_system_get_config(CUSTOMER_NV_WIFI_OP_MODE, &wifi_mode, sizeof(wifi_mode));
    if ((len == (int)sizeof(wifi_mode)) && (wifi_mode >= WIFI_MODE_STA) && (wifi_mode < WIFI_MODE_MAX)) {
        wifi_ctx->mode = wifi_mode;
        SYS_LOGD("Work mode from NV: %d", wifi_mode);
    } else
#endif
    {
        wifi_ctx->mode = WIFI_MODE_AP_STA;
        SYS_LOGD("Using default work mode: AP_STA");
    }

    /* Initialize regulatory configuration and register to PS layer */
    if (wifi_regulatory_config_init() != 0) {
        SYS_LOGE("Failed to initialize regulatory configuration");
        /* Continue with default CN fallback configuration */
    }

    /* ✅ SAFE: Load NV info (no driver interaction) */
    sys_wifi_load_nv_info(NULL);

#if defined(CONFIG_NV) && defined(CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT) && defined(EAP_TLS_TEST_CRED_HAS_ANY)
    {
        sys_err_t cred_ret = sys_wifi_eap_tls_test_cred_provision();
        if (cred_ret != SYS_OK) {
            SYS_LOGW("EAP-TLS test credential provisioning failed: 0x%x", (unsigned int)cred_ret);
        }
    }
#endif

#ifdef CONFIG_NV
    /* ✅ SAFE: Load ap_follow_sta policy from NV */
    {
        unsigned char val = 0;
        if (hal_system_get_config(CUSTOMER_NV_WIFI_AP_FOLLOW_STA, &val, sizeof(val))) {
            wifi_ctx->policy.ap_follow_sta = (val != 0);
        }
    }

    /* ✅ SAFE: Load capability to cache (no driver sync yet) */
    load_capability_from_nv_to_cache();
#else
    /* No NV support: use chip defaults */
    init_capability_defaults(&wifi_ctx->capability_config);
#endif

    /* Mark late init as pending */
    wifi_ctx->late_init_done = false;

    /* Register reset handler */
    drv_reset_system_hook_register(reset_handler, NULL);

    /* Mark early init as done to prevent duplicate calls */
    wifi_ctx->early_init_done = true;

    wifi_print_init_summary(wifi_ctx);
    return SYS_OK;
}

/**
 * @brief Late WiFi initialization (after RTOS scheduler starts)
 *
 * Syncs cached configuration from context to WiFi driver/WPA supplicant.
 * This function performs all operations requiring threads/mutexes/semaphores.
 *
 * MUST be called after:
 * - RTOS scheduler is running
 * - WiFi driver threads are started
 * - WPA event loop is running
 *
 * Automatically called in evt_on_wifi_ready() callback.
 * Idempotent - safe to call multiple times.
 *
 * @return SYS_OK on success, SYS_ERR if already initialized
 *
 * @note This function uses mutex/semaphore/queue - NOT safe before RTOS starts
 */
static sys_err_t sys_wifi_init_late(void)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_country_info_t country_info;
    int sync_errors = 0;  /* Error counter for sync operations */

    /* Idempotent check */
    if (wifi_ctx->late_init_done) {
        SYS_LOGW("WiFi late init already completed");
        return SYS_ERR;
    }

    /* Apply the runtime country selected by wifi_country_ctx_init(), without NV writes. */
    if (wpas_get_country_info(&country_info) != 0 ||
        set_country_info_internal(&country_info, false) != 0) {
        SYS_LOGE("Failed to sync runtime country configuration");
        sync_errors++;
    } else {
        SYS_LOGI("Country synced to driver: cc=%s, schan=%u, nchan=%u",
                 country_info.country_code, country_info.schan_2g, country_info.nchan_2g);
    }

#ifdef CONFIG_NV
    /* ✅ NOW SAFE: Sync capability to driver (driver threads running) */
    /*SYS_LOGI("Syncing capability to driver: band=%d, bw_sta=%d, bw_ap=%d, proto_sta=0x%02x, proto_ap=0x%02x",
             wifi_ctx->capability_config.band_mode,
             wifi_ctx->capability_config.bandwidth[WIFI_VIF_STA],
             wifi_ctx->capability_config.bandwidth[WIFI_VIF_AP],
             wifi_ctx->capability_config.protocol_bitmap[WIFI_VIF_STA],
             wifi_ctx->capability_config.protocol_bitmap[WIFI_VIF_AP]);*/

    if (fhost_set_band_mode(wifi_ctx->capability_config.band_mode) != 0) {
        SYS_LOGE("Failed to sync band_mode to driver");
        sync_errors++;
    }

    if (fhost_set_bandwidth(WIFI_VIF_STA, wifi_ctx->capability_config.bandwidth[WIFI_VIF_STA]) != 0) {
        SYS_LOGE("Failed to sync STA bandwidth to driver");
        sync_errors++;
    }

    if (fhost_set_bandwidth(WIFI_VIF_AP, wifi_ctx->capability_config.bandwidth[WIFI_VIF_AP]) != 0) {
        SYS_LOGE("Failed to sync AP bandwidth to driver");
        sync_errors++;
    }

    if (fhost_set_protocol(WIFI_VIF_STA, wifi_ctx->capability_config.protocol_bitmap[WIFI_VIF_STA]) != 0) {
        SYS_LOGE("Failed to sync STA protocol to driver");
        sync_errors++;
    }

    if (fhost_set_protocol(WIFI_VIF_AP, wifi_ctx->capability_config.protocol_bitmap[WIFI_VIF_AP]) != 0) {
        SYS_LOGE("Failed to sync AP protocol to driver");
        sync_errors++;
    }

     /* Sync ap_follow_sta policy to driver */
    fhost_set_ap_channel_follow(wifi_ctx->policy.ap_follow_sta);
#endif

    if (sync_errors > 0) {
        SYS_LOGW("WiFi configuration synced with %d errors", sync_errors);
    }

    // Refresh wpa_supplicant hw_feature cache after country/protocol sync
    // This ensures AP/STA startup will use correct channel/protocol capabilities
    wpas_ctrl_refresh_hw_feature(WIFI_VIF_STA);
    wpas_ctrl_refresh_hw_feature(WIFI_VIF_AP);

    /* Mark as completed */
    wifi_ctx->late_init_done = true;

    return SYS_OK;
}

/**
 * @brief Set WiFi interface status
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @param status WiFi status to set
 */
void sys_wifi_set_status(int vif, wifi_status_e status)
{
    if (!IS_VALID_VIF(vif)) {
        return;
    }

    // Update wifi_context directly
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_ctx->interfaces[vif].status = status;
}

/**
 * @brief Get WiFi interface status
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @return WiFi status, or STA_STATUS_STOP if invalid VIF
 */
wifi_status_e sys_wifi_get_status(int vif)
{
    if (!IS_VALID_VIF(vif)) {
        return STA_STATUS_STOP;
    }

    // Read directly from wifi_context
    sys_wifi_context_t *wifi_ctx = get_context();
    return wifi_ctx->interfaces[vif].status;
}

/**
 * @brief Check if WiFi interface link is up
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @return true if link is up (STA connected or AP started), false otherwise
 */
bool sys_wifi_is_link_up(int vif)
{
    if (!IS_VALID_VIF(vif)) {
        return false;
    }

    wifi_status_e status = sys_wifi_get_status(vif);
    if ((status == STA_STATUS_CONNECTED) || (status == AP_STATUS_STARTED)) {
        return true;
    }

    return false;
}
/**
 * @brief Set MAC addresses for STA and/or SoftAP interfaces
 *
 * This function sets MAC addresses for both interfaces atomically.
 * If only one MAC is provided, the other is automatically derived.
 *
 * @param sta_mac STA interface MAC address (6 bytes), can be NULL
 * @param ap_mac  AP interface MAC address (6 bytes), can be NULL
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if both sta_mac and ap_mac are NULL
 * @retval SYS_ERR_WIFI_INVALID_MAC if both provided but not a valid pair
 *
 * @note At least one of sta_mac or ap_mac must be non-NULL
 * @note If only one MAC provided, uses HAL_SYSTEM_COMPUTE_SECONDARY_MAC to derive the other
 * @note If both provided, validates they form a valid pair using HAL_SYSTEM_IS_MAC_PAIR
 */
sys_err_t sys_wifi_set_mac(const uint8_t *sta_mac, const uint8_t *ap_mac)
{
    uint8_t sta[6], ap[6];
    int ret;

    /* At least one MAC must be provided */
    if (!sta_mac && !ap_mac) {
        return SYS_ERR_INVALID_ARG;
    }

    if (sta_mac && !ap_mac) {
        /* Only STA MAC provided, derive AP MAC */
        memcpy(sta, sta_mac, 6);
        memcpy(ap, sta_mac, 6);
        HAL_SYSTEM_COMPUTE_SECONDARY_MAC(ap);
    } else if (!sta_mac && ap_mac) {
        /* Only AP MAC provided, derive STA MAC */
        memcpy(ap, ap_mac, 6);
        memcpy(sta, ap_mac, 6);
        HAL_SYSTEM_COMPUTE_SECONDARY_MAC(sta);
    } else {
        /* Both provided, validate they form a valid pair */
        if (!HAL_SYSTEM_IS_MAC_PAIR(sta_mac, ap_mac)) {
            return SYS_ERR_WIFI_INVALID_MAC;
        }
        memcpy(sta, sta_mac, 6);
        memcpy(ap, ap_mac, 6);
    }

    /* Send both MACs to PS layer */
    if (fhost_set_mac(sta, ap) != 0) {
        return SYS_ERR;
    }

    /* Update network layer for both interfaces */
    ret = sys_netif_set_mac(WIFI_VIF_STA, sta);
    if (ret == SYS_OK) {
        /* Notify WPA layer for both interfaces */
        ret = wpas_ctrl_iface_update_mac(WIFI_VIF_STA, sta);
        if (ret != SYS_OK) {
            SYS_LOGE("Failed to update STA MAC in WPA layer",
                     " ret=%d", ret);
        }
    } else {
        SYS_LOGE("Failed to set STA MAC in network layer",
                 " ret=%d", ret);
    }

    ret = sys_netif_set_mac(WIFI_VIF_AP, ap);
    if (ret == SYS_OK) {
        ret = wpas_ctrl_iface_update_mac(WIFI_VIF_AP, ap);
        if (ret != SYS_OK) {
            SYS_LOGE("Failed to update AP MAC in WPA layer",
                     " ret=%d", ret);
        }
    } else {
        SYS_LOGE("Failed to set AP MAC in network layer",
                 " ret=%d", ret);
    }

    return SYS_OK;
}

/**
 * @brief Set MAC address for WiFi interface
 *
 * Sets the MAC address for specified interface. The other interface's MAC
 * is automatically derived using HAL_SYSTEM_COMPUTE_SECONDARY_MAC.
 * Both interfaces are updated atomically via sys_wifi_set_mac.
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @param mac MAC address (6 bytes)
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if invalid parameters
 *
 * @see sys_wifi_set_mac() for setting both interfaces explicitly
 */
sys_err_t sys_wifi_if_set_mac(wifi_vif_e vif, unsigned char *mac)
{
    if (!IS_VALID_VIF(vif) || !mac) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Delegate to sys_wifi_set_mac which handles derivation and PS layer update */
    if (vif == WIFI_VIF_STA) {
        return sys_wifi_set_mac(mac, NULL);
    } else {
        return sys_wifi_set_mac(NULL, mac);
    }
}

/**
 * @brief Get MAC address of WiFi interface
 *
 * @param vif Virtual interface ID
 * @param mac Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if invalid parameters
 */
sys_err_t sys_wifi_if_get_mac(wifi_vif_e vif, unsigned char *mac)
{
    if (!IS_VALID_VIF(vif) || !mac) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Get MAC from network layer */
    return sys_netif_get_mac(vif, mac);
}

/**
 * @brief Classify a channel within the SDK's 2.4GHz/5GHz channel ranges
 */
wifi_band_e sys_wifi_channel_to_band(unsigned int channel)
{
    if (channel >= WIFI_CHANNEL_1 && channel <= WIFI_CHANNEL_14) {
        return WIFI_BAND_2G;
    }
    if (channel >= WIFI_CHANNEL_36 && channel <= WIFI_CHANNEL_177) {
        return WIFI_BAND_5G;
    }
    return WIFI_BAND_UNKNOWN;
}

/**
 * @brief Convert WiFi channel to frequency
 *
 * @param channel Channel number (1-14 for 2.4GHz, 36-165 for 5GHz)
 * @return Frequency in MHz, or 0 if invalid channel
 */
unsigned int sys_wifi_channel_to_freq(unsigned int channel)
{
    if ((channel >= WIFI_CHANNEL_1) && (channel <= WIFI_CHANNEL_14)) {
        if (channel == WIFI_CHANNEL_14) {
            return CHANNEL_14_FREQ;
        } else {
            return CHANNEL_1_FREQ + CHANNEL_2G4_BANDWIDTH(channel);
        }
    } else if ((channel >= WIFI_CHANNEL_36) && (channel <= WIFI_CHANNEL_165)) {
        return CHANNEL_36_FREQ + CHANNEL_5G_BANDWIDTH(channel);
    }

    return 0;
}

/**
 * @brief Convert frequency to WiFi channel
 *
 * @param freq Frequency in MHz
 * @return Channel number, or 0 if invalid frequency
 */
unsigned int sys_wifi_freq_to_channel(unsigned int freq)
{
    if ((freq >= CHANNEL_1_FREQ) && (freq <= CHANNEL_14_FREQ)) {
        if (freq == CHANNEL_14_FREQ) {
            return WIFI_CHANNEL_14;
        } else {
            return WIFI_CHANNEL_1 + FREQ_TO_CHANNEL_2G4(freq);
        }
    } else if ((freq >= CHANNEL_36_FREQ) && (freq <= CHANNEL_165_FREQ)) {
        return WIFI_CHANNEL_36 + FREQ_TO_CHANNEL_5G(freq);
    }

    return 0;
}

/**
 * @brief Set sniffer monitoring channel
 *
 * Sets the WiFi channel for packet capture in sniffer mode.
 * Should be called before sys_wifi_sniffer_start() to ensure
 * monitoring on the desired channel.
 *
 * @param channel Channel number (1-14 for 2.4GHz, 36-165 for 5GHz)
 * @return SYS_OK on success, SYS_ERR on failure
 *
 * @note Channel availability depends on regulatory domain configuration
 */
sys_err_t sys_wifi_sniffer_set_channel(unsigned char  channel)
{
    return fhost_set_channel(channel) ? SYS_ERR : SYS_OK;
}

/**
 * @brief Get current WiFi channel
 *
 * @return Current channel number
 */
uint8_t sys_wifi_get_current_channel(void)
{
    uint8_t channel = 0;
    //struct wifi_channel_info phy_info;
    //phy_get_channel(&phy_info, 0);
    fhost_get_channel(&channel);
    //channel = wifi_freq_to_channel(PHY_INFO_CHAN(phy_info));

    return channel;
}

/**
 * @brief Start WiFi sniffer (promiscuous mode)
 *
 * Enables monitor mode to capture 802.11 frames. The callback function
 * receives wifi_sniffer_packet_t containing:
 * - rx_info: RSSI, channel, rate, MCS, bandwidth, etc.
 * - payload: Pointer to raw 802.11 frame data
 *
 * @param cb     Callback function invoked for each captured frame
 *               Signature: void (*cb)(void *buf, int len, wifi_sniffer_pkt_type_t type)
 * @param filter Frame filter configuration (NULL for all frames)
 *               Use filter_mask with WIFI_SNIFFER_FILTER_* flags:
 *               - WIFI_SNIFFER_FILTER_MGMT (0x01): Management frames
 *               - WIFI_SNIFFER_FILTER_CTRL (0x02): Control frames
 *               - WIFI_SNIFFER_FILTER_DATA (0x04): Data frames
 *               - WIFI_SNIFFER_FILTER_ALL (0xFFFFFFFF): All frames
 * @return SYS_OK on success, SYS_ERR_WIFI_IF_DOWN if WiFi interface is down
 *
 * @note Sniffer can run with STA/AP but may impact performance
 */
sys_err_t sys_wifi_sniffer_start(wifi_sniffer_cb_t cb, wifi_sniffer_filter_t *filter)
{
    if (filter) {
        wifi_sniffer_set_filter(filter);
    }
    wifi_sniffer_set_callback(cb);
    return wifi_sniffer_enable(true);
}

/**
 * @brief Stop WiFi sniffer (promiscuous mode)
 *
 * Disables monitor mode and cleans up:
 * - Stops hardware monitor mode
 * - Unregisters frame callback
 * - Resets filter to default (WIFI_SNIFFER_FILTER_ALL)
 *
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_sniffer_stop(void)
{
    return wifi_sniffer_enable(false);
}

/**
 * @brief Set sniffer frame filter
 *
 * Configures which frame types to capture. If sniffer is running,
 * the new filter is applied immediately.
 *
 * @param filter Filter configuration with filter_mask field
 *               Flags can be OR'ed: WIFI_SNIFFER_FILTER_MGMT | WIFI_SNIFFER_FILTER_DATA
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if filter is NULL
 */
sys_err_t sys_wifi_sniffer_set_filter(const wifi_sniffer_filter_t *filter)
{
    return wifi_sniffer_set_filter(filter);
}

/**
 * @brief Get current sniffer frame filter
 *
 * Retrieves the current filter configuration.
 *
 * @param filter Output buffer for filter configuration
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if filter is NULL
 */
sys_err_t sys_wifi_sniffer_get_filter(wifi_sniffer_filter_t *filter)
{
    return wifi_sniffer_get_filter(filter);
}

/**
 * @brief Send raw WiFi packet
 *
 * @param vif Virtual interface ID
 * @param frame Pointer to frame data
 * @param len Length of frame data
 * @return SYS_OK on success, SYS_ERR on failure
 */
sys_err_t sys_wifi_send_raw_pkt(wifi_vif_e vif, const uint8_t *frame, const uint16_t len)
{
    /* Validate VIF parameter */
    if (!IS_VALID_VIF(vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!frame || len == 0) {
        return SYS_ERR_INVALID_ARG;
    }

    return sys_net_packet_send(vif, frame, len, 1, 0) ? SYS_ERR:SYS_OK;
}

/**
 * @brief Convert WPA type to system_wifi type
 *
 * Converts WiFi information from WPA supplicant format to system_wifi format.
 *
 * @param[in]  wpa_info  WPA WiFi information
 * @param[out] wifi_info system_wifi WiFi information
 */
static void cvt_wpa_to_wifi_info(const wpa_wifi_info_t *wpa_info,
                                      wifi_info_t *wifi_info)
{
    if (!wpa_info || !wifi_info) {
        return;
    }

    memset(wifi_info, 0, sizeof(*wifi_info));

    /* Copy SSID with boundary check */
    if (wpa_info->ssid_len <= WIFI_SSID_PROTOCOL_MAX_LEN) {
        memcpy(wifi_info->ssid, wpa_info->ssid, wpa_info->ssid_len);
        wifi_info->ssid_len = wpa_info->ssid_len;
        wifi_info->ssid[wpa_info->ssid_len] = '\0';
    } else {
        SYS_LOGE("SSID length overflow: %d > %d", wpa_info->ssid_len, WIFI_SSID_PROTOCOL_MAX_LEN);
        wifi_info->ssid_len = 0;
    }

    /* Copy BSSID */
    memcpy(wifi_info->bssid, wpa_info->bssid, 6);

    /* Channel and RSSI */
    wifi_info->channel = wpa_info->channel;
    wifi_info->rssi = wpa_info->rssi;

    /* Authentication and cipher */
    switch (wpa_info->auth) {
        case WPA_CTRL_AUTH_WAPI_PSK:
            wifi_info->auth = AUTH_WAPI_PSK;
            break;
        case WPA_CTRL_AUTH_WAPI_CERT:
            wifi_info->auth = AUTH_WAPI_CERT;
            break;
        default:
            wifi_info->auth = wpa_info->auth;
            break;
    }
    wifi_info->cipher = wpa_info->cipher;
    wifi_info->group_cipher = wpa_info->group_cipher;
    wifi_info->wifi_protocol = wpa_info->wifi_protocol;
    wifi_info->wps = wpa_info->wps;

    /* Copy password with boundary check */
    if (wpa_info->pwd_len <= WIFI_PWD_PROTOCOL_MAX_LEN) {
        memcpy(wifi_info->pwd, wpa_info->pwd, wpa_info->pwd_len);
        wifi_info->pwd_len = wpa_info->pwd_len;
        wifi_info->pwd[wpa_info->pwd_len] = '\0';
    } else {
        SYS_LOGE("Password length overflow: %d > %d", wpa_info->pwd_len, WIFI_PWD_PROTOCOL_MAX_LEN);
        wifi_info->pwd_len = 0;
    }
}

static bool is_valid_wpa_password(const char *password, size_t password_len)
{
    if (!password) {
        return false;
    }

    if (password_len >= WPA_PASSWD_LEN_MIN &&
        password_len <= WIFI_PASSPHRASE_MAX_LEN) {
        return true;
    }

    if (password_len != WIFI_PSK_HEX_LEN) {
        return false;
    }

    for (size_t i = 0; i < password_len; i++) {
        char c = password[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Convert application-layer auth to WPA auth + cipher
 *
 * Converts authentication mode from application layer format to WPA supplicant
 * format, including authentication mode, pairwise cipher, and group cipher.
 *
 * @param[in]  wifi_auth    Application-layer authentication mode
 * @param[in]  password     Password string
 * @param[in]  password_len Password length
 * @param[out] wpa_auth     WPA authentication mode
 * @param[out] pairwise     Pairwise cipher mode
 * @param[out] group        Group cipher mode
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on failure
 */
static sys_err_t cvt_wifi_auth_to_wpa(
    wifi_auth_mode_e wifi_auth,
    const char *password,
    unsigned char password_len,
    wpa_auth_mode_e *wpa_auth,
    wpa_cipher_e *pairwise,
    wpa_cipher_e *group)
{
    if (!wpa_auth || !pairwise || !group) {
        return SYS_ERR_INVALID_ARG;
    }

    switch (wifi_auth) {
        case AUTH_OPEN:
            *wpa_auth = WPA_CTRL_AUTH_OPEN;
            *pairwise = WPA_CTRL_CIPHER_NONE;
            *group = WPA_CTRL_CIPHER_NONE;
            break;

        case AUTH_WEP:
            *wpa_auth = WPA_CTRL_AUTH_WEP;
            /* Determine WEP40 or WEP104 based on key length */
            if (password_len >= 13) {
                *pairwise = WPA_CTRL_CIPHER_WEP104;
            } else {
                *pairwise = WPA_CTRL_CIPHER_WEP40;
            }
            *group = *pairwise;
            break;

        case AUTH_WPA_PSK:
            *wpa_auth = WPA_CTRL_AUTH_WPA_PSK;
            *pairwise = WPA_CTRL_CIPHER_TKIP;
            *group = WPA_CTRL_CIPHER_TKIP;
            break;

        case AUTH_WPA2_PSK:
            *wpa_auth = WPA_CTRL_AUTH_WPA2_PSK;
            *pairwise = WPA_CTRL_CIPHER_CCMP;
            *group = WPA_CTRL_CIPHER_CCMP;
            break;

        case AUTH_WPA_WPA2_PSK:
            *wpa_auth = WPA_CTRL_AUTH_WPA_WPA2_PSK;
            *pairwise = WPA_CTRL_CIPHER_CCMP;
            *group = WPA_CTRL_CIPHER_CCMP;
            break;

        default:
            SYS_LOGE("Unsupported auth mode: %d", wifi_auth);
            return SYS_ERR_INVALID_ARG;
    }
    
    SYS_LOGI("Converted auth: wifi_auth=%d -> wpa_auth=%d, pairwise=%d, group=%d",
             wifi_auth, *wpa_auth, *pairwise, *group);
    return SYS_OK;
}

/**
 * @brief Convert WPA auth to application-layer auth
 *
 * @param[in] wpa_auth WPA authentication mode
 * @return Application-layer authentication mode
 */
static wifi_auth_mode_e cvt_wpa_to_wifi_auth(wpa_auth_mode_e wpa_auth)
{
    switch (wpa_auth) {
        case WPA_CTRL_AUTH_OPEN:
            return AUTH_OPEN;
        case WPA_CTRL_AUTH_WEP:
            return AUTH_WEP;
        case WPA_CTRL_AUTH_WPA_PSK:
            return AUTH_WPA_PSK;
        case WPA_CTRL_AUTH_WPA2_PSK:
            return AUTH_WPA2_PSK;
        case WPA_CTRL_AUTH_WPA_WPA2_PSK:
            return AUTH_WPA_WPA2_PSK;
        case WPA_CTRL_AUTH_WPA2_ENTERPRISE:
            return AUTH_WPA2_ENTERPRISE_TLS;
        case WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE:
            return AUTH_WPA2_WPA3_ENTERPRISE_TLS;
        case WPA_CTRL_AUTH_WPA3_ENT_192:
            return AUTH_WPA3_ENTERPRISE_192_TLS;
        case WPA_CTRL_AUTH_WPA3_ENTERPRISE:
            return AUTH_WPA3_ENTERPRISE_TLS;
        case WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE:
            return AUTH_WPA_WPA2_ENTERPRISE_TLS;
        default:
            return AUTH_OPEN;
    }
}

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
static size_t get_optional_text_len(const char *text, size_t explicit_len)
{
    if (!text) {
        return 0;
    }

    return explicit_len ? explicit_len : strlen(text);
}

static bool sys_wifi_eap_tls_has_complete_credential(void)
{
#if defined(CONFIG_NV) && defined(CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT)
    return wifi_eap_tls_cred_is_complete_by_profile(WIFI_EAP_TLS_CRED_PROFILE_WPA2) ||
           wifi_eap_tls_cred_is_complete_by_profile(WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT) ||
           wifi_eap_tls_cred_is_complete_by_profile(WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT_192);
#else
    return false;
#endif
}
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

static void sys_wifi_copy_optional_text(char *dst, size_t dst_len,
                                        const char *src, size_t explicit_len)
{
    size_t len;

    if (!dst || !dst_len) {
        return;
    }

    dst[0] = '\0';
    if (!src) {
        return;
    }

    len = explicit_len ? explicit_len : strnlen(src, dst_len - 1);
    if (len >= dst_len) {
        len = dst_len - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void sys_wifi_update_sta_eap_nv(const wifi_sta_config_t *sta_cfg)
{
    sys_wifi_context_t *ctx = get_context();
    wifi_sta_eap_nv_info_t eap;

    os_memset(&eap, 0, sizeof(eap));
    if (sta_cfg) {
        eap.method = sta_cfg->eap_method;
        sys_wifi_copy_optional_text(eap.identity, sizeof(eap.identity),
                                    sta_cfg->identity, sta_cfg->identity_len);
        sys_wifi_copy_optional_text(eap.username, sizeof(eap.username),
                                    sta_cfg->eap_username, sta_cfg->eap_username_len);
        sys_wifi_copy_optional_text(eap.password, sizeof(eap.password),
                                    sta_cfg->eap_password, sta_cfg->eap_password_len);
    }

    if (memcmp(&ctx->nv_info.eap, &eap, sizeof(eap)) != 0) {
        os_memcpy(&ctx->nv_info.eap, &eap, sizeof(eap));
        ctx->sta_eap_nv_dirty = true;
    }
}

/**
 * @brief Convert application-layer AP config to WPA network config
 *
 * Converts AP configuration from application layer format to WPA supplicant
 * network configuration format.
 *
 * @param[in]  ap_cfg         Application-layer AP configuration
 * @param[out] wpa_cfg        WPA network configuration
 * @param[in]  actual_channel Actual channel to use
 * @return SYS_OK on success, error code on failure
 */
static sys_err_t cvt_wifi_ap_to_wpa_net(
    const wifi_ap_config_t *ap_cfg,
    wpa_network_config_t *wpa_cfg,
    unsigned char actual_channel)
{
    sys_err_t ret;

    if (!ap_cfg || !wpa_cfg) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(wpa_cfg, 0, sizeof(*wpa_cfg));

    /* SSID */
    wpa_cfg->ssid_len = strnlen((char *)ap_cfg->ssid, sizeof(ap_cfg->ssid));
    if (!wpa_cfg->ssid_len || wpa_cfg->ssid_len > WIFI_SSID_PROTOCOL_MAX_LEN) {
        os_printf(LL_ERR, "Invalid AP SSID length\n");
        return SYS_ERR_INVALID_ARG;
    }
    memcpy(wpa_cfg->ssid, ap_cfg->ssid, wpa_cfg->ssid_len);

    /* Password (support passphrase 8-63 and hex PSK 64) */
    if (ap_cfg->password[0]) {
        size_t password_len = strnlen(ap_cfg->password, WIFI_PWD_MAX_LEN);

        if (password_len > WIFI_PWD_PROTOCOL_MAX_LEN) {
            SYS_LOGE("WiFi password length overflow");
            return SYS_ERR_INVALID_ARG;
        }

        /* Password length validation */
        if (ap_cfg->authmode == AUTH_WEP && password_len != WEP40_PASSWD_LEN && password_len != WEP104_PASSWD_LEN) {
            SYS_LOGE("WEP password length error");
            return SYS_ERR_INVALID_ARG;
        } else if ((ap_cfg->authmode == AUTH_WPA_PSK || ap_cfg->authmode == AUTH_WPA2_PSK ||
                    ap_cfg->authmode == AUTH_WPA_WPA2_PSK) &&
                   !is_valid_wpa_password(ap_cfg->password, password_len)) {
            SYS_LOGE("WPA/WPA2 password length error (expected 8-63 chars or 64 hex chars)");
            return SYS_ERR_INVALID_ARG;
        }

        wpa_cfg->password_len = password_len;
        memcpy(wpa_cfg->password, ap_cfg->password, wpa_cfg->password_len);

        /* Convert authentication mode and cipher */
        ret = cvt_wifi_auth_to_wpa(ap_cfg->authmode, ap_cfg->password,
                                             wpa_cfg->password_len, &wpa_cfg->auth_mode,
                                             &wpa_cfg->pairwise, &wpa_cfg->group);
        if (ret != SYS_OK) {
            return ret;
        }
    } else {
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_OPEN;
        wpa_cfg->pairwise = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->group = WPA_CTRL_CIPHER_NONE;
    }

    /* Channel → Frequency */
    wpa_cfg->channel = actual_channel;
    wpa_cfg->frequency = sys_wifi_channel_to_freq(actual_channel);

    /* AP mode settings */
    wpa_cfg->mode = 2; /* AP mode */
    wpa_cfg->hidden_ssid = ap_cfg->hidden_ssid;
    wpa_cfg->max_connect = ap_cfg->max_connect;

    /* Beacon interval */
    if (ap_cfg->beacon_int) {
        if ((ap_cfg->beacon_int < BEACON_INT_MIN_MS) || (ap_cfg->beacon_int > BEACON_INT_MAX_MS)) {
            SYS_LOGE("beacon interval Beyond range");
            return SYS_ERR_INVALID_ARG;
        }
        wpa_cfg->beacon_int = ap_cfg->beacon_int;
    }

    /* AP client inactive timeout */
    wpa_cfg->ap_inactive_time = ap_cfg->ap_inactive_time;

    return SYS_OK;
}

/**
 * @brief Convert WPA network config to application-layer AP config
 *
 * @param[in]  wpa_cfg WPA network configuration
 * @param[out] ap_cfg  Application-layer AP configuration
 * @return SYS_OK on success, error code on failure
 */
static sys_err_t cvt_wpa_net_to_wifi_ap(
    const wpa_network_config_t *wpa_cfg,
    wifi_ap_config_t *ap_cfg)
{
    if (!wpa_cfg || !ap_cfg) {
        return SYS_ERR_INVALID_ARG;
    }

    /* SSID with boundary check */
    if (wpa_cfg->ssid_len <= WIFI_SSID_PROTOCOL_MAX_LEN) {
        memcpy(ap_cfg->ssid, wpa_cfg->ssid, wpa_cfg->ssid_len);
        ap_cfg->ssid[wpa_cfg->ssid_len] = '\0';  /* Null-terminate */
    } else {
        SYS_LOGE("SSID length overflow: %d > %d", wpa_cfg->ssid_len, WIFI_SSID_PROTOCOL_MAX_LEN);
        return SYS_ERR_INVALID_ARG;
    }

    /* Password with boundary check */
    if (wpa_cfg->password_len <= WIFI_PWD_PROTOCOL_MAX_LEN) {
        memcpy(ap_cfg->password, wpa_cfg->password, wpa_cfg->password_len);
        ap_cfg->password[wpa_cfg->password_len] = '\0';  /* Null-terminate */
    } else {
        SYS_LOGE("Password length overflow: %d > %d", wpa_cfg->password_len, WIFI_PWD_PROTOCOL_MAX_LEN);
        return SYS_ERR_INVALID_ARG;
    }

    /* Auth mode */
    ap_cfg->authmode = cvt_wpa_to_wifi_auth(wpa_cfg->auth_mode);

    /* Other fields */
    ap_cfg->max_connect = wpa_cfg->max_connect;
    ap_cfg->hidden_ssid = wpa_cfg->hidden_ssid;
    ap_cfg->beacon_int = wpa_cfg->beacon_int;
    ap_cfg->ap_inactive_time = wpa_cfg->ap_inactive_time;
    ap_cfg->channel = wpa_cfg->channel;

    return SYS_OK;
}

/**
 * @brief Start SoftAP
 *
 * Starts the SoftAP with the specified configuration. In concurrent mode
 * (STA+AP), the AP will use the STA's channel if STA is connected.
 *
 * @param config AP configuration (SSID, password, channel, auth mode, etc.)
 * @return SYS_OK on success
 * @retval SYS_ERR_WIFI_INVALID_MODE if WiFi mode doesn't support AP
 * @retval SYS_ERR if configuration conversion or WPA start failed
 */
sys_err_t sys_wifi_ap_start(wifi_config_u *config)
{
    wifi_work_mode_e wifi_opmode = sys_wifi_get_mode();
    wpa_network_config_t wpa_cfg;
    sys_err_t ret;

    if (!config) {
        return SYS_ERR_INVALID_ARG;
    }
    if (config->ap.hidden_ssid > 1 ||
        config->ap.authmode < AUTH_OPEN || config->ap.authmode > AUTH_WPA_WPA2_PSK ||
        ((config->ap.authmode == AUTH_OPEN) != (config->ap.password[0] == '\0'))) {
        os_printf(LL_ERR, "Invalid AP hidden_ssid or auth/password combination\n");
        return SYS_ERR_INVALID_ARG;
    }

    /* Check mode */
    if (wifi_opmode != WIFI_MODE_AP && wifi_opmode != WIFI_MODE_AP_STA) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    unsigned char actual_channel = config->ap.channel;
    sys_wifi_context_t *wifi_ctx = get_context();

    /* In concurrent mode, follow STA channel if policy enabled */
    if (wifi_opmode == WIFI_MODE_AP_STA) {
        if (wifi_ctx->policy.ap_follow_sta &&
            wifi_ctx->interfaces[WIFI_VIF_STA].channel) {
            actual_channel = wifi_ctx->interfaces[WIFI_VIF_STA].channel;
        }
    }

    /* Validate channel compatibility with band mode */
    if (actual_channel != 0) {
        if (!sys_wifi_channel_to_freq(actual_channel) ||
            !sys_wifi_is_valid_ap_channel(actual_channel)) {
            os_printf(LL_ERR, "Invalid AP channel: %u\n", actual_channel);
            return SYS_ERR_INVALID_ARG;
        }
        if (!validate_channel_band_compatibility(
                actual_channel,
                wifi_ctx->capability_config.band_mode)) {
            SYS_LOGE("AP start failed: channel %d incompatible with band mode %d",
                     actual_channel, wifi_ctx->capability_config.band_mode);
            return SYS_ERR_INVALID_ARG;
        }
    }

    /* Call conversion function */
    ret = cvt_wifi_ap_to_wpa_net(&config->ap, &wpa_cfg, actual_channel);
    if (ret != SYS_OK) {
        return ret;
    }

    /* Call WPA control interface */
    ret = wpas_ctrl_network_start(WIFI_VIF_AP, &wpa_cfg);
    if (ret != SYS_OK) {
        return ret;
    }

    return SYS_OK;

}

/**
 * @brief Stop SoftAP
 *
 * Stops the running SoftAP. DHCP server stop is automatically handled
 * by the network module via event subscription.
 *
 * @return SYS_OK on success
 * @note If SoftAP is already stopped, no operation or stop event is generated
 */
sys_err_t sys_wifi_ap_stop(void)
{
    if (sys_wifi_get_status(WIFI_VIF_AP) == AP_STATUS_STOP) {
        return SYS_OK;
    }

    wpas_ctrl_network_stop(WIFI_VIF_AP);
    // ❌ Removed: sys_netif_dhcps_stop(WIFI_VIF_AP);
    // Now handled by network module event subscription

    return SYS_OK;
}

/**
 * @brief Get SoftAP configuration information
 *
 * Retrieves the current AP configuration including SSID, password, channel,
 * authentication mode, max connections, hidden SSID, and beacon interval.
 *
 * @param softap_info Output buffer for AP configuration
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if softap_info is NULL
 * @retval SYS_ERR_WIFI_INVALID_MODE if WiFi mode doesn't support AP
 * @retval SYS_ERR_WIFI_AP_NOT_STARTED if AP is not started
 * @retval SYS_ERR if WPA query or conversion failed
 *
 * @note AP must be started before calling this function
 */
sys_err_t sys_wifi_ap_get_info(wifi_ap_config_t *softap_info)
{
    wifi_work_mode_e wifi_opmode = sys_wifi_get_mode();
    wpa_network_config_t wpa_info;
    sys_err_t ret;

    if (!softap_info) {
        return SYS_ERR_INVALID_ARG;
    }

    if ((wifi_opmode != WIFI_MODE_AP) && (wifi_opmode != WIFI_MODE_AP_STA)) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    /* Check if AP is actually started before querying WPA */
    if (!sys_wifi_ap_is_started()) {
        return SYS_ERR_WIFI_AP_NOT_STARTED;
    }

    /* 1. Call WPA query interface */
    ret = wpas_ctrl_softap_get_config(WIFI_VIF_AP, &wpa_info);
    if (ret != SYS_OK) {
        return ret;
    }

    memset(softap_info, 0, sizeof(*softap_info));

    /* Call conversion function */
    ret = cvt_wpa_net_to_wifi_ap(&wpa_info, softap_info);
    if (ret != SYS_OK) {
        return ret;
    }

    return SYS_OK;
}

/**
 * @brief Get SoftAP SSID
 *
 * @param ssid Output buffer for SSID (must be at least 33 bytes)
 * @return SYS_OK on success
 */
sys_err_t sys_wifi_ap_get_ssid(char *ssid)
{
    wifi_ap_config_t softap_info = {0};
    sys_err_t ret;
    size_t ssid_len;

    if (!ssid) {
        return SYS_ERR_INVALID_ARG;
    }

    ret = sys_wifi_ap_get_info(&softap_info);
    if (ret != SYS_OK) {
        return ret;
    }

    /* Use strnlen to safely get SSID length, SSID may not be NULL-terminated */
    ssid_len = strnlen((char*)softap_info.ssid, WIFI_SSID_PROTOCOL_MAX_LEN);
    memcpy(ssid, softap_info.ssid, ssid_len);
    ssid[ssid_len] = '\0';  /* Ensure NULL termination */

    return SYS_OK;
}

/**
 * @brief Get list of connected STAs
 *
 * Retrieves the list of STAs currently connected to the SoftAP.
 *
 * @param info Output buffer for STA information array
 * @param num Input: buffer size; Output: actual number of connected STAs
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if info or num is NULL
 * @retval SYS_ERR_NO_MEM if memory allocation failed
 * @retval SYS_ERR if WPA query failed
 */
sys_err_t sys_wifi_ap_get_sta_list(struct softap_sta_info *info, unsigned int *num)
{
    wpa_softap_sta_info_t *wpa_sta_list = NULL;
    sys_err_t ret;
    unsigned int i;

    if (!info || !num) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 1. Allocate WPA type buffer */
    wpa_sta_list = (wpa_softap_sta_info_t *)os_malloc(*num * sizeof(wpa_softap_sta_info_t));
    if (!wpa_sta_list) {
        return SYS_ERR_NO_MEM;
    }

    /* 2. Call WPA query interface */
    ret = wpas_ctrl_softap_get_sta_list(WIFI_VIF_AP, wpa_sta_list, (unsigned int *)num);
    if (ret != SYS_OK) {
        free(wpa_sta_list);
        return ret;
    }

    /* 3. Type conversion (direct memcpy if types match) */
    for (i = 0; i < *num; i++) {
        os_memcpy(info[i].mac_addr, wpa_sta_list[i].mac_addr, 6);
    }

    free(wpa_sta_list);
    return SYS_OK;

}

/**
 * @brief Deauthenticate a connected STA
 *
 * Disconnects a specific STA from the SoftAP by sending a deauthentication frame.
 *
 * @param sta_mac MAC address of the STA to deauthenticate
 * @param reason Deauthentication reason code (IEEE 802.11)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_ap_deauth_sta(const uint8_t *sta_mac, unsigned short reason)
{
    return wpas_ctrl_softap_deauth_sta(WIFI_VIF_AP, sta_mac, reason);
}

/**
 * @brief Get SoftAP beacon interval
 *
 * @return Beacon interval in milliseconds
 */
unsigned int sys_wifi_ap_get_beacon_interval(void)
{
    return wpas_ctrl_softap_get_beacon_interval();
}

/**
 * @brief Set beacon vendor specific IE (runtime)
 */
sys_err_t sys_wifi_ap_set_vendor_elements(const unsigned char *data, unsigned int len)
{
    return wpas_ctrl_softap_set_vendor_elements(WIFI_VIF_AP, data, len);
}

/**
 * @brief Set beacon interval (runtime)
 */
sys_err_t sys_wifi_ap_set_beacon_interval(unsigned int interval)
{
    return wpas_ctrl_softap_set_beacon_interval(WIFI_VIF_AP, interval);
}

/**
 * @brief Set maximum STA connection count for SoftAP
 *
 * @param max_sta Maximum number of STAs that can connect
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_ap_set_sta_max(unsigned short max_sta)
{
    return wpas_ctrl_softap_set_max_sta(max_sta);
}

/**
 * @brief Get current configured maximum STA count
 *
 * Retrieves the currently configured maximum number of stations
 * that can connect to the AP. This is the user-configured value,
 * which may differ from the hardware capability.
 *
 * @param max_sta Pointer to receive configured maximum STA count
 * @return SYS_OK on success, error code otherwise
 * @retval SYS_ERR_INVALID_ARG if max_sta is NULL
 * @retval SYS_ERR_WIFI_INVALID_MODE if WiFi mode doesn't support AP
 */
sys_err_t sys_wifi_ap_get_sta_max(unsigned short *max_sta)
{
    wifi_work_mode_e wifi_opmode;
    wpa_network_config_t wpa_info;
    sys_err_t ret;

    if (!max_sta) {
        return SYS_ERR_INVALID_ARG;
    }

    wifi_opmode = sys_wifi_get_mode();
    if ((wifi_opmode != WIFI_MODE_AP) && (wifi_opmode != WIFI_MODE_AP_STA)) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    ret = wpas_ctrl_softap_get_config(WIFI_VIF_AP, &wpa_info);
    if (ret != SYS_OK) {
        return ret;
    }

    *max_sta = wpa_info.max_connect;
    return SYS_OK;
}

/**
 * @brief Get hardware capability maximum STA count
 *
 * Returns the maximum number of stations supported by the hardware.
 * This is a hardware limitation that cannot be exceeded.
 *
 * @return Hardware maximum STA count
 */
unsigned char sys_wifi_ap_get_sta_max_capability(void)
{
    return (unsigned char)fhost_get_sta_max_cnt();
}

/**
 * @brief Set AP MAC address
 *
 * Sets AP interface MAC. STA MAC is automatically derived.
 *
 * @param mac MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @see sys_wifi_set_mac() for setting both interfaces explicitly
 */
sys_err_t sys_wifi_ap_set_mac(unsigned char *mac)
{
    return sys_wifi_if_set_mac(WIFI_VIF_AP, mac);
}

/**
 * @brief Get AP MAC address
 *
 * @param mac Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_ap_get_mac(unsigned char *mac)
{
    return sys_wifi_if_get_mac(WIFI_VIF_AP, mac);
}

/**
 * @brief Get AP interface status
 *
 * @return AP WiFi status
 */
wifi_status_e sys_wifi_ap_get_status(void)
{
    return sys_wifi_get_status(WIFI_VIF_AP);
}

/**
 * @brief Check if AP is started
 *
 * @return true if AP is started, false otherwise
 */
bool sys_wifi_ap_is_started(void)
{
    return sys_wifi_is_link_up(WIFI_VIF_AP);
}

/**
 * @brief Start SoftAP from NV configuration
 *
 * Simple wrapper that loads AP config from NV and starts AP.
 * Internal flow:
 * 1. Call sys_wifi_ap_load_config() to read AP config from NV
 * 2. If load succeeds, copy config to wificonfig.ap and call sys_wifi_ap_start()
 *
 * @return SYS_OK on success, SYS_ERR if no NV config or start failed
 */
sys_err_t sys_wifi_ap_auto_start(void)
{
    wifi_config_u wificonfig;
    wifi_ap_config_t softap_conf;
    sys_err_t ret = SYS_ERR;

    memset(&softap_conf, 0, sizeof(softap_conf));
    memset(&wificonfig, 0, sizeof(wificonfig));

    if (sys_wifi_ap_load_config(&softap_conf) == SYS_OK) {
        memcpy(&wificonfig.ap, &softap_conf, sizeof(softap_conf));
        ret = sys_wifi_ap_start(&wificonfig);
    }

    return ret;
}

/**
 * @brief Start SoftAP using configuration from NV storage
 *
 * Starts the WiFi SoftAP (Access Point) mode using configuration stored in non-volatile memory.
 * If no configuration is provided as parameter, it will load from NV storage automatically.
 *
 * @param[in] nv_info Pointer to AP configuration structure. If NULL, configuration will be
 *                    loaded from NV storage using sys_wifi_ap_load_config()
 *
 * @return
 * - SYS_OK: SoftAP started successfully
 * - SYS_ERR: Failed to start SoftAP (e.g., invalid config, empty SSID, NV load failure)
 *
 * @note The function will fail if:
 *       - nv_info is NULL and NV storage has no valid configuration
 *       - SSID is empty
 *
 * @see sys_wifi_ap_load_config(), sys_wifi_ap_start()
 */
sys_err_t sys_wifi_ap_start_by_nv(wifi_ap_config_t *nv_info)
{
    wifi_config_u wifi_config;
    wifi_ap_config_t softap_conf;

    memset(&wifi_config, 0, sizeof(wifi_config));

    if (!nv_info) {
        if (sys_wifi_ap_load_config(&softap_conf) == SYS_OK) {
            nv_info = &softap_conf;
        } else {
            return SYS_ERR;
        }
    }

    if (!nv_info->ssid[0]) {
        return SYS_ERR;
    }

    SYS_LOGI("wifi_set_softap_by_nv: ssid[%s],has_password[%d],channel[%d],authmode[%d],max_connect[%d],hidden_ssid[%d]",
             nv_info->ssid, nv_info->password[0] != '\0', nv_info->channel, nv_info->authmode, nv_info->max_connect,
             nv_info->hidden_ssid);
    memcpy(&wifi_config.ap, nv_info, sizeof(wifi_ap_config_t));

    return sys_wifi_ap_start(&wifi_config);
}

/**
 * @brief Switch SoftAP to a different channel
 *
 * Performs Channel Switch Announcement (CSA) to move the SoftAP to a new channel.
 * Connected STAs are notified and will follow the channel change.
 *
 * @param channel Target channel number
 * @param csa_count Number of beacons before switch (0-255)
 * @param blocktx Whether to block TX during channel switch (1=block, 0=allow)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_ap_switch_channel(unsigned short channel, unsigned char csa_count, unsigned char blocktx)
{
    wpa_csa_config_t config = {0};

    config.freq = sys_wifi_channel_to_freq(channel);
    config.csa_count = csa_count;
    config.blocktx = blocktx;

    return wpas_ctrl_softap_config_csa(WIFI_VIF_AP, &config);
}

/**
 * @brief Set AP channel follow STA policy
 *
 * When enabled, the local SoftAP will automatically follow the STA channel
 * when STA receives a CSA (Channel Switch Announcement) from the connected AP.
 * This is useful in concurrent STA+AP mode to keep both interfaces on the same channel.
 *
 * @param enable true to enable channel following, false to disable
 */
void sys_wifi_ap_set_follow_sta(bool enable)
{
    sys_wifi_context_t *ctx = get_context();

    if (ctx->policy.ap_follow_sta == enable) {
        return;
    }

    ctx->policy.ap_follow_sta = enable;
    fhost_set_ap_channel_follow(enable);
#ifdef CONFIG_NV
    unsigned char val = enable ? 1 : 0;
    hal_system_set_config(CUSTOMER_NV_WIFI_AP_FOLLOW_STA, &val, sizeof(val));
#endif
}

/**
 * @brief Get AP channel follow STA policy status
 *
 * @return true if AP follows STA channel, false otherwise
 */
bool sys_wifi_ap_get_follow_sta(void)
{
    sys_wifi_context_t *ctx = get_context();
    return ctx->policy.ap_follow_sta;
}

/**
 * @brief Synchronize AP channel with STA channel
 *
 * Internal function that synchronizes the SoftAP channel with the STA channel
 * when both interfaces are running. Only performs sync if AP is started and
 * ap_follow_sta policy is enabled. Called automatically on STA connection events.
 */
void sys_wifi_ap_sync_with_sta_channel(void)
{
    int sta_freq, softap_freq;
    unsigned char sta_channel;

    if (sys_wifi_ap_get_status() != AP_STATUS_STARTED) {
        return;
    }

    if (!sys_wifi_ap_get_follow_sta()) {
        return;
    }

    if (sys_wifi_sta_get_status() != STA_STATUS_CONNECTED) {
        return;
    }

    sta_freq = fhost_get_sta_freq();
    softap_freq = fhost_get_softap_freq();
    if (sta_freq < 0 || softap_freq < 0) {
        SYS_LOGD("%s sta_freq=%d softap:%d", __func__, sta_freq, softap_freq);
        return;
    }

    if (sta_freq != softap_freq) {
        /* Convert frequency to channel number before CSA */
        sta_channel = (unsigned char)sys_wifi_freq_to_channel(sta_freq);
        if (sta_channel == 0) {
            SYS_LOGE("%s invalid frequency %d, CSA aborted", __func__, sta_freq);
            return;
        }

        SYS_LOGI("%s csa channel %d (freq %d -> %d)", __func__,
                 sta_channel, softap_freq, sta_freq);
        sys_wifi_ap_switch_channel(sta_channel, 10, 1);
    }
}

/**
 * @brief Load SoftAP configuration from NV storage
 *
 * Loads AP configuration including SSID, password, channel, auth mode,
 * max connections, and hidden SSID setting from NV.
 *
 * @param softap_info Output buffer for AP configuration
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if output is NULL, SYS_ERR if NV read failed
 */
sys_err_t sys_wifi_ap_load_config(wifi_ap_config_t *softap_info)
{
    if (!softap_info) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(softap_info, 0, sizeof(*softap_info));

    if (hal_system_get_config(CUSTOMER_NV_WIFI_AP_SSID, softap_info->ssid , sizeof(softap_info->ssid) - 1) == 0 ||
        hal_system_get_config(CUSTOMER_NV_WIFI_AP_PWD, softap_info->password, sizeof(softap_info->password) - 1) == 0 ||
        hal_system_get_config(CUSTOMER_NV_WIFI_AP_CHANNEL, &softap_info->channel, sizeof(softap_info->channel)) == 0 ||
        hal_system_get_config(CUSTOMER_NV_WIFI_AP_AUTH, &softap_info->authmode, sizeof(softap_info->authmode)) == 0) {
        SYS_LOGE("load ap info nv fail");
        return SYS_ERR;
    }

    hal_system_get_config(CUSTOMER_NV_WIFI_MAX_CON, &softap_info->max_connect, sizeof(softap_info->max_connect));
    hal_system_get_config(CUSTOMER_NV_WIFI_HIDDEN_SSID, &softap_info->hidden_ssid, sizeof(softap_info->hidden_ssid));
    hal_system_get_config(CUSTOMER_NV_WIFI_BEACON_INT, &softap_info->beacon_int, sizeof(softap_info->beacon_int));
    hal_system_get_config(CUSTOMER_NV_WIFI_AP_INACTIVE_TIME, &softap_info->ap_inactive_time, sizeof(softap_info->ap_inactive_time));

    return SYS_OK;
}

/**
 * @brief Save SoftAP configuration to NV storage
 *
 * Saves AP configuration including SSID, password, channel, auth mode,
 * max connections, and hidden SSID setting to NV. On failure, all AP
 * NV entries are deleted for consistency.
 *
 * @note Callers must validate field values before saving. Zero hidden_ssid and
 *       ap_inactive_time are saved; zero max_connect and beacon_int keep NV unchanged.
 * @param softap_info AP configuration to save
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if NULL, SYS_ERR if NV write failed
 */
sys_err_t sys_wifi_ap_save_config(wifi_ap_config_t *softap_info)
{
    int ret = 0;

    if (!softap_info) {
        return SYS_ERR_INVALID_ARG;
    }

    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AP_SSID, softap_info->ssid , sizeof(softap_info->ssid) - 1);
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AP_PWD, softap_info->password, sizeof(softap_info->password));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AP_CHANNEL, &softap_info->channel, sizeof(softap_info->channel));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AP_AUTH, &softap_info->authmode, sizeof(softap_info->authmode));
    if (softap_info->max_connect != 0) {
        ret |= hal_system_set_config(CUSTOMER_NV_WIFI_MAX_CON, &softap_info->max_connect, sizeof(softap_info->max_connect));
    }

    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_HIDDEN_SSID, &softap_info->hidden_ssid, sizeof(softap_info->hidden_ssid));

    if (softap_info->beacon_int != 0) {
        ret |= hal_system_set_config(CUSTOMER_NV_WIFI_BEACON_INT, &softap_info->beacon_int, sizeof(softap_info->beacon_int));
    }

    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AP_INACTIVE_TIME, &softap_info->ap_inactive_time, sizeof(softap_info->ap_inactive_time));

    if (ret) {
        // On error, delete all AP config for consistency
        hal_system_del_config(CUSTOMER_NV_WIFI_AP_SSID);
        hal_system_del_config(CUSTOMER_NV_WIFI_AP_PWD);
        hal_system_del_config(CUSTOMER_NV_WIFI_AP_CHANNEL);
        hal_system_del_config(CUSTOMER_NV_WIFI_AP_AUTH);
        hal_system_del_config(CUSTOMER_NV_WIFI_MAX_CON);
        hal_system_del_config(CUSTOMER_NV_WIFI_HIDDEN_SSID);
        hal_system_del_config(CUSTOMER_NV_WIFI_BEACON_INT);
        hal_system_del_config(CUSTOMER_NV_WIFI_AP_INACTIVE_TIME);   
        return SYS_ERR;
    }

    return SYS_OK;
}

/**
 * @brief Convert wifi_sta_config_t to wpa_network_config_t
 *
 * @param[in]  sta_cfg System WiFi configuration type
 * @param[out] wpa_cfg WPA configuration type
 */
static void cvt_sta_to_wpa_net(const wifi_sta_config_t *sta_cfg,
                                        wpa_network_config_t *wpa_cfg)
{
    if (!sta_cfg || !wpa_cfg) {
        return;
    }

    memset(wpa_cfg, 0, sizeof(*wpa_cfg));

    /* 1. SSID */
    wpa_cfg->ssid_len = sta_cfg->ssid[0] ?
                        strnlen((char *)sta_cfg->ssid, WIFI_SSID_PROTOCOL_MAX_LEN) : 0;
    if (wpa_cfg->ssid_len > 0) {
        memcpy(wpa_cfg->ssid, sta_cfg->ssid, wpa_cfg->ssid_len);
    }

    /* 2. Password - format already validated in sys_wifi_sta_start()
     * Valid: 0 (OPEN), 5 (WEP40-ASCII), 8-63 (WPA passphrase),
     * or 64 hexadecimal characters (WPA/WPA2 raw PSK).
     */
    wpa_cfg->password_len = sta_cfg->password[0] ?
                            strnlen(sta_cfg->password, WIFI_PWD_MAX_LEN) : 0;
    if (wpa_cfg->password_len > 0) {
        memcpy(wpa_cfg->password, sta_cfg->password, wpa_cfg->password_len);
    }

    /* 3. BSSID (optional) */
    if (!IS_ZERO_MAC(sta_cfg->bssid)) {
        memcpy(wpa_cfg->bssid, sta_cfg->bssid, 6);
    }

    /* 4. Channel → Frequency conversion */
    wpa_cfg->channel = sta_cfg->channel;
    if (sta_cfg->channel > 0) {
        wpa_cfg->frequency = sys_wifi_channel_to_freq(sta_cfg->channel);
    }

    if (sta_cfg->candidate_channel && sta_cfg->candidate_channel != sta_cfg->channel) {
        wpa_cfg->candidate_frequency = sys_wifi_channel_to_freq(sta_cfg->candidate_channel);
    }

    /* 5. Infer authentication and encryption based on password */
    if (wpa_cfg->password_len == 0) {
        /* Open network */
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_OPEN;
        wpa_cfg->pairwise = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->group = WPA_CTRL_CIPHER_NONE;
    } else if (wpa_cfg->password_len == WEP40_PASSWD_LEN/* || wpa_cfg->password_len == 13*/) {
        /* WEP (auto-detect key length in lower layer) */
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_WEP;
        //wpa_cfg->pairwise = WPA_CTRL_CIPHER_WEP;
        //wpa_cfg->group = WPA_CTRL_CIPHER_WEP;
    } else {
        /* Auto-adapt: support WPA2/WPA3, let lower layer negotiate cipher */
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_WPA2_WPA3_PSK;
        wpa_cfg->pairwise = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->group = WPA_CTRL_CIPHER_NONE;
    }

    switch (sta_cfg->eap_method) {
    case WIFI_STA_EAP_NONE:
        break;
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    case WIFI_STA_EAP_TLS:
        wpa_cfg->eap_method = WPA_CTRL_EAP_TLS;
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_ENTERPRISE_AUTO;
        wpa_cfg->pairwise = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->group = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->identity = sta_cfg->identity ? sta_cfg->identity :
                            WIFI_STA_DEFAULT_EAP_IDENTITY;
        wpa_cfg->identity_len = get_optional_text_len(wpa_cfg->identity,
                                                      sta_cfg->identity ?
                                                      sta_cfg->identity_len : 0);
        break;
    case WIFI_STA_EAP_PEAP:
    case WIFI_STA_EAP_TTLS:
    case WIFI_STA_EAP_LEAP:
        if (!sta_cfg->eap_username || !sta_cfg->eap_password) {
            break;
        }
        if (sta_cfg->eap_method == WIFI_STA_EAP_PEAP) {
            wpa_cfg->eap_method = WPA_CTRL_EAP_PEAP;
        } else if (sta_cfg->eap_method == WIFI_STA_EAP_TTLS) {
            wpa_cfg->eap_method = WPA_CTRL_EAP_TTLS;
        } else {
            wpa_cfg->eap_method = WPA_CTRL_EAP_LEAP;
        }
        wpa_cfg->auth_mode = WPA_CTRL_AUTH_ENTERPRISE_AUTO;
        wpa_cfg->pairwise = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->group = WPA_CTRL_CIPHER_NONE;
        wpa_cfg->identity = sta_cfg->eap_username;
        wpa_cfg->identity_len = get_optional_text_len(sta_cfg->eap_username,
                                                      sta_cfg->eap_username_len);
        wpa_cfg->eap_password = sta_cfg->eap_password;
        wpa_cfg->eap_password_len = get_optional_text_len(sta_cfg->eap_password,
                                                          sta_cfg->eap_password_len);
        if (sta_cfg->eap_method != WIFI_STA_EAP_LEAP) {
            wpa_cfg->phase2 = "auth=MSCHAPV2";
        }
        if (!wpa_cfg->identity_len || !wpa_cfg->eap_password_len) {
            wpa_cfg->eap_method = WPA_CTRL_EAP_NONE;
            wpa_cfg->identity = NULL;
            wpa_cfg->identity_len = 0;
            wpa_cfg->eap_password = NULL;
            wpa_cfg->eap_password_len = 0;
            wpa_cfg->phase2 = NULL;
        }
        break;
#else
    case WIFI_STA_EAP_TLS:
    case WIFI_STA_EAP_PEAP:
    case WIFI_STA_EAP_TTLS:
    case WIFI_STA_EAP_LEAP:
        break;
#endif
    default:
        break;
    }

    /* 6. Mode */
    wpa_cfg->mode = 0; /* STA mode */
    wpa_cfg->network_id = 0;

    /* 7. scan_ssid: Enable active scan for BSSID-only connection or hidden SSID
     * - When BSSID is provided: enable scan to find and match the specific AP
     * - When SSID is empty: BSSID-only connection requires scan to get SSID
     */
    if (!IS_ZERO_MAC(sta_cfg->bssid) || wpa_cfg->ssid_len == 0) {
        wpa_cfg->scan_ssid = 1;
    } else {
        wpa_cfg->scan_ssid = 0;
    }

    wpa_cfg->hidden_ssid = 0;
}

static sys_err_t sys_wifi_sta_check_eap_config(const wifi_sta_config_t *sta_cfg, const wpa_network_config_t *wpa_cfg)
{
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    size_t username_len;
    size_t password_len;
#endif

    if (!sta_cfg) {
        return SYS_ERR_INVALID_ARG;
    }

    switch (sta_cfg->eap_method) {
    case WIFI_STA_EAP_NONE:
        return SYS_OK;
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    case WIFI_STA_EAP_TLS:
        if (sys_wifi_eap_tls_has_complete_credential()) {
            return SYS_OK;
        }
        if (wpa_cfg && wpas_ctrl_network_has_pmksa(WIFI_VIF_STA, wpa_cfg) == SYS_OK) {
            SYS_LOGI("EAP-TLS credential missing: use cached PMKSA for STA connection");
            return SYS_OK;
        }
        SYS_LOGE("EAP-TLS credential missing: abort STA connection");
        return SYS_ERR_INVALID_STATE;
    case WIFI_STA_EAP_PEAP:
    case WIFI_STA_EAP_TTLS:
    case WIFI_STA_EAP_LEAP:
        username_len = get_optional_text_len(sta_cfg->eap_username,
                                             sta_cfg->eap_username_len);
        password_len = get_optional_text_len(sta_cfg->eap_password,
                                             sta_cfg->eap_password_len);
        if (!username_len || !password_len) {
            SYS_LOGE("EAP username/password missing: abort STA connection");
            return SYS_ERR_INVALID_ARG;
        }
        return SYS_OK;
#else
    case WIFI_STA_EAP_TLS:
    case WIFI_STA_EAP_PEAP:
    case WIFI_STA_EAP_TTLS:
    case WIFI_STA_EAP_LEAP:
        SYS_LOGE("Enterprise authentication is not supported");
        return SYS_ERR_NOT_SUPPORTED;
#endif
    default:
        return SYS_ERR_INVALID_ARG;
    }

    return SYS_OK;
}

static void sys_wifi_sta_selected_bss_bridge(
        const wpas_ctrl_selected_bss_info_t *info, void *ctx)
{
    wifi_selected_bss_info_t selected;

    (void)ctx;

    if (!info || !s_selected_bss_cb) {
        return;
    }

    memset(&selected, 0, sizeof(selected));
    memcpy(selected.bssid, info->bssid, sizeof(selected.bssid));
    selected.ssid_len = info->ssid_len;
    memcpy(selected.ssid, info->ssid, selected.ssid_len);
    selected.freq = info->freq;
    selected.rssi = info->rssi;

    s_selected_bss_cb(&selected, s_selected_bss_cb_ctx);
}

sys_err_t sys_wifi_sta_register_selected_bss_callback(wifi_selected_bss_cb_t cb,
                                                      void *ctx)
{
    if (!cb) {
        return SYS_ERR_INVALID_ARG;
    }

    s_selected_bss_cb = cb;
    s_selected_bss_cb_ctx = ctx;

    return wpas_ctrl_sta_register_selected_bss_cb(
        sys_wifi_sta_selected_bss_bridge, NULL);
}

sys_err_t sys_wifi_sta_unregister_selected_bss_callback(void)
{
    s_selected_bss_cb = NULL;
    s_selected_bss_cb_ctx = NULL;
    return wpas_ctrl_sta_unregister_selected_bss_cb();
}

/**
 * @brief Convert application-layer scan config to WPA scan config
 *
 * @param[in]  scan_cfg        Application-layer scan configuration
 * @param[out] wpa_scan        WPA scan configuration
 * @param[out] freq_buffer_out Dynamically allocated frequency buffer pointer (caller must free)
 * @param[in]  band_mode       Current band mode for channel filtering
 * @return SYS_OK on success, error code on failure
 */
static sys_err_t cvt_scan_to_wpa(const wifi_scan_config_t *scan_cfg,
                                      wpa_scan_config_t *wpa_scan,
                                      unsigned short **freq_buffer_out,
                                      wifi_band_mode_e band_mode)
{
    if (!scan_cfg || !wpa_scan || !freq_buffer_out) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(wpa_scan, 0, sizeof(*wpa_scan));
    *freq_buffer_out = NULL;

    /* SSID (optional) */
    if (scan_cfg->ssid && scan_cfg->ssid[0]) {
        size_t len = strnlen((char *)scan_cfg->ssid, WIFI_SSID_PROTOCOL_MAX_LEN);
        if (len > 0 && len <= WIFI_SSID_PROTOCOL_MAX_LEN) {
            memcpy(wpa_scan->ssid, scan_cfg->ssid, len);
        }
    }

    /* BSSID (optional) */
    if (scan_cfg->bssid) {
        memcpy(wpa_scan->bssid, scan_cfg->bssid, 6);
    }

    /* Scan parameters */
    wpa_scan->scan_time = scan_cfg->scan_time;
    wpa_scan->max_item = scan_cfg->max_item;
    wpa_scan->passive = scan_cfg->passive;

    /* Channel handling: prefer channel_list, fallback to channel */
    if (scan_cfg->num_chan > 0 && scan_cfg->num_chan <= WIFI_SCAN_CHANNEL_MAX) {
        /* Multi-channel scan: channel → frequency conversion (dynamic allocation, 2.4G + 5G) */
        unsigned short *freq_buffer = (unsigned short *)os_malloc(sizeof(unsigned short) * scan_cfg->num_chan);
        if (!freq_buffer) {
            SYS_LOGE("Failed to allocate freq buffer for scan");
            return SYS_ERR_NO_MEM;
        }

        unsigned char valid_count = 0;
        unsigned char filtered_count = 0;
        for (unsigned int i = 0; i < scan_cfg->num_chan; i++) {
            unsigned char channel = scan_cfg->channel_list[i];

            /* Band mode filtering */
            if (!validate_channel_band_compatibility(channel, band_mode)) {
                filtered_count++;
                SYS_LOGD("Filtered channel %d (incompatible with band mode %d)",
                         channel, band_mode);
                continue;
            }

            unsigned short freq = sys_wifi_channel_to_freq(channel);
            if (freq > 0) {
                freq_buffer[valid_count++] = freq;
            } else {
                SYS_LOGW("Invalid channel in list: %d", channel);
            }
        }

        if (filtered_count > 0) {
            SYS_LOGI("Scan: filtered %d channels due to band mode %d",
                     filtered_count, band_mode);
        }

        if (valid_count > 0) {
            wpa_scan->freq_count = valid_count;
            wpa_scan->freq_list = freq_buffer;
            *freq_buffer_out = freq_buffer;  /* Pass to caller */
        } else {
            os_free(freq_buffer);  /* No valid channels, free memory */
        }
    } else if (scan_cfg->channel > 0) {
        /* Validate single channel compatibility with band mode */
        if (!validate_channel_band_compatibility(scan_cfg->channel, band_mode)) {
            SYS_LOGE("Scan failed: channel %d incompatible with band mode %d",
                     scan_cfg->channel, band_mode);
            return SYS_ERR_INVALID_ARG;
        }

        /* Single channel scan: channel → frequency conversion */
        unsigned short *freq_buffer = (unsigned short *)os_malloc(sizeof(unsigned short));
        if (!freq_buffer) {
            SYS_LOGE("Failed to allocate freq buffer for single channel scan");
            return SYS_ERR_NO_MEM;
        }

        unsigned short freq = sys_wifi_channel_to_freq(scan_cfg->channel);
        //SYS_LOGI("[SCAN_DEBUG] Single channel scan: ch=%d freq=%d", scan_cfg->channel, freq);
        if (freq > 0) {
            freq_buffer[0] = freq;
            wpa_scan->freq_count = 1;
            wpa_scan->freq_list = freq_buffer;
            *freq_buffer_out = freq_buffer;
            //SYS_LOGI("[SCAN_DEBUG] freq_buffer[0]=%d, freq_count=%d", freq_buffer[0], wpa_scan->freq_count);
        } else {
            SYS_LOGW("Invalid single channel: %d", scan_cfg->channel);
            os_free(freq_buffer);
        }
    }

    return SYS_OK;
}

/**
 * @brief Start STA and connect to AP
 *
 * Starts the STA interface and initiates connection to the specified AP.
 * This function does not start or stop the SoftAP interface.
 *
 * When called in STA_STATUS_DISCON state with the same network config,
 * takes a fast reconnect path using BSS cache (skips full scan).
 * When config differs, stops the old network and does a full start.
 *
 * @param config STA configuration (SSID, password, BSSID, auth mode, etc.)
 * @return SYS_OK on success
 * @retval SYS_ERR_WIFI_INVALID_MODE if WiFi mode doesn't support STA
 * @retval SYS_ERR_WIFI_BUSY if STA is in a state other than STOP or DISCON
 * @retval SYS_ERR_WIFI_FAIL if WPA start failed
 */
sys_err_t sys_wifi_sta_start(wifi_config_u *config)
{
    wpa_network_config_t wpa_cfg;
    wifi_work_mode_e wifi_opmode = sys_wifi_get_mode();
    sys_err_t ret;

    /* 1. Parameter validation */
    if (!config) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 2. SSID validation: must not be empty */
    if (config->sta.ssid[0] == '\0') {
        return SYS_ERR_WIFI_NO_SSID;
    }

    /* 3. Password validation
     * Valid lengths:
     * - 0: OPEN network (no password)
     * - WEP40-ASCII: WEP40_PASSWD_LEN (5)
     * - WPA passphrase: WPA_PASSWD_LEN_MIN-WIFI_PASSPHRASE_MAX_LEN (8-63)
     * - WPA/WPA2 raw PSK: WIFI_PSK_HEX_LEN (64 hexadecimal characters)
     */
    size_t pwd_len = strnlen(config->sta.password, WIFI_PWD_MAX_LEN);
    if (pwd_len > 0) {
        bool valid = (pwd_len == WEP40_PASSWD_LEN) ||
                     is_valid_wpa_password(config->sta.password, pwd_len);
        if (!valid) {
            return SYS_ERR_WIFI_INVALID_PASSWORD;
        }
    }

    /* 4. Check mode */
    if (wifi_opmode != WIFI_MODE_STA && wifi_opmode != WIFI_MODE_AP_STA) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    /* 5. Check status - allow STOP and DISCON */
    wifi_status_e sta_status = sys_wifi_get_status(WIFI_VIF_STA);
    if (sta_status != STA_STATUS_STOP && sta_status != STA_STATUS_DISCON) {
        return SYS_ERR_WIFI_BUSY;
    }

    sys_wifi_context_t *wifi_ctx = get_context();
    if ((config->sta.candidate_channel && !config->sta.channel) ||
        !validate_channel_band_compatibility(config->sta.channel, wifi_ctx->capability_config.band_mode) ||
        !validate_channel_band_compatibility(config->sta.candidate_channel, wifi_ctx->capability_config.band_mode)) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 5a. DISCON: try fast reconnect if config matches */
    if (sta_status == STA_STATUS_DISCON) {
        wpa_network_config_t chk_cfg;
        cvt_sta_to_wpa_net(&config->sta, &chk_cfg);
        ret = sys_wifi_sta_check_eap_config(&config->sta, &chk_cfg);
        if (ret != SYS_OK) {
            return ret;
        }
        ret = wpas_ctrl_network_reconnect(WIFI_VIF_STA, &chk_cfg);
        if (ret == SYS_OK) {
            /* Config matches, fast reconnect initiated */
            sys_wifi_set_status(WIFI_VIF_STA, STA_STATUS_START);
            return SYS_OK;
        }
        /* Config mismatch, stop old network and fall through to full start */
        wpas_ctrl_network_stop(WIFI_VIF_STA);
    }

    /* 7. Type conversion: wifi_sta_config_t → wpa_network_config_t */
    cvt_sta_to_wpa_net(&config->sta, &wpa_cfg);
    ret = sys_wifi_sta_check_eap_config(&config->sta, &wpa_cfg);
    if (ret != SYS_OK) {
        return ret;
    }

    /* 8. Call WPA control interface */
    ret = wpas_ctrl_network_start(WIFI_VIF_STA, &wpa_cfg);
    if (ret != SYS_OK) {
        SYS_LOGE("WPA network start failed: %d", ret);
        return SYS_ERR_WIFI_FAIL;
    }

    sys_wifi_update_sta_eap_nv(&config->sta);

    /* 9. Update status */
    sys_wifi_set_status(WIFI_VIF_STA, STA_STATUS_START);
    return SYS_OK;
}

/**
 * @brief Stop STA and disconnect from AP
 *
 * Stops the STA interface and disconnects from the current AP.
 * Network interface down is automatically handled by the network
 * module via event subscription.
 *
 * @return SYS_OK on success
 * @note If STA is already stopped, no operation or stop event is generated
 */
sys_err_t sys_wifi_sta_stop(void)
{
    if (sys_wifi_get_status(WIFI_VIF_STA) == STA_STATUS_STOP) {
        return SYS_OK;
    }

    wpas_ctrl_network_stop(WIFI_VIF_STA);
    sys_wifi_set_status(WIFI_VIF_STA, STA_STATUS_STOP);

    return 0;
}

/**
 * @brief Disconnect STA from the current AP while keeping configuration
 *
 * Disconnects from the AP but preserves the WPA network configuration,
 * allowing fast reconnection via sys_wifi_sta_reconnect() or
 * sys_wifi_sta_start() with the same config.
 *
 * @return SYS_OK on success, error code otherwise
 *
 * @see sys_wifi_sta_reconnect(), sys_wifi_sta_stop()
 */
sys_err_t sys_wifi_sta_disconnect(void)
{
    sys_err_t ret;

    ret = wpas_ctrl_network_disconnect(WIFI_VIF_STA);
    if (ret != SYS_OK) {
        return ret;
    }

    sys_wifi_set_status(WIFI_VIF_STA, STA_STATUS_DISCON);
    return SYS_OK;
}

/**
 * @brief Reconnect STA to the previously configured network
 *
 * Fast reconnect path: uses BSS cache when available to avoid full scan.
 * Only works after sys_wifi_sta_disconnect() (STA_STATUS_DISCON state).
 *
 * @return SYS_OK on reconnect initiated, error code otherwise
 *
 * @see sys_wifi_sta_disconnect(), sys_wifi_sta_start()
 */
sys_err_t sys_wifi_sta_reconnect(void)
{
    wifi_work_mode_e wifi_opmode = sys_wifi_get_mode();
    sys_err_t ret;

    if (wifi_opmode != WIFI_MODE_STA && wifi_opmode != WIFI_MODE_AP_STA) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    if (sys_wifi_get_status(WIFI_VIF_STA) != STA_STATUS_DISCON) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = wpas_ctrl_network_reconnect(WIFI_VIF_STA, NULL);
    if (ret != SYS_OK) {
        return ret;
    }

    sys_wifi_set_status(WIFI_VIF_STA, STA_STATUS_START);
    return SYS_OK;
}

/**
 * @brief Roam STA to a target BSSID in the current ESS
 */
sys_err_t sys_wifi_sta_roam(const unsigned char *bssid)
{
    wifi_work_mode_e wifi_opmode = sys_wifi_get_mode();

    if (wifi_opmode != WIFI_MODE_STA && wifi_opmode != WIFI_MODE_AP_STA) {
        return SYS_ERR_WIFI_INVALID_MODE;
    }

    if (sys_wifi_get_status(WIFI_VIF_STA) != STA_STATUS_CONNECTED) {
        return SYS_ERR_WIFI_NOT_CONNECTED;
    }

    if (!bssid || IS_ZERO_MAC(bssid) || IS_BRODCAST_MAC(bssid) ||
        IS_MULTCAST_MAC(bssid)) {
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_roam(WIFI_VIF_STA, bssid);
}

/**
 * @brief Get STA connection information
 *
 * Retrieves comprehensive information about the current STA connection
 * including SSID, BSSID, channel, RSSI, authentication mode, and cipher.
 *
 * @param info Output buffer for connection information
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if info is NULL
 * @retval SYS_ERR if WPA query failed or not connected
 */
sys_err_t sys_wifi_sta_get_info(wifi_info_t *info)
{
    wpa_wifi_info_t wpa_info;
    sys_err_t ret;

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 1. Call WPA query interface */
    ret = wpas_ctrl_station_get_info(WIFI_VIF_STA, &wpa_info);
    if (ret != SYS_OK) {
        return ret;
    }

    /* 2. Type conversion (handled by upper layer) */
    cvt_wpa_to_wifi_info(&wpa_info, info);

    return SYS_OK;
}

/**
 * @brief Get connected AP SSID
 *
 * @param ssid Output buffer for SSID (must be at least 33 bytes)
 * @return SYS_OK on success, SYS_ERR if not connected or query failed
 */
sys_err_t sys_wifi_sta_get_ssid(char *ssid)
{
    wpa_wifi_info_t info = {0};

    if (!ssid) {
        return SYS_ERR_INVALID_ARG;
    }

    if (sys_wifi_get_status(WIFI_VIF_STA) != STA_STATUS_CONNECTED) {
        return SYS_ERR;
    }

    if (wpas_ctrl_station_get_info(WIFI_VIF_STA, &info) != SYS_OK) {
        return SYS_ERR;
    }

    os_memcpy(ssid, info.ssid, info.ssid_len);
    ssid[info.ssid_len] = '\0';  /* Ensure NULL termination */

    return SYS_OK;
}

/**
 * @brief Get connected AP BSSID
 *
 * @param bssid Output buffer for BSSID (6 bytes)
 * @return SYS_OK on success, SYS_ERR if not connected or query failed
 */
sys_err_t sys_wifi_sta_get_bssid(unsigned char *bssid)
{
    wpa_wifi_info_t info = {0};

    if (sys_wifi_get_status(WIFI_VIF_STA) != STA_STATUS_CONNECTED) {
        return SYS_ERR;
    }

    if (wpas_ctrl_station_get_info(WIFI_VIF_STA, &info) != SYS_OK) {
        return SYS_ERR;
    }
    memcpy(bssid, info.bssid, NETIF_MAX_HWADDR_LEN);

    return SYS_OK;
}

/**
 * @brief Get RSSI (signal strength) of connected AP
 *
 * @param rssi Output buffer for RSSI value in dBm
 * @return SYS_OK on success, SYS_ERR if not connected
 */
sys_err_t sys_wifi_sta_get_rssi(signed char *rssi)
{
    wifi_info_t info = {0};

    if (sys_wifi_get_status(WIFI_VIF_STA) != STA_STATUS_CONNECTED) {
        return SYS_ERR;
    }

    sys_wifi_sta_get_info(&info);
    *rssi = info.rssi;

    return SYS_OK;
}

/**
 * @brief Send 802.11k RRM Neighbor Report Request from STA
 */
sys_err_t sys_wifi_sta_rrm_neighbor_req(wifi_vif_e vif_id,
                                        const unsigned char *ssid,
                                        unsigned int ssid_len)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("RRM Neighbor Report is only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (ssid_len > WIFI_SSID_PROTOCOL_MAX_LEN) {
        SYS_LOGE("Invalid RRM Neighbor Report SSID length: %u", ssid_len);
        return SYS_ERR_INVALID_ARG;
    }

    if (!ssid && ssid_len) {
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_rrm_neighbor_req((uint8_t)vif_id, ssid, ssid_len);
}

/**
 * @brief Send 802.11v WNM BSS Transition Management Query from STA
 */
sys_err_t sys_wifi_sta_wnm_bss_query(wifi_vif_e vif_id,
                                     unsigned char reason,
                                     int cand_list)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("WNM BSS Transition query is only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_wnm_bss_query((uint8_t)vif_id, reason, cand_list);
}

/**
 * @brief Set PCI mode for station
 */
sys_err_t sys_wifi_sta_set_pci_mode(wifi_vif_e vif_id, wifi_pci_mode_e pci_mode)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("PCI mode only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (pci_mode != WIFI_PCI_DISABLE && pci_mode != WIFI_PCI_ENABLE) {
        SYS_LOGE("Invalid PCI mode: %d", pci_mode);
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_set_pci_mode((uint8_t)vif_id, (int)pci_mode);
}

/**
 * @brief Get PCI mode for station
 */
sys_err_t sys_wifi_sta_get_pci_mode(wifi_vif_e vif_id, wifi_pci_mode_e *pci_mode)
{
    int mode = 0;
    sys_err_t ret;

    if (vif_id != WIFI_VIF_STA || !pci_mode) {
        SYS_LOGE("Invalid arguments for get PCI mode");
        return SYS_ERR_INVALID_ARG;
    }

    ret = wpas_ctrl_sta_get_pci_mode((uint8_t)vif_id, &mode);
    if (ret == SYS_OK) {
        *pci_mode = (wifi_pci_mode_e)mode;
    }

    return ret;
}

/**
 * @brief Set the weakest authentication mode accepted by STA
 */
sys_err_t sys_wifi_sta_set_authmode_threshold(
    wifi_vif_e vif_id, wifi_auth_mode_e authmode_threshold)
{
    wpa_auth_mode_e wpa_authmode_threshold;

    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("Authmode threshold only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    switch (authmode_threshold) {
    case AUTH_OPEN:
        wpa_authmode_threshold = WPA_CTRL_AUTH_OPEN;
        break;
    case AUTH_WEP:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WEP;
        break;
    case AUTH_WPA_PSK:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA_PSK;
        break;
    case AUTH_WPA2_PSK:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA2_PSK;
        break;
    case AUTH_WPA_WPA2_PSK:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA_WPA2_PSK;
        break;
    case AUTH_WPA3_SAE:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA3_SAE;
        break;
    case AUTH_WPA2_WPA3_PSK:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA2_WPA3_PSK;
        break;
    case AUTH_WPA2_ENTERPRISE_TLS:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA2_ENTERPRISE;
        break;
    case AUTH_WPA2_WPA3_ENTERPRISE_TLS:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE;
        break;
    case AUTH_WPA3_ENTERPRISE_192_TLS:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA3_ENT_192;
        break;
    case AUTH_WPA3_ENTERPRISE_TLS:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA3_ENTERPRISE;
        break;
    case AUTH_WPA_WPA2_ENTERPRISE_TLS:
        wpa_authmode_threshold = WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE;
        break;
    default:
        SYS_LOGE("Invalid STA authmode threshold: %d", authmode_threshold);
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_set_authmode_threshold(
        (uint8_t)vif_id, wpa_authmode_threshold);
}

/**
 * @brief Set PMF mode for STA (AT command internal use)
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @param vif_id Virtual interface ID (must be WIFI_VIF_STA)
 * @param pmf_mode PMF mode (DISABLED/OPTIONAL/REQUIRED)
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on invalid parameters
 */
sys_err_t sys_wifi_sta_set_pmf(wifi_vif_e vif_id, wifi_pmf_mode_e pmf_mode)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("PMF mode only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (pmf_mode < WIFI_PMF_DISABLED || pmf_mode > WIFI_PMF_REQUIRED) {
        SYS_LOGE("Invalid PMF mode: %d (valid: 0-2)", pmf_mode);
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_set_pmf((uint8_t)vif_id, (int)pmf_mode);
}

/**
 * @brief Get current PMF mode for STA (AT command internal use)
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @param vif_id Virtual interface ID (must be WIFI_VIF_STA)
 * @param[out] pmf_mode Current PMF mode
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on invalid parameters
 */
sys_err_t sys_wifi_sta_get_pmf(wifi_vif_e vif_id, wifi_pmf_mode_e *pmf_mode)
{
    int mode = 0;
    sys_err_t ret;

    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("PMF mode only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (!pmf_mode) {
        SYS_LOGE("pmf_mode pointer is NULL");
        return SYS_ERR_INVALID_ARG;
    }

    ret = wpas_ctrl_sta_get_pmf((uint8_t)vif_id, &mode);
    if (ret == SYS_OK) {
        *pmf_mode = (wifi_pmf_mode_e)mode;
    }

    return ret;
}

/**
 * @brief Set STA connection policy (AT command internal use)
 *
 * Configure reconnection interval and retry count for STA.
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @param vif_id Virtual interface ID (must be WIFI_VIF_STA)
 * @param interval_sec Reconnection interval in seconds (0 = use default 5ms/100ms)
 * @param repeat_count Maximum reconnection attempts (0 = unlimited)
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on invalid parameters
 */
sys_err_t sys_wifi_sta_set_connect_policy(wifi_vif_e vif_id, int interval_sec, int repeat_count)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("Connection policy only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (interval_sec < 0 || repeat_count < 0) {
        SYS_LOGE("Invalid connection policy parameters: interval=%d, count=%d",
                 interval_sec, repeat_count);
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_set_connect_policy((uint8_t)vif_id, interval_sec, repeat_count);
}

/**
 * @brief Get current STA connection policy (AT command internal use)
 *
 * @note This function is designed for AT command implementation.
 *       General applications typically don't need to call this directly.
 *
 * @param vif_id Virtual interface ID (must be WIFI_VIF_STA)
 * @param[out] interval_sec Reconnection interval in seconds
 * @param[out] repeat_count Maximum reconnection attempts
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG on invalid parameters
 */
sys_err_t sys_wifi_sta_get_connect_policy(wifi_vif_e vif_id, int *interval_sec, int *repeat_count)
{
    if (vif_id != WIFI_VIF_STA) {
        SYS_LOGE("Connection policy only applicable to STA interface");
        return SYS_ERR_INVALID_ARG;
    }

    if (!interval_sec || !repeat_count) {
        SYS_LOGE("Output pointers cannot be NULL");
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_sta_get_connect_policy((uint8_t)vif_id, interval_sec, repeat_count);
}


/**
 * @brief Connect to AP using NV-stored configuration
 *
 * Initiates STA connection using WiFi configuration loaded from NV storage.
 * If nv_info is NULL, uses internal WiFi context. Supports fast connect mode
 * if CONFIG_FAST_CONNECT is defined.
 *
 * @param nv_info NV configuration to use (NULL to use internal context)
 * @return SYS_OK on success
 * @retval SYS_ERR if SSID is empty or connection failed
 */
sys_err_t sys_wifi_set_sta_by_nv(wifi_nv_info_t *nv_info)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_config_u sta_config;
    /* Internal NV auto-connect remains channel-only; explicit profiles honor BSSID. */
    bool use_bssid = nv_info != NULL;

    memset(&sta_config, 0, sizeof(sta_config));

    if (!nv_info)
        nv_info = &wifi_ctx->nv_info;
    if (!nv_info->sta_ssid[0])
        return SYS_ERR;

    memcpy(sta_config.sta.ssid, nv_info->sta_ssid, strlen(nv_info->sta_ssid));
    if (use_bssid) {
        memcpy(sta_config.sta.bssid, nv_info->sta_bssid, sizeof(nv_info->sta_bssid));
    }
//  sta_config.sta.channel = nv_info->channel;
#if defined(CONFIG_FAST_CONNECT)
    sta_config.sta.channel = nv_info->channel;
    if (sta_config.sta.channel &&
        !validate_channel_band_compatibility(sta_config.sta.channel, wifi_ctx->capability_config.band_mode)) {
        sta_config.sta.channel = 0;
    }
    if (sta_config.sta.channel && nv_info->candidate_channel &&
        validate_channel_band_compatibility(nv_info->candidate_channel, wifi_ctx->capability_config.band_mode)) {
        sta_config.sta.candidate_channel = nv_info->candidate_channel;
    }
    //wifi_set_scan_method(WIFI_FAST_SCAN);
#endif
#if defined(CONFIG_ECW6700) || defined(CONFIG_ECW6280_WIFI)
    //wifi_set_scan_5g_priority(1);
#endif
    if (nv_info->sta_pwd[0] != '\0') {
        memcpy(sta_config.sta.password, nv_info->sta_pwd, strlen(nv_info->sta_pwd));
    }
    sta_config.sta.eap_method = nv_info->eap.method;
    if (nv_info->eap.identity[0] != '\0') {
        sta_config.sta.identity = nv_info->eap.identity;
        sta_config.sta.identity_len = strnlen(nv_info->eap.identity,
                                              sizeof(nv_info->eap.identity));
    }
    if (nv_info->eap.username[0] != '\0') {
        sta_config.sta.eap_username = nv_info->eap.username;
        sta_config.sta.eap_username_len = strnlen(nv_info->eap.username,
                                                  sizeof(nv_info->eap.username));
    }
    if (nv_info->eap.password[0] != '\0') {
        sta_config.sta.eap_password = nv_info->eap.password;
        sta_config.sta.eap_password_len = strnlen(nv_info->eap.password,
                                                  sizeof(nv_info->eap.password));
    }
    os_printf(LL_INFO, "##%s, has_pwd:%d\n", __func__,
              (nv_info->sta_pwd[0] != '\0'));
    return sys_wifi_sta_start(&sta_config);
}

/**
 * @brief Start STA auto-connect from NV configuration
 *
 * Attempts to connect to the AP saved in NV storage if auto-connect
 * is enabled. Typically called during system boot.
 *
 * @return SYS_OK on success, SYS_ERR if auto-connect disabled or no saved AP
 */
sys_err_t sys_wifi_sta_auto_connect_start(void)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_nv_info_t *nv_info = &wifi_ctx->nv_info;
    sys_err_t ret = SYS_ERR;

    if (nv_info->auto_conn == 1 && nv_info->sta_ssid[0] != 0) {
        ret = sys_wifi_set_sta_by_nv(NULL);
    }

    return ret;
}

/**
 * @brief Enable/disable STA auto-connect on power-up
 *
 * Configures whether the device automatically connects to the last
 * successfully connected AP when powered on.
 *
 * @param enable true: Enable auto-connect (default), false: Disable
 * @return SYS_OK on success, error code otherwise
 *
 * @note NV write optimization: Only writes to NV if value actually changes
 */
sys_err_t sys_wifi_sta_set_auto_connect(bool enable)
{
    sys_wifi_context_t *ctx = get_context();
    unsigned char new_val = enable ? 1 : 0;

    /* Skip NV write if value unchanged */
    if (ctx->nv_info.auto_conn == new_val) {
        return SYS_OK;
    }

    ctx->nv_info.auto_conn = new_val;
    return hal_system_set_config(CUSTOMER_NV_WIFI_AUTO_CONN,
                                 &ctx->nv_info.auto_conn,
                                 sizeof(ctx->nv_info.auto_conn)) ? SYS_ERR : SYS_OK;
}

/**
 * @brief Get STA auto-connect on power-up setting
 *
 * @param enable Output: true if auto-connect enabled (default), false otherwise
 * @return SYS_OK on success, SYS_ERR_INVALID_ARG if enable is NULL
 */
sys_err_t sys_wifi_sta_get_auto_connect(bool *enable)
{
    if (!enable) {
        return SYS_ERR_INVALID_ARG;
    }
    sys_wifi_context_t *ctx = get_context();
    *enable = (ctx->nv_info.auto_conn == 1);
    return SYS_OK;
}

/**
 * @brief Set STA beacon loss threshold
 *
 * Configures the time threshold for detecting beacon loss from the connected AP.
 * If no beacons are received within this time, the connection is considered lost
 * and a disconnection event is triggered.
 *
 * @param time_ms Beacon loss threshold in milliseconds
 */
void sys_wifi_sta_set_beacon_loss_thd(unsigned int time_ms)
{
    fhost_sta_set_beacon_loss_thd(time_ms);
}

/**
 * @brief Get STA beacon loss threshold
 *
 * Retrieves the current beacon loss detection threshold.
 *
 * @return Beacon loss threshold in milliseconds
 */
unsigned int sys_wifi_sta_get_beacon_loss_thd(void)
{
    return fhost_sta_get_beacon_loss_thd();
}

/**
 * @brief Set STA MAC address
 *
 * Sets STA interface MAC. AP MAC is automatically derived.
 *
 * @param mac MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 *
 * @see sys_wifi_set_mac() for setting both interfaces explicitly
 */
sys_err_t sys_wifi_sta_set_mac(unsigned char *mac)
{
    return sys_wifi_if_set_mac(WIFI_VIF_STA, mac);
}

/**
 * @brief Get STA MAC address
 *
 * @param mac Output buffer for MAC address (6 bytes)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_sta_get_mac(unsigned char *mac)
{
    return sys_wifi_if_get_mac(WIFI_VIF_STA, mac);
}

/**
 * @brief Get STA interface status
 *
 * @return STA WiFi status
 */
wifi_status_e sys_wifi_sta_get_status(void)
{
    return sys_wifi_get_status(WIFI_VIF_STA);
}

/**
 * @brief Check if STA is connected
 *
 * @return true if STA is connected to an AP, false otherwise
 */
bool sys_wifi_sta_is_connected(void)
{
    return sys_wifi_is_link_up(WIFI_VIF_STA);
}

/**
 * Ensure scan semaphore is initialized
 */
static void ensure_scan_sem_init(sys_wifi_context_t *ctx)
{
    if (!ctx->scan_sem) {
        ctx->scan_sem = os_sem_create(1, 0);
    }
}

/**
 * @brief Scan completion event handler
 *
 * Called by event system when scan completes. Signals waiting threads.
 *
 * @param vif Virtual interface ID
 * @return SYS_OK
 */
sys_err_t sys_wifi_event_scan_handler(int vif)
{
    sys_wifi_context_t *ctx = get_context();

    if (!ctx->scan_sem) {
        return SYS_OK;
    }

    if (ctx->scan_waiting) {
        os_sem_post(ctx->scan_sem);
    }

    return SYS_OK;
}

/**
 * @brief Wait for scan completion
 *
 * Blocks until scan completes or timeout expires.
 *
 * @param timeoutms Timeout in milliseconds
 * @return SYS_OK if scan completed, SYS_ERR on timeout or error
 */
sys_err_t sys_wifi_wait_scan_done(unsigned int timeoutms)
{
    BaseType_t ret;
    sys_wifi_context_t *ctx = get_context();

    ensure_scan_sem_init(ctx);

    if (!ctx->scan_sem) {
        return SYS_ERR;
    }

    ctx->scan_waiting = true;
    ret = os_sem_wait(ctx->scan_sem, timeoutms);
    ctx->scan_waiting = false;

    if (ret == 0) {
        return SYS_OK;
    }

    return SYS_ERR;
}

/**
 * @brief Start WiFi scan
 *
 * Initiates a WiFi scan with the specified configuration. Can be blocking
 * or non-blocking. Supports single-channel, multi-channel, or full-band scan.
 *
 * @param block true for blocking scan (waits for completion), false for async
 * @param config Scan configuration (NULL for default full-channel scan)
 * @return SYS_OK on success, error code otherwise
 */
static sys_err_t sys_wifi_scan_start_impl(bool block, const wifi_scan_config_t *config, bool strict_admission)
{
    wpa_scan_config_t wpa_scan;
    unsigned short *freq_buffer = NULL;
    sys_err_t ret;
    unsigned int timeout;
    unsigned int scan_time;
    wifi_scan_config_t default_config;
    const wifi_scan_config_t *scan_cfg;

    /* If config is NULL, use default full channel scan config */
    if (!config) {
        memset(&default_config, 0, sizeof(default_config));
        default_config.scan_time = 100;  // Default 100ms per channel
        default_config.channel = 0;      // 0 means full channel scan
        default_config.num_chan = 0;     // 0 means full channel scan
        scan_cfg = &default_config;
    } else {
        scan_cfg = config;
    }

    /* Type conversion: wifi_scan_config_t → wpa_scan_config_t (dynamic freq buffer) */
    sys_wifi_context_t *wifi_ctx = get_context();
    ret = cvt_scan_to_wpa(scan_cfg, &wpa_scan, &freq_buffer,
                          wifi_ctx->capability_config.band_mode);
    if (ret != SYS_OK) {
        return ret;
    }

    /* Arm before dispatch: a short scan may complete before start returns.
     * Clear only stale semaphore tokens; no scan is retried or canceled here. */
    if (block) {
        ensure_scan_sem_init(wifi_ctx);
        if (!wifi_ctx->scan_sem) {
            if (freq_buffer) {
                os_free(freq_buffer);
            }
            return SYS_ERR_NO_MEM;
        }
        (void)os_sem_wait(wifi_ctx->scan_sem, 0); /* Binary semaphore: at most one token. */
        wifi_ctx->scan_waiting = true;
    }

    /* Call WPA control interface */
    ret = strict_admission ? wpas_ctrl_scan_start_checked(WIFI_VIF_STA, &wpa_scan)
                           : wpas_ctrl_scan_start(WIFI_VIF_STA, &wpa_scan);

    /* Free dynamically allocated frequency buffer */
    if (freq_buffer) {
        os_free(freq_buffer);
        freq_buffer = NULL;
    }

    if (ret != SYS_OK) {
        if (block) {
            wifi_ctx->scan_waiting = false;
        }
        return ret;
    }

    /* If blocking wait is needed */
    if (block) {
        scan_time = scan_cfg->scan_time ? scan_cfg->scan_time : 100;

        /* Calculate timeout based on actual channel count (support 2.4G + 5G) */
        if (scan_cfg->num_chan > 0) {
            /* Use specified channel_list count */
            /* Consider channel switching overhead (~50ms/channel) + scheduling delay (2000ms buffer) */
            timeout = (scan_time + 50) * scan_cfg->num_chan + 2000;
        } else if (scan_cfg->channel > 0) {
            /* Single channel scan */
            timeout = scan_time + 1500;
        } else {
            /* Full channel scan: 2.4G(14) + 5G(28) = 42 */
            /* Consider channel switching overhead (~50ms/channel) + scheduling delay (3000ms buffer) */
            timeout = (scan_time + 50) * WIFI_SCAN_CHANNEL_MAX + 3000;
        }

        int wait_result = os_sem_wait(wifi_ctx->scan_sem, timeout);
        wifi_ctx->scan_waiting = false;
        return wait_result == 0 ? SYS_OK : SYS_ERR;
    }

    return SYS_OK;
}

sys_err_t sys_wifi_scan_start(bool block, const wifi_scan_config_t *config)
{
    return sys_wifi_scan_start_impl(block, config, false);
}

/* Additive BSP entry: BUSY never queues a deferred manual scan. */
sys_err_t sys_wifi_scan_start_checked(bool block, const wifi_scan_config_t *config)
{
    return sys_wifi_scan_start_impl(block, config, true);
}

sys_err_t sys_wifi_scan_stop(void)
{
    return wpas_ctrl_scan_stop(WIFI_VIF_STA);
}

/**
 * @brief Get number of scan results
 *
 * @return Number of available scan results
 */
int sys_wifi_scan_get_count(void)
{
    return wpas_ctrl_scan_get_count(WIFI_VIF_STA);
}

/**
 * @brief Get scan result by index
 *
 * @param index Index of scan result (0 to num-1)
 * @param info Output buffer for scan result information
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if info is NULL
 * @retval SYS_ERR if index out of range or query failed
 */
sys_err_t sys_wifi_scan_get_result(unsigned int index, wifi_info_t *info)
{
    wpa_wifi_info_t wpa_info;
    sys_err_t ret;

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 1. Call WPA query interface */
    ret = wpas_ctrl_scan_get_result_by_index(WIFI_VIF_STA, index, &wpa_info);
    if (ret != SYS_OK) {
        return ret;
    }

    /* 2. Type conversion */
    cvt_wpa_to_wifi_info(&wpa_info, info);

    return SYS_OK;

}

/**
 * @brief Get all scan results
 *
 * @param info_list Array to store scan results
 * @param count Input: array size; Output: actual number of results returned
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if info_list or count is NULL
 */
sys_err_t sys_wifi_scan_get_all_results(wifi_info_t *info_list, uint32_t *count)
{
    uint32_t scan_num;
    uint32_t i;
    sys_err_t ret;

    if (!info_list || !count) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Get scan result count */
    scan_num = sys_wifi_scan_get_count();
    if (scan_num == 0) {
        *count = 0;
        return SYS_OK;
    }

    /* Limit return count */
    if (scan_num > *count) {
        scan_num = *count;
    }

    /* Iterate through scan results */
    for (i = 0; i < scan_num; i++) {
        wpa_wifi_info_t wpa_info;

        ret = wpas_ctrl_scan_get_result_by_index(WIFI_VIF_STA, i, &wpa_info);
        if (ret != SYS_OK) {
            continue;  /* Skip failed results */
        }

        cvt_wpa_to_wifi_info(&wpa_info, &info_list[i]);
    }

    *count = scan_num;
    return SYS_OK;
}

/**
 * @brief Get scan result by SSID
 *
 * Finds and returns the scan result for a specific SSID.
 *
 * @param ssid SSID to search for
 * @param info Output buffer for scan result information
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if ssid or info is NULL
 * @retval SYS_ERR if SSID not found or query failed
 */
sys_err_t sys_wifi_scan_get_by_ssid(const char *ssid, wifi_info_t *info)
{
    wpa_wifi_info_t wpa_info;
    sys_err_t ret;

    if (!ssid || !info) {
        return SYS_ERR_INVALID_ARG;
    }

    /* 1. Call WPA query interface */
    ret = wpas_ctrl_scan_get_result_by_ssid(WIFI_VIF_STA, ssid, &wpa_info);
    if (ret != SYS_OK) {
        return ret;
    }

    /* 2. Type conversion */
    cvt_wpa_to_wifi_info(&wpa_info, info);

    return SYS_OK;
}

/**
 * @brief Set scan result expiration time
 *
 * Configures how long scan results are cached before being considered stale.
 * After the expiration time, old scan results will be removed from the BSS list.
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @param age Expiration age in seconds (minimum 10 seconds)
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if vif invalid or age < 10
 * @retval SYS_ERR_INVALID_STATE if WPA supplicant not initialized
 * @retval SYS_ERR if WPA internal operation failed
 */
sys_err_t sys_wifi_scan_set_expire(int vif, unsigned int age)
{
    if (!IS_VALID_VIF(vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_scan_set_expire((unsigned char)vif, age);
}

/**
 * @brief Flush BSS scan results
 * @param vif Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param flush_age Age in seconds (0 = flush all, >0 = flush entries older than this)
 * @return sys_err_t Execution result
 */
sys_err_t sys_wifi_flush_scan_results(wifi_vif_e vif, int flush_age)
{
    if (!IS_VALID_VIF(vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    return wpas_ctrl_scan_flush_bss((unsigned char)vif, flush_age);
}

/**
 * @brief Set 5GHz scan priority
 *
 * When enabled, full-band scans start with 5GHz before 2.4GHz.
 *
 * @param enable 1 to enable 5GHz priority, 0 to disable
 * @return 0 on success
 */
int sys_wifi_scan_set_5g_preferred(int enable)
{
    if (enable != 0 && enable != 1) {
        return -1;
    }

    return fhost_set_5g_scan_priority(enable);
}

/**
 * @brief Set WiFi scan method
 *
 * Configures the scan strategy (e.g., fast scan vs. full scan).
 * Fast scan reduces connection time by stopping after finding a matching SSID.
 *
 * @param method Scan method (WIFI_FAST_SCAN or WIFI_ALL_CHANNEL_SCAN)
 * @return 0 on success
 */
int sys_wifi_scan_set_method(wifi_scan_method_e method)
{
    if (method != WIFI_ALL_CHANNEL_SCAN && method != WIFI_FAST_SCAN) {
        return -1;
    }

    return fhost_set_scan_method(method);
}

/**
 * @brief Load WiFi configuration from NV storage
 *
 * Loads WiFi STA configuration including SSID, password, BSSID, channel,
 * and autoconnect flag from non-volatile storage. If nv_info is NULL,
 * loads into internal WiFi context.
 *
 * @param nv_info Output buffer for NV information (NULL to use internal context)
 * @return SYS_OK on success
 * @retval SYS_ERR if required NV entries (SSID/BSSID/channel) not found
 */
sys_err_t sys_wifi_load_nv_info(wifi_nv_info_t *nv_info)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    if (!nv_info) {
        nv_info = &wifi_ctx->nv_info;
    }

    memset(nv_info, 0, sizeof(*nv_info));
#ifdef CONFIG_WIFI_DISABLE_POWERON_NV_AUTO_CONNECT
    nv_info->auto_conn = 0;
#else
    if( 0 >= hal_system_get_config(CUSTOMER_NV_WIFI_AUTO_CONN, &nv_info->auto_conn, sizeof(nv_info->auto_conn))){
        nv_info->auto_conn = 1;
    }
#endif
    
    if (0 >= hal_system_get_config(CUSTOMER_NV_WIFI_STA_SSID, nv_info->sta_ssid, sizeof(nv_info->sta_ssid) - 1) ||
        0 >= hal_system_get_config(CUSTOMER_NV_WIFI_STA_BSSID, nv_info->sta_bssid, sizeof(nv_info->sta_bssid)) ||
        0 >= hal_system_get_config(CUSTOMER_NV_WIFI_STA_CHANNEL, &nv_info->channel, sizeof(nv_info->channel))) {
        SYS_LOGE("load wifi nv fail:%d.", nv_info->auto_conn);
        return SYS_ERR;
    }
    nv_info->sync = 1;

    hal_system_get_config(CUSTOMER_NV_WIFI_STA_PWD, nv_info->sta_pwd, sizeof(nv_info->sta_pwd) - 1);
    hal_system_get_config(CUSTOMER_NV_WIFI_STA_EAP_CFG, &nv_info->eap, sizeof(nv_info->eap));

    /* get_config returns copied length: read an extra byte to reject oversized keys. */
    unsigned char candidate[sizeof(nv_info->candidate_channel) + 1] = {0};
    if (hal_system_get_config(CUSTOMER_NV_WIFI_STA_CANDIDATE_CHANNEL,
                             candidate, sizeof(candidate)) == sizeof(nv_info->candidate_channel) &&
        nv_info->channel && candidate[0] && sys_wifi_channel_to_freq(candidate[0])) {
        nv_info->candidate_channel = candidate[0];
    }

    SYS_LOGD("load wifi nv:ssid[%s],has_pwd[%d],channel[%d],auto[%d]",
        nv_info->sta_ssid, nv_info->sta_pwd[0] != '\0', nv_info->channel, nv_info->auto_conn);
    return SYS_OK;

}

/**
 * @brief Save WiFi configuration to NV storage
 *
 * Saves WiFi STA configuration (SSID, password, BSSID, channel, autoconnect)
 * to non-volatile storage. On failure, performs rollback by deleting all
 * STA NV entries to maintain consistency. In VNET_SERVICE mode, also deletes
 * AP configuration on success.
 *
 * Implementation uses bit-or (ret |=) to accumulate errors from multiple
 * hal_system_set_config() calls. This simplifies error handling but only
 * indicates "some error occurred" rather than which specific write failed.
 *
 * @param nv_info WiFi configuration to save
 * @return SYS_OK on success
 * @retval SYS_ERR_INVALID_ARG if nv_info is NULL
 * @retval SYS_OK if SSID is empty (no-op)
 * @retval SYS_ERR if NV write failed (after rollback)
 */
sys_err_t sys_wifi_save_nv_info(wifi_nv_info_t *nv_info)
{
    int ret = 0;

    if (!nv_info)
        return SYS_ERR_INVALID_ARG;
    if (!nv_info->sta_ssid[0])
       return SYS_OK;
       
    wifi_band_e band = sys_wifi_channel_to_band(nv_info->channel);
    if (band == WIFI_BAND_UNKNOWN || !sys_wifi_channel_to_freq(nv_info->candidate_channel) ||
        sys_wifi_channel_to_band(nv_info->candidate_channel) == band) {
        nv_info->candidate_channel = 0;
    }
    nv_info->sync = 0;
    memcpy(&get_context()->nv_info, nv_info, sizeof(wifi_nv_info_t));

    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_STA_SSID, nv_info->sta_ssid,
                                 strnlen(nv_info->sta_ssid, WIFI_SSID_PROTOCOL_MAX_LEN));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_STA_PWD, nv_info->sta_pwd,
                                 strnlen(nv_info->sta_pwd, WIFI_PWD_PROTOCOL_MAX_LEN));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_STA_BSSID, nv_info->sta_bssid, sizeof(nv_info->sta_bssid));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_STA_CHANNEL, &nv_info->channel, sizeof(nv_info->channel));
    ret |= hal_system_set_config(CUSTOMER_NV_WIFI_AUTO_CONN, &nv_info->auto_conn, sizeof(nv_info->auto_conn));
    if (nv_info->eap.method != WIFI_STA_EAP_NONE) {
        ret |= hal_system_set_config(CUSTOMER_NV_WIFI_STA_EAP_CFG, &nv_info->eap, sizeof(nv_info->eap));
    } else {
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_EAP_CFG);
    }

    SYS_LOGE("%d, %d, %d", strlen(nv_info->sta_ssid), strlen(nv_info->sta_pwd), sizeof(nv_info->channel));
    SYS_LOGE("save wifi nv:ssid[%s],has_pwd[%d],channel[%d],auto[%d].",
        nv_info->sta_ssid, nv_info->sta_pwd[0] != '\0', nv_info->channel, nv_info->auto_conn);
    if (ret) { //roll back.
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_SSID);
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_PWD);
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_BSSID);
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_CHANNEL);
        hal_system_del_config(CUSTOMER_NV_WIFI_AUTO_CONN);
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_EAP_CFG);
        hal_system_del_config(CUSTOMER_NV_WIFI_STA_CANDIDATE_CHANNEL);
        return SYS_ERR;
    }

    if (nv_info->candidate_channel) {
        ret = hal_system_set_config(CUSTOMER_NV_WIFI_STA_CANDIDATE_CHANNEL,
                                    &nv_info->candidate_channel, sizeof(nv_info->candidate_channel));
    } else {
        ret = hal_system_del_config(CUSTOMER_NV_WIFI_STA_CANDIDATE_CHANNEL);
    }
    if (ret) {
        return SYS_ERR;
    }
    nv_info->sync = 1;
    get_context()->nv_info.sync = 1;

    return SYS_OK;
}

/**
 * @brief Clear WiFi configuration from NV storage
 *
 * Deletes all STA-related WiFi configuration entries from non-volatile storage,
 * including SSID, password, BSSID, channel, and autoconnect flag.
 *
 * @return SYS_OK on success
 */
sys_err_t sys_wifi_clean_nv_info(void)
{
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_CANDIDATE_CHANNEL);
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_SSID);
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_PWD);
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_BSSID);
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_CHANNEL);
    hal_system_del_config(CUSTOMER_NV_WIFI_AUTO_CONN);
    hal_system_del_config(CUSTOMER_NV_WIFI_STA_EAP_CFG);
    SYS_LOGE("sys_wifi_clean_nv_info");

    return SYS_OK;
}

/****************************************************************************
 *                    Country Code NV Management (Internal)
 ****************************************************************************/

/**
 * @brief Save country configuration to NV storage (internal)
 *
 * Low-level NV write operation.
 *
 * @param nv Country data to save
 * @return SYS_OK on success, SYS_ERR on failure
 */
static sys_err_t save_country_to_nv(const wifi_country_nv_t *nv)
{
    int ret;

    if (!nv) {
        return SYS_ERR_INVALID_ARG;
    }

    ret = hal_system_set_config(CUSTOMER_NV_WIFI_COUNTRYCODE,
                                  (void *)nv,
                                  sizeof(wifi_country_nv_t));
    if (ret != 0) {
        SYS_LOGE("Failed to save country to NV");
        return SYS_ERR;
    }

    SYS_LOGI("Saved country NV: code=[%c%c], policy=%d, schan_2g=%d, nchan_2g=%d",
             nv->country_code[0] ? nv->country_code[0] : '?',
             nv->country_code[1] ? nv->country_code[1] : '?',
             nv->policy, nv->schan_2g, nv->nchan_2g);

    return SYS_OK;
}

/**
 * @brief Save WiFi capability configuration to NV
 *
 * Saves the current capability configuration (band mode, bandwidth, protocol bitmap)
 * to non-volatile storage. Called automatically when configuration changes.
 *
 * @return SYS_OK on success, SYS_ERR on failure
 */
static sys_err_t save_capability_config_to_nv(void)
{
    sys_wifi_context_t *ctx = get_context();
    int ret;

    ret = hal_system_set_config(CUSTOMER_NV_WIFI_CAPABILITY_CONFIG,
                                 (void *)&ctx->capability_config,
                                 sizeof(wifi_capability_config_t));
    if (ret != 0) {
        SYS_LOGE("Failed to save capability config to NV (ret=%d)", ret);
        return SYS_ERR;
    }

    SYS_LOGI("Saved capability config to NV: band=%d, STA_bw(2g=%d,5g=%d), AP_bw(2g=%d,5g=%d), STA_proto(2g=0x%04x,5g=0x%04x), AP_proto(2g=0x%04x,5g=0x%04x)",
             ctx->capability_config.band_mode,
             ctx->capability_config.bandwidth[WIFI_VIF_STA].ghz_2g,
             ctx->capability_config.bandwidth[WIFI_VIF_STA].ghz_5g,
             ctx->capability_config.bandwidth[WIFI_VIF_AP].ghz_2g,
             ctx->capability_config.bandwidth[WIFI_VIF_AP].ghz_5g,
             ctx->capability_config.protocol_bitmap[WIFI_VIF_STA].ghz_2g,
             ctx->capability_config.protocol_bitmap[WIFI_VIF_STA].ghz_5g,
             ctx->capability_config.protocol_bitmap[WIFI_VIF_AP].ghz_2g,
             ctx->capability_config.protocol_bitmap[WIFI_VIF_AP].ghz_5g);

    return SYS_OK;
}

/**
 * @brief Convert wifi_country_info_t to wifi_country_nv_t (internal)
 *
 * @param info Source info structure
 * @param nv Destination NV structure
 */
static void convert_info_to_nv(const wifi_country_info_t *info, wifi_country_nv_t *nv)
{
    memset(nv, 0, sizeof(wifi_country_nv_t));

    nv->policy = (uint8_t)info->policy;

    /* Copy country code */
    if (info->country_code[0]) {
        nv->country_code[0] = info->country_code[0];
        nv->country_code[1] = info->country_code[1];
        nv->country_code[2] = '\0';
    }

    /* Copy 2.4G parameters (store original values, may be 0) */
    nv->schan_2g = info->schan_2g;
    nv->nchan_2g = info->nchan_2g;
}

/* Select the candidate channel and update runtime scan hints. */
static unsigned char sys_wifi_learn_candidate(const wifi_info_t *info, bool same_network)
{
    sys_wifi_context_t *ctx = get_context();
    struct wpas_network_scan_result result;
    unsigned char candidate = same_network ? ctx->nv_info.candidate_channel : 0;
    wifi_band_e band = sys_wifi_channel_to_band(info->channel);
    wifi_band_e previous_band = sys_wifi_channel_to_band(ctx->nv_info.channel);

    if (band == WIFI_BAND_UNKNOWN || wpas_ctrl_network_get_scan_result(WIFI_VIF_STA, &result) != SYS_OK ||
        !result.connected || result.ssid_len != info->ssid_len ||
        memcmp(result.ssid, info->ssid, info->ssid_len) ||
        memcmp(result.bssid, info->bssid, sizeof(result.bssid))) {
        return candidate;
    }
    if (same_network && previous_band != WIFI_BAND_UNKNOWN && previous_band != band) {
        candidate = ctx->nv_info.channel;
    }
    if (result.bssid_set) {
        candidate = 0;
    } else {
        unsigned short frequency = band == WIFI_BAND_2G ?
                                   result.selected_freq_5g : result.selected_freq_2g;
        unsigned char channel = frequency ? sys_wifi_freq_to_channel(frequency) : 0;

        if (channel && validate_channel_band_compatibility(channel, ctx->capability_config.band_mode)) {
            candidate = channel;
        }
    }
    if (candidate && (!sys_wifi_channel_to_freq(candidate) ||
        sys_wifi_channel_to_band(candidate) == band)) {
        candidate = 0;
    }
#ifdef CONFIG_FAST_CONNECT
    if (!result.candidate_frequency) {
        wpas_network_scan_hint_t hint = {
            .frequency = sys_wifi_channel_to_freq(info->channel),
            .candidate_frequency = validate_channel_band_compatibility(candidate, ctx->capability_config.band_mode) ?
                                   sys_wifi_channel_to_freq(candidate) : 0
        };
        wpas_ctrl_network_update_scan_hint(WIFI_VIF_STA, &hint);
    }
#endif
    SYS_LOGD("STA channel %u candidate %u", info->channel, candidate);
    return candidate;
}

/**
 * @brief Update NV storage with current STA connection info
 *
 * This function is called when STA connection is established.
 * It compares current connection parameters with NV stored values,
 * and saves to NV if any parameter has changed.
 *
 * @return SYS_OK on success, SYS_ERR on failure
 */
static sys_err_t nv_update_sta_connected(void)
{
    sys_wifi_context_t *wifi_ctx = get_context();
    wifi_nv_info_t *nv_info = &wifi_ctx->nv_info;
    wifi_info_t info;
    sys_err_t result;
    bool nv_changed = false;

    /* Get current STA connection info */
    result = sys_wifi_sta_get_info(&info);
    if (result != SYS_OK) {
        SYS_LOGE("%s: failed to get STA info", __func__);
        return SYS_ERR;
    }

    if (wifi_ctx->sta_eap_nv_dirty) {
        nv_changed = true;
        wifi_ctx->sta_eap_nv_dirty = false;
    }

    /* Compare and update SSID */
    size_t nv_ssid_len = strnlen(nv_info->sta_ssid, WIFI_SSID_PROTOCOL_MAX_LEN);
    if (nv_ssid_len != info.ssid_len ||
        memcmp(nv_info->sta_ssid, info.ssid, info.ssid_len) != 0) {
        nv_changed = true;
        memset(nv_info->sta_ssid, 0, WIFI_SSID_MAX_LEN);
        memcpy(nv_info->sta_ssid, (char *)info.ssid, info.ssid_len);
    }

    /* Compare and update password */
    size_t nv_pwd_len = strnlen(nv_info->sta_pwd, WIFI_PWD_PROTOCOL_MAX_LEN);
    if (nv_pwd_len != info.pwd_len ||
        memcmp(nv_info->sta_pwd, info.pwd, info.pwd_len) != 0) {
        nv_changed = true;
        memset(nv_info->sta_pwd, 0, WIFI_PWD_MAX_LEN);
        memcpy(nv_info->sta_pwd, (char *)info.pwd, info.pwd_len);
    }

    /* At this point nv_changed only reflects SSID, password or EAP changes. */
    unsigned char candidate = sys_wifi_learn_candidate(&info, !nv_changed);
    if (nv_info->candidate_channel != candidate) {
        nv_info->candidate_channel = candidate;
        nv_changed = true;
    }

    /* Compare and update BSSID (skip zero/multicast MAC) */
    if (!(IS_ZERO_MAC(info.bssid) || IS_MULTCAST_MAC(info.bssid))) {
        if (memcmp(nv_info->sta_bssid, info.bssid, 6) != 0) {
            nv_changed = true;
            memcpy(nv_info->sta_bssid, info.bssid, 6);
        }
    }

    /* Compare and update channel */
    if (nv_info->channel != info.channel) {
        nv_changed = true;
        nv_info->channel = info.channel;
    }

    /* Keep RAM learning active when automatic Flash writes are disabled. */
    if (nv_changed) {
        nv_info->sync = 0;
    }
    if (wifi_ctx->policy.auto_save_nv && !nv_info->sync) {
        result = sys_wifi_save_nv_info(nv_info);
        if (result != SYS_OK) {
            SYS_LOGE("Failed to save NV info");
            return SYS_ERR;
        }
    }

    return SYS_OK;
}

/**
 * @brief Set auto-save NV on STA connection policy
 *
 * When enabled, WiFi configuration is automatically saved to NV storage
 * whenever STA successfully connects to an AP. When disabled, configuration
 * must be saved manually using sys_wifi_save_nv_info().
 *
 * @param enable true to enable auto-save, false to disable
 */
void sys_wifi_set_auto_save_nv(bool enable)
{
    sys_wifi_context_t *ctx = get_context();
    ctx->policy.auto_save_nv = enable;
}

/**
 * @brief Get auto-save NV policy status
 *
 * @return true if auto-save is enabled, false otherwise
 */
bool sys_wifi_get_auto_save_nv(void)
{
    sys_wifi_context_t *ctx = get_context();
    return ctx->policy.auto_save_nv;
}

/****************************************************************************
 *                    Country Code Configuration (Public)
 ****************************************************************************/

/**
 * @brief Set WiFi country information (internal implementation)
 *
 * Internal function that optionally saves to NV based on auto_save flag.
 * Runtime country synchronization skips NV writes.
 *
 * @param info Country information structure
 * @param auto_save true to auto-save to NV on success, false to skip NV save
 * @return 0 on success, -1 on failure
 */
static int set_country_info_internal(const wifi_country_info_t *info, bool auto_save)
{
    int ret;
    wifi_country_nv_t nv;

    if (!info) {
        SYS_LOGE("%s: info is NULL", __func__);
        return -1;
    }

    /* Critical validation: country_code cannot be empty */
    if (!info->country_code[0] || info->country_code[0] == 0xFF) {
        SYS_LOGE("%s: country_code cannot be empty", __func__);
        return -1;
    }

    /* Call WPA layer to configure (0-value auto-fill happens in driver layer) */
    ret = wpas_set_country_info(info);
    if (ret != 0) {
        SYS_LOGE("%s: wpas_set_country_info failed", __func__);
        return ret;
    }

    /* Auto-save to NV if requested */
    if (auto_save) {
        /* Convert user input (original values with 0s) to NV format */
        convert_info_to_nv(info, &nv);

        /* Save to NV */
        if (save_country_to_nv(&nv) != SYS_OK) {
            SYS_LOGW("Country configured but NV save failed");
            /* Non-fatal: configuration succeeded, NV save is optional */
        }
    }

    return 0;
}

/**
 * @brief Set WiFi country information (public interface)
 *
 * Configures WiFi regulatory domain and automatically saves to NV.
 *
 * @param info Country information structure
 * @return 0 on success, -1 on failure
 */
int sys_wifi_set_country_info(const wifi_country_info_t *info)
{
    /* Call internal function with auto_save=true */
    return set_country_info_internal(info, true);
}

/**
 * @brief Get current WiFi country information
 *
 * @param info Output buffer for country information
 * @return 0 on success, -1 on failure
 */
int sys_wifi_get_country_info(wifi_country_info_t *info)
{
    if (!info) {
        SYS_LOGE("%s: info is NULL", __func__);
        return -1;
    }

    /* Call WPA layer to retrieve */
    return wpas_get_country_info(info);
}

extern uint32_t wifi_5g_channel_to_bit(uint8_t channel);

bool sys_wifi_is_valid_ap_channel(uint8_t channel)
{
    wifi_country_info_t info;

    if (sys_wifi_get_country_info(&info) != 0) {
        return false;
    }

    if (sys_wifi_channel_to_band(channel) == WIFI_BAND_2G) {
        /* 2.4GHz channel - validate against country range */
        return (channel >= info.schan_2g &&
                channel < info.schan_2g + info.nchan_2g);
    }

    /* 5GHz channel - validate against bitmap */
    uint32_t bit = wifi_5g_channel_to_bit(channel);
    return (bit != 0) && ((info.chan_bitmap_5g & bit) != 0);
}

/**
 * @brief Set WiFi country code (legacy interface)
 *
 * Legacy compatibility wrapper. Constructs wifi_country_info_t with
 * 0 values (auto-filled from regulatory database) and calls new interface.
 *
 * @param country_code ISO country code (2 characters)
 * @return 0 on success, -1 on failure
 */
int sys_wifi_set_country_code(const char *country_code)
{
    wifi_country_info_t info;

    if (!country_code || strlen(country_code) != 2) {
        SYS_LOGE("%s: invalid country_code", __func__);
        return -1;
    }

    /* Construct wifi_country_info_t with 0 values (will be auto-filled) */
    memset(&info, 0, sizeof(info));

    info.policy = WIFI_COUNTRY_POLICY_MANUAL;
    info.country_code[0] = country_code[0];
    info.country_code[1] = country_code[1];
    info.country_code[2] = '\0';
    info.domain = REG_DOMAIN_NONE;
    info.schan_2g = 0;  /* Auto-fill from regulatory DB */
    info.nchan_2g = 0;  /* Auto-fill from regulatory DB */
    info.power_limit_2g4 = NULL;  /* Auto-fill from regulatory DB */
    info.chan_bitmap_5g = 0;
    info.power_limit_5g = NULL;

    /* Call new interface (auto-saves to NV) */
    return sys_wifi_set_country_info(&info);
}

/**
 * @brief Get WiFi country code (legacy interface)
 *
 * Legacy compatibility wrapper. Calls new interface and extracts
 * only the country code field.
 *
 * @param country_code Output buffer (at least 3 bytes)
 * @return 0 on success, -1 on failure
 */
int sys_wifi_get_country_code(char *country_code)
{
    wifi_country_info_t info;

    if (!country_code) {
        SYS_LOGE("%s: country_code is NULL", __func__);
        return -1;
    }

    if (sys_wifi_get_country_info(&info) != 0) {
        SYS_LOGE("%s: failed to get country info", __func__);
        return -1;
    }

    /* Copy country code */
    country_code[0] = info.country_code[0];
    country_code[1] = info.country_code[1];
    country_code[2] = '\0';

    return 0;
}

/**
 * @brief Set WPA supplicant log level
 *
 * Configures the verbosity of WPA supplicant debug logging.
 *
 * @param level Log level (0=EXCESSIVE, 1=MSGDUMP, 2=DEBUG, 3=INFO, 4=WARNING, 5=ERROR)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_wpa_set_log_level(int level)
{
    return wpas_ctrl_system_set_log_level(level);
}

/**
 * @brief Dump WPA supplicant status to console
 *
 * Outputs detailed WPA supplicant status information for the STA interface,
 * including connection state, SSID, BSSID, key management, etc.
 * Used for debugging purposes.
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_wpa_dump_status(unsigned char vif)
{
    return wpas_ctrl_iface_dump_status(vif);
}

/**
 * @brief Dump configured network list to console
 *
 * Outputs the list of configured networks for debugging purposes.
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_wpa_dump_list_networks(unsigned char vif)
{
    return wpas_ctrl_iface_dump_list_networks(vif);
}

/**
 * @brief Clear WPA PMK cache
 *
 * Clears cached PMK state for the selected WPA supplicant interface.
 *
 * @param vif Virtual interface ID (WIFI_VIF_STA or WIFI_VIF_AP)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_pmk_cache_clean(unsigned char vif)
{
    return wpas_ctrl_pmk_cache_clean(vif);
}

/****************************************************************************
* 	                          Event Subscription System
****************************************************************************/
sys_err_t evt_on_wifi_ready(void *ctx, system_event_t *event)
{
    (void)ctx;
    (void)event;

    /* Sync cached configuration to driver (now safe - all threads running) */
    sys_wifi_init_late();

    /* Mark WiFi subsystem as ready (after late init completes) */
    sys_wifi_set_ready(true);

    return SYS_OK;
}

static sys_err_t evt_on_sta_channel_changed(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    wifi_nv_info_t *nv_info = &ctx->nv_info;
    wifi_info_t info;
    wifi_band_e band;
    unsigned char candidate;

    if (!event || event->vif != WIFI_VIF_STA) {
        return SYS_ERR_INVALID_ARG;
    }
    system_event_sta_channel_changed_t *change = &event->event_info.channel_changed;
    band = sys_wifi_channel_to_band(change->channel);
    if (band == WIFI_BAND_UNKNOWN || sys_wifi_sta_get_info(&info) != SYS_OK ||
        memcmp(info.bssid, change->bssid, sizeof(info.bssid)) || info.channel != change->channel) {
        return SYS_ERR_INVALID_STATE;
    }
    ctx->interfaces[WIFI_VIF_STA].channel = info.channel;

    /* Only update the saved network; CSA must not relearn stale scan results. */
    if (memcmp(nv_info->sta_bssid, info.bssid, sizeof(info.bssid)) ||
        strnlen(nv_info->sta_ssid, WIFI_SSID_PROTOCOL_MAX_LEN) != info.ssid_len ||
        memcmp(nv_info->sta_ssid, info.ssid, info.ssid_len)) {
        return SYS_OK;
    }
    candidate = nv_info->candidate_channel;
    if (candidate && (!sys_wifi_channel_to_freq(candidate) ||
        sys_wifi_channel_to_band(candidate) == band)) {
        candidate = 0;
    }
    if (nv_info->channel != info.channel || nv_info->candidate_channel != candidate) {
        nv_info->channel = info.channel;
        nv_info->candidate_channel = candidate;
        nv_info->sync = 0;
    }
    if (ctx->policy.auto_save_nv && !nv_info->sync) {
        return sys_wifi_save_nv_info(nv_info);
    }
    return SYS_OK;
}

static sys_err_t evt_on_sta_connected(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    wifi_country_info_t ap_country = {0};
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    // WiFi state management
    ctx->interfaces[event->vif].status = STA_STATUS_CONNECTED;
    ctx->interfaces[event->vif].channel = event->event_info.connected.channel;
    os_memcpy(ctx->interfaces[event->vif].bssid,
              event->event_info.connected.bssid, 6);

    nv_update_sta_connected();

    if (wpas_ctrl_station_get_ap_country_info((uint8_t)event->vif, &ap_country) == SYS_OK &&
        wpas_ctrl_update_country_on_sta_event(&ap_country) != 0) {
        SYS_LOGW("Failed to sync AUTO country info on STA connect");
    }

    // AP channel sync (according to policy)
    sys_wifi_ap_sync_with_sta_channel();

    return SYS_OK;
}

static sys_err_t evt_on_sta_roamed(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    ctx->interfaces[event->vif].status = STA_STATUS_CONNECTED;
    ctx->interfaces[event->vif].channel = event->event_info.roamed.channel;
    os_memcpy(ctx->interfaces[event->vif].bssid,
              event->event_info.roamed.bssid, 6);

    sys_wifi_ap_sync_with_sta_channel();
    return SYS_OK;
}

static sys_err_t evt_on_sta_disconnected(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    if (ctx->interfaces[event->vif].status != STA_STATUS_STOP) {
        ctx->interfaces[event->vif].status = STA_STATUS_DISCON;
    }

    ctx->interfaces[event->vif].channel = 0;

    if (event->vif == WIFI_VIF_STA &&
        wpas_ctrl_update_country_on_sta_event(NULL) != 0) {
        SYS_LOGW("Failed to restore AUTO country info on STA disconnect");
    }

    return SYS_OK;
}

static sys_err_t evt_on_ap_start(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    ctx->interfaces[event->vif].status = AP_STATUS_STARTED;

    sys_wifi_ap_sync_with_sta_channel();

    return SYS_OK;
}

static sys_err_t evt_on_ap_stop(void *user_ctx, system_event_t *event)
{
    sys_wifi_context_t *ctx = get_context();
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    ctx->interfaces[event->vif].status = AP_STATUS_STOP;

    return SYS_OK;
}

static sys_err_t evt_on_scan_done(void *user_ctx, system_event_t *event)
{
    (void)user_ctx;

    if (!event || !IS_VALID_VIF(event->vif)) {
        return SYS_ERR_INVALID_ARG;
    }

    // Call scan handler
    sys_wifi_event_scan_handler(event->vif);

    return SYS_OK;
}

/**
 * @brief Initialize WiFi event subscriptions
 *
 * Registers event handlers for WiFi system events including WiFi ready,
 * STA connection/disconnection, AP start/stop, and scan completion.
 * This function is called during WiFi system initialization.
 */
void sys_wifi_event_init(void)
{
    sys_event_subscribe_internal(SYSTEM_EVENT_WIFI_READY,
                                  evt_on_wifi_ready,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_STA_CONNECTED,
                                  evt_on_sta_connected,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_STA_CHANNEL_CHANGED,
                                  evt_on_sta_channel_changed,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_STA_ROAMED,
                                  evt_on_sta_roamed,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_STA_DISCONNECTED,
                                  evt_on_sta_disconnected,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_AP_START,
                                  evt_on_ap_start,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_AP_STOP,
                                  evt_on_ap_stop,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    sys_event_subscribe_internal(SYSTEM_EVENT_SCAN_DONE,
                                  evt_on_scan_done,
                                  EVENT_FW_PRIO_CRITICAL,
                                  NULL);

    SYS_LOGI("WiFi event subscriptions initialized");
}

/****************************************************************************
 *                  WiFi Capability Configuration APIs
 ****************************************************************************/

/**
 * @brief Auto-correct protocol bitmap based on dependencies and band constraints
 *
 * Automatically corrects protocol bitmap to ensure valid combinations:
 * 1. Protocol dependency chain: AC/AX → N → G
 * 2. Band-specific base protocols: 2.4G needs B or G; 5G needs G
 * 3. 5G band exclusion: Remove 11B from 5G
 *
 * @param protocol_bitmap  Input protocol bitmap
 * @param band_mode       Current band mode
 * @return Auto-corrected protocol bitmap
 *
 * @par Supported Combinations:
 *   2.4G: b, g, bg, bgn, bgnac, bgnacax, gn, gnac, gnacax
 *   5G:   g, gn, gnac, gnacax
 *
 * @note Logs warnings when auto-correction occurs
 */
/**
 * @brief Set WiFi band mode (global configuration)
 * @param mode  Band mode (AUTO/2G_ONLY/5G_ONLY)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_band_mode(wifi_band_mode_e mode)
{
    sys_wifi_context_t *ctx = get_context();
    wifi_capability_config_t defaults;
    wifi_protocol_t adjusted[WIFI_VIF_CFG_MAX];
    bool changed[WIFI_VIF_CFG_MAX];
    int vif, undo;
    int ret;
    int refresh_ret = SYS_OK;

    // Validate band mode parameter
    if (mode >= WIFI_BAND_MODE_MAX) {
        SYS_LOGE("Invalid band mode: %d", mode);
        return SYS_ERR_INVALID_ARG;
    }

    // Chip defaults, used to repopulate a band this function had cleared on an
    // earlier call. On a single-band chip the unsupported band defaults to 0,
    // so restoring cannot enable a band the hardware does not have.
    init_capability_defaults(&defaults);

    // Work out the target protocol of every VIF without touching the context,
    // so a failure below leaves the previous configuration intact.
    for (vif = 0; vif <= WIFI_VIF_AP; vif++) {
        wifi_protocol_t proto = ctx->capability_config.protocol_bitmap[vif];

        switch (mode) {
            case WIFI_BAND_MODE_2G_ONLY:
                proto.ghz_5g = 0;  // Clear 5G configuration
                break;
            case WIFI_BAND_MODE_5G_ONLY:
                proto.ghz_2g = 0;  // Clear 2.4G configuration
                break;
            case WIFI_BAND_MODE_AUTO:
                // A band left empty by a previous 2G_ONLY/5G_ONLY call was
                // persisted as 0, so the value the user configured is gone.
                // Both bands are active in AUTO and an empty bitmap would leave
                // the band with no PHY at all, so fall back to the chip default.
                if (proto.ghz_2g == 0) {
                    proto.ghz_2g = defaults.protocol_bitmap[vif].ghz_2g;
                }
                if (proto.ghz_5g == 0) {
                    proto.ghz_5g = defaults.protocol_bitmap[vif].ghz_5g;
                }
                break;
            default:
                break;
        }

        adjusted[vif] = proto;
        changed[vif] = (proto.ghz_2g != ctx->capability_config.protocol_bitmap[vif].ghz_2g) ||
                       (proto.ghz_5g != ctx->capability_config.protocol_bitmap[vif].ghz_5g);
    }

    // Sync to fhost layer
    ret = fhost_set_band_mode(mode);
    if (ret != 0) {
        SYS_LOGE("Failed to sync band mode to fhost layer (ret=%d)", ret);
        return SYS_ERR_WIFI_FAIL;
    }

    // Sync the adjusted protocols. The context still holds the previous values,
    // so it is the source to roll back from if one of these is rejected.
    for (vif = 0; vif <= WIFI_VIF_AP; vif++) {
        if (!changed[vif]) {
            continue;
        }

        ret = fhost_set_protocol(vif, adjusted[vif]);
        if (ret != 0) {
            SYS_LOGE("Failed to sync protocol to fhost layer (vif=%d, ret=%d), rolling back", vif, ret);
            // Restoring a value that was accepted before is not expected to
            // fail, but if it does the context no longer describes the fhost
            // layer and only a log can say so.
            for (undo = 0; undo < vif; undo++) {
                if (changed[undo] &&
                    fhost_set_protocol(undo, ctx->capability_config.protocol_bitmap[undo]) != 0) {
                    SYS_LOGE("Rollback of VIF %d protocol failed, fhost layer is out of sync", undo);
                }
            }
            if (fhost_set_band_mode(ctx->capability_config.band_mode) != 0) {
                SYS_LOGE("Rollback of band mode failed, fhost layer is out of sync");
            }
            return SYS_ERR_WIFI_FAIL;
        }
    }

    // Everything accepted, commit to the context
    ctx->capability_config.band_mode = mode;
    for (vif = 0; vif <= WIFI_VIF_AP; vif++) {
        if (changed[vif]) {
            SYS_LOGW("VIF %d protocol adjusted for band mode %d: 2.4G 0x%04x->0x%04x, 5G 0x%04x->0x%04x",
                     vif, mode,
                     ctx->capability_config.protocol_bitmap[vif].ghz_2g, adjusted[vif].ghz_2g,
                     ctx->capability_config.protocol_bitmap[vif].ghz_5g, adjusted[vif].ghz_5g);
            ctx->capability_config.protocol_bitmap[vif] = adjusted[vif];
        }
    }

    // Band filtering also changes modes when protocol bitmaps stay unchanged.
    // Refresh both VIFs, including retries after a previous refresh failed.
    for (vif = 0; vif <= WIFI_VIF_AP; vif++) {
        ret = wpas_ctrl_refresh_hw_feature(vif);
        if (ret != 0) {
            SYS_LOGW("Failed to refresh hw_feature cache (vif=%d, ret=%d), hardware cache unavailable",
                     vif, ret);
            refresh_ret = SYS_ERR_WIFI_FAIL;
        }
    }

    if (refresh_ret != SYS_OK) {
        return refresh_ret;
    }

    // Save to NV (non-blocking, log warning on failure)
    ret = save_capability_config_to_nv();
    if (ret != SYS_OK) {
        SYS_LOGW("Failed to persist to NV, config will be lost on reboot");
    }

    SYS_LOGI("Band mode set to %d", mode);
    return SYS_OK;
}

/**
 * @brief Get WiFi band mode
 * @param mode  Pointer to store band mode
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_get_band_mode(wifi_band_mode_e *mode)
{
    if (!mode) {
        return SYS_ERR_INVALID_ARG;
    }

    sys_wifi_context_t *ctx = get_context();
    *mode = ctx->capability_config.band_mode;

    return SYS_OK;
}

/**
 * @brief Set WiFi bandwidth per VIF (internal use only)
 *
 * Internal function for setting bandwidth configuration per virtual interface.
 * External callers should use sys_wifi_sta_set_bandwidth() instead.
 *
 * @param vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param bw   Bandwidth structure (contains 2.4G and 5G configs)
 * @return SYS_OK on success, error code otherwise
 *
 * @note This is an internal function. Do not call directly from application code.
 * @note Current SoftAP mode only supports 20MHz or AUTO bandwidth (software limitation)
 * @note Configuration is saved to NV storage and synchronized to fhost layer
 */
static sys_err_t sys_wifi_set_bandwidth(wifi_vif_e vif, wifi_bandwidth_t bw)
{
    sys_wifi_context_t *ctx = get_context();
    int ret;

    // Validate VIF parameter
    if (vif > WIFI_VIF_AP) {
        SYS_LOGE("Invalid VIF: %d", vif);
        return SYS_ERR_INVALID_ARG;
    }

    // Validate bandwidth values
    if (bw.ghz_2g >= WIFI_BW_MAX || bw.ghz_5g >= WIFI_BW_MAX) {
        SYS_LOGE("Invalid bandwidth: 2.4G=%d, 5G=%d", bw.ghz_2g, bw.ghz_5g);
        return SYS_ERR_INVALID_ARG;
    }

    // SoftAP bandwidth validation (software limitation)
    if (vif == WIFI_VIF_AP) {
        if (bw.ghz_2g != WIFI_BW_HT20 && bw.ghz_2g != WIFI_BW_AUTO) {
            SYS_LOGE("SoftAP 2.4G only supports WIFI_BW_AUTO or WIFI_BW_HT20");
            return SYS_ERR_NOT_SUPPORTED;
        }
        if (bw.ghz_5g != WIFI_BW_HT20 && bw.ghz_5g != WIFI_BW_AUTO) {
            SYS_LOGE("SoftAP 5G only supports WIFI_BW_AUTO or WIFI_BW_HT20");
            return SYS_ERR_NOT_SUPPORTED;
        }
    }

    // Sync to fhost layer before committing to the context. The platform layer
    // applies its own validation, and a rejected value must not stay readable
    // through sys_wifi_sta_get_bandwidth() or reach NV.
    ret = fhost_set_bandwidth(vif, bw);
    if (ret != 0) {
        SYS_LOGE("Failed to sync bandwidth to fhost layer (ret=%d)", ret);
        return SYS_ERR_WIFI_FAIL;
    }

    // Save to context
    ctx->capability_config.bandwidth[vif] = bw;

    // Save to NV (non-blocking, log warning on failure)
    ret = save_capability_config_to_nv();
    if (ret != SYS_OK) {
        SYS_LOGW("Failed to persist to NV, config will be lost on reboot");
    }

    SYS_LOGI("Bandwidth for VIF %d set to 2.4G=%d, 5G=%d",
              vif, bw.ghz_2g, bw.ghz_5g);
    return SYS_OK;
}

/**
 * @brief Get WiFi bandwidth per VIF (internal use only)
 * @param vif  Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param bw   Pointer to store bandwidth structure
 * @return SYS_OK on success, error code otherwise
 */
static sys_err_t sys_wifi_get_bandwidth(wifi_vif_e vif, wifi_bandwidth_t *bw)
{
    if (!bw) {
        return SYS_ERR_INVALID_ARG;
    }

    // Validate VIF parameter
    if (vif > WIFI_VIF_AP) {
        SYS_LOGE("Invalid VIF: %d", vif);
        return SYS_ERR_INVALID_ARG;
    }

    sys_wifi_context_t *ctx = get_context();
    *bw = ctx->capability_config.bandwidth[vif];

    return SYS_OK;
}

/**
 * @brief Set STA bandwidth configuration (public wrapper)
 *
 * Public API for configuring STA interface bandwidth. This function
 * internally calls sys_wifi_set_bandwidth() with WIFI_VIF_STA parameter.
 *
 * @param bw  Bandwidth structure (contains 2.4G and 5G configs)
 * @return SYS_OK on success, error code otherwise
 *
 * @note WIFI_BW_VHT80 (80MHz) is reserved for future support, not currently implemented
 * @see sys_wifi_set_bandwidth()
 */
sys_err_t sys_wifi_sta_set_bandwidth(wifi_bandwidth_t bw)
{
    return sys_wifi_set_bandwidth(WIFI_VIF_STA, bw);
}

/**
 * @brief Get STA bandwidth configuration (public wrapper)
 *
 * Public API for retrieving STA interface bandwidth configuration.
 * This function internally calls sys_wifi_get_bandwidth() with WIFI_VIF_STA parameter.
 *
 * @param[out] bw  Pointer to store bandwidth structure
 * @return SYS_OK on success, error code otherwise
 *
 * @see sys_wifi_get_bandwidth()
 */
sys_err_t sys_wifi_sta_get_bandwidth(wifi_bandwidth_t *bw)
{
    return sys_wifi_get_bandwidth(WIFI_VIF_STA, bw);
}

/**
 * @brief Set WiFi protocol per VIF (internal use only)
 * @param vif       Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param protocol  Protocol structure (contains 2.4G and 5G configs)
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_set_protocol(wifi_vif_e vif, wifi_protocol_t protocol)
{
    sys_wifi_context_t *ctx = get_context();
    int ret;

    // Validate VIF parameter
    if (vif > WIFI_VIF_AP) {
        SYS_LOGE("Invalid VIF: %d", vif);
        return SYS_ERR_INVALID_ARG;
    }

    // Simple validation: 5G should not have 11B
    if (protocol.ghz_5g & WIFI_PROTOCOL_11B) {
        SYS_LOGE("5G band does not support 11B (DSSS/CCK)");
        return SYS_ERR_INVALID_ARG;
    }

    // Sync to fhost layer before committing to the context. The platform layer
    // applies its own validation, and a rejected bitmap must not stay readable
    // through sys_wifi_get_protocol() or reach NV.
    ret = fhost_set_protocol(vif, protocol);
    if (ret != 0) {
        SYS_LOGE("Failed to sync protocol to fhost layer (ret=%d)", ret);
        return SYS_ERR_WIFI_FAIL;
    }

    // Save to context
    ctx->capability_config.protocol_bitmap[vif] = protocol;

    // Refresh WPA hardware feature cache to apply new protocol config
    // This ensures next AP startup will use the updated protocol bitmap
    ret = wpas_ctrl_refresh_hw_feature(vif);
    if (ret != 0) {
        SYS_LOGW("Failed to refresh hw_feature cache (ret=%d), hardware cache unavailable", ret);
        return SYS_ERR_WIFI_FAIL;
    }

    // Save to NV (non-blocking, log warning on failure)
    ret = save_capability_config_to_nv();
    if (ret != SYS_OK) {
        SYS_LOGW("Failed to persist to NV, config will be lost on reboot");
    }

    SYS_LOGI("Protocol for VIF %d set to 2.4G=0x%04x, 5G=0x%04x",
              vif, protocol.ghz_2g, protocol.ghz_5g);
    return SYS_OK;
}

/**
 * @brief Get WiFi protocol per VIF
 * @param vif       Virtual interface (WIFI_VIF_STA/WIFI_VIF_AP)
 * @param protocol  Pointer to store protocol structure
 * @return SYS_OK on success, error code otherwise
 */
sys_err_t sys_wifi_get_protocol(wifi_vif_e vif, wifi_protocol_t *protocol)
{
    if (!protocol) {
        return SYS_ERR_INVALID_ARG;
    }

    // Validate VIF parameter
    if (vif > WIFI_VIF_AP) {
        SYS_LOGE("Invalid VIF: %d", vif);
        return SYS_ERR_INVALID_ARG;
    }

    sys_wifi_context_t *ctx = get_context();
    *protocol = ctx->capability_config.protocol_bitmap[vif];

    return SYS_OK;
}

/**
 * @brief Set WiFi preamble type for specified interface (AT command internal use)
 *
 * AUTO preserves normal RC/negotiated output; LONG_ONLY and SHORT_ONLY override
 * the per-frame CCK preamble after rate selection without changing RC tables.
 * Settings are per role, work before connection, and survive reconnect.
 *
 * @param[in] vif      VIF type (WIFI_VIF_STA or WIFI_VIF_AP)
 * @param[in] preamble AUTO, LONG_ONLY or SHORT_ONLY
 * @return SYS_OK on success, error code otherwise
 *
 * @note Runtime policy applies to subsequent TX frames, including after reconnect.
 * @note Preamble setting only affects CCK rates (2-11 Mbps) in 802.11b/g modes
 */
sys_err_t sys_wifi_set_preamble(wifi_vif_e vif, wifi_preamble_t preamble)
{
    int ret;

    // Validate VIF parameter
    if (vif != WIFI_VIF_STA && vif != WIFI_VIF_AP) {
        SYS_LOGE("Invalid VIF: %d", vif);
        return SYS_ERR_INVALID_ARG;
    }

    // Validate preamble parameter
    if (preamble != WIFI_PREAMBLE_AUTO && preamble != WIFI_PREAMBLE_LONG_ONLY &&
        preamble != WIFI_PREAMBLE_SHORT_ONLY) {
        SYS_LOGE("Invalid preamble type: %d", preamble);
        return SYS_ERR_INVALID_ARG;
    }

    // Call fhost layer to send configuration
    ret = fhost_set_preamble(vif, preamble);
    if (ret != 0) {
        SYS_LOGE("fhost_set_preamble failed, vif=%d preamble=%d (ret=%d)", vif, preamble, ret);
        return SYS_ERR_WIFI_FAIL;
    }

    SYS_LOGI("Preamble set successfully: vif=%d type=%s",
              vif, (preamble == WIFI_PREAMBLE_LONG_ONLY) ? "LONG_ONLY" :
                   (preamble == WIFI_PREAMBLE_SHORT_ONLY) ? "SHORT_ONLY" : "AUTO");

    return SYS_OK;
}

wifi_discon_reason_e sys_wifi_get_disconnect_reason(void)
{
    return (wifi_discon_reason_e)wpas_ctrl_sta_get_disconnect_reason();
}

uint16_t sys_wifi_get_disconnect_reason_code(void)
{
    return wpas_ctrl_sta_get_disconnect_reason_code();
}

/*============================================================================
 * Inject/Monitor TX control wrappers
 * Encapsulate fhost_ioctl calls for components layer
 *============================================================================*/

sys_err_t sys_wifi_signal_test_set_enabled(bool enable)
{
    return fhost_signal_test_set_enabled(enable) == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_set_tx_rate(int standard, int mcs_index)
{
    return fhost_set_tx_rate(standard, mcs_index) == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_clear_tx_rate(void)
{
    return fhost_clear_tx_rate() == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_set_tx_ack_policy(int no_ack)
{
    return fhost_set_tx_ack_policy(no_ack) == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_set_monitor_rx_ack(bool enable)
{
    return fhost_set_monitor_rx_ack(enable) == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_set_tx_cfo(int enable, const uint8_t *bssid)
{
    return fhost_set_tx_cfo(enable, bssid) == 0 ? SYS_OK : SYS_ERR;
}

sys_err_t sys_wifi_sta_set_auth_assoc_retry_policy(uint8_t auth_max_attempts,
                                                   uint8_t assoc_max_attempts,
                                                   uint32_t rsp_timeout_us)
{
    if (!auth_max_attempts || !assoc_max_attempts || !rsp_timeout_us) {
        return SYS_ERR_INVALID_ARG;
    }

    return fhost_set_auth_assoc_retry_policy(auth_max_attempts,
                                             assoc_max_attempts,
                                             rsp_timeout_us) == 0 ?
        SYS_OK : SYS_ERR;
}

/*============================================================================
 * Debug-only WiFi tuning wrappers (NOT for production)
 * See system_wifi.h for warnings.
*============================================================================*/
sys_err_t sys_wifi_dbg_set_retry_limit(uint8_t mgmt_srl, uint8_t data_srl)
{
    return fhost_set_retry_limit(mgmt_srl, data_srl) == 0 ? SYS_OK : SYS_ERR;
}

/****************************************************************
*****************************************************************
****************************************************************/
