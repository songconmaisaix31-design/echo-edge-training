/**
 * @file wpas_cntrl_api.c
 * @brief WPA Supplicant Control API Implementation
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2025 Eswin Computing. All rights reserved.
 */

#include "utils/includes.h"
#include "utils/common.h"
#include "utils/eloop.h"
#include "config.h"
#include "wpa_supplicant_i.h"
#include "bss.h"
#include "common/wpa_common.h"
#include "rsn_supp/wpa.h"
#include "eswin_fhost_cntrl.h"
#include "wpas_cntrl_api.h"
#include "wpas_struct_iface.h"
#include "format_conversion.h"
#include "common/ieee802_11_defs.h"
#include "common/wpa_ctrl.h"
#include "driver_i.h"
#include "ap/sta_info.h"
#include "ap.h"
#include "wpa_debug.h"
#include "eswin_errno.h"
#ifdef CONFIG_WPA_KVR_SUPPORT
#include "wpa_kvr.h"
#endif /* CONFIG_WPA_KVR_SUPPORT */

extern int wifi_country_ctx_on_sta_connected(const wifi_country_info_t *ap_country);
extern int wifi_country_ctx_on_sta_disconnected(void);

/**
 * @brief Get STA list handler
 * @param user_ctx Pointer to structure containing info array and num
 */
struct get_sta_list_ctx {
    wpa_softap_sta_info_t *info;
    unsigned int *num;
    unsigned int current;
};
/**
 * @brief Get scan result by SSID handler context
 * @param user_ctx Pointer to structure containing SSID and wpa_wifi_info_t
 */
struct get_scan_by_ssid_ctx {
    const char *ssid;
    wpa_wifi_info_t *info;
};

struct get_scan_result_ctx {
    unsigned int index;
    wpa_wifi_info_t *info;
};

struct get_ap_country_info_ctx {
    wifi_country_info_t *info;
};

/**
 * @brief Scan count context
 */
struct scan_count_ctx {
    int count;
};

/**
 * @brief Set maximum STA count context
 */
struct set_max_sta_ctx {
    unsigned short max_sta;
};

/**
 * @brief Country code operation context
 */
struct country_code_ctx {
    char country_code[3];
};

/**
 * @brief Log level setting context
 */
struct log_level_ctx {
    int level;
};

/**
 * @brief Beacon interval context
 */
struct beacon_interval_ctx {
    unsigned int interval;
};

/**
 * @brief SoftAP beacon vendor specific IE context
 */
struct vendor_elements_ctx {
    const unsigned char *data;
    unsigned int len;
};

/**
 * @brief STA connect policy context (for set operation)
 */
struct sta_connect_policy_ctx {
    int interval_sec;
    int repeat_count;
};

/**
 * @brief STA connect policy get context (for get operation)
 */
struct sta_connect_policy_get_ctx {
    int *interval_sec;
    int *repeat_count;
};

/**
 * @brief Country information context (for wifi_country_info_t)
 */
struct country_info_ctx {
    wifi_country_info_t info;
};

/**
 * @brief PCI mode context
 */
struct pci_mode_ctx {
    int pci_enable;
};

/**
 * @brief Authentication mode threshold context
 */
struct authmode_threshold_ctx {
    wpa_auth_mode_e authmode_threshold;
};

/**
 * @brief Scan expire age context
 */
struct scan_expire_ctx {
    unsigned int age;
};

/**
 * @brief Scan flush context
 */
struct scan_flush_ctx {
    int flush_age;
};

#ifdef CONFIG_WPA_KVR_SUPPORT
struct rrm_neighbor_req_ctx {
    const unsigned char *ssid;
    unsigned int ssid_len;
};

struct wnm_bss_query_ctx {
    unsigned char reason;
    int cand_list;
};

struct sta_roam_ctx {
    const unsigned char *bssid;
};
#endif /* CONFIG_WPA_KVR_SUPPORT */

struct ctrl_iface_priv {
    struct wpa_supplicant *wpa_s;
    os_sem_handle_t finish;
    os_mutex_handle_t mutex;
    sys_err_t status; /* Error code type using eswin_errno */
};

struct ctrl_iface_global_priv {
    struct ctrl_iface_priv *ctrl_if[ESWIN_WIFI_IF_NUM];
};

static struct ctrl_iface_global_priv* global_ctrl_if = NULL;
static wpas_ctrl_selected_bss_cb_t selected_bss_cb = NULL;
static void *selected_bss_cb_ctx = NULL;

/**
 * @brief Get wpa_supplicant instance (by vif_id)
 * @param vif_id Virtual interface ID
 * @return wpa_supplicant pointer, returns NULL on failure
 */
struct wpa_supplicant *wpas_internal_get_iface(int vif_id)
{
    if (!global_ctrl_if || vif_id >= ESWIN_WIFI_IF_NUM ||
        !global_ctrl_if->ctrl_if[vif_id]) {
        return NULL;
    }

    return global_ctrl_if->ctrl_if[vif_id]->wpa_s;
}

sys_err_t wpas_ctrl_sta_register_selected_bss_cb(wpas_ctrl_selected_bss_cb_t cb,
                                                 void *ctx)
{
    if (!cb) {
        return SYS_ERR_INVALID_ARG;
    }

    selected_bss_cb = cb;
    selected_bss_cb_ctx = ctx;
    return SYS_OK;
}

sys_err_t wpas_ctrl_sta_unregister_selected_bss_cb(void)
{
    selected_bss_cb = NULL;
    selected_bss_cb_ctx = NULL;
    return SYS_OK;
}

void wpas_ctrl_sta_notify_selected_bss(const unsigned char *bssid,
                                       const unsigned char *ssid,
                                       unsigned char ssid_len,
                                       int freq,
                                       int rssi)
{
    wpas_ctrl_selected_bss_cb_t cb = selected_bss_cb;
    wpas_ctrl_selected_bss_info_t info;

    if (!cb || !bssid || !ssid) {
        return;
    }

    os_memset(&info, 0, sizeof(info));
    os_memcpy(info.bssid, bssid, sizeof(info.bssid));
    os_memcpy(info.ssid, ssid, ssid_len);
    info.ssid_len = ssid_len;
    info.freq = freq;
    info.rssi = rssi;

    cb(&info, selected_bss_cb_ctx);
}

/**
 * @brief Get ctrl_iface_priv instance
 * @param wpa_s wpa_supplicant instance
 * @return ctrl_iface_priv pointer, returns NULL on failure
 */
struct ctrl_iface_priv *wpas_internal_get_priv(struct wpa_supplicant *wpa_s)
{
    int vif_id = 0;

    if (!global_ctrl_if) {
        return NULL;
    }

    for (vif_id = 0; vif_id < ESWIN_WIFI_IF_NUM; vif_id++) {
        if (global_ctrl_if->ctrl_if[vif_id] && global_ctrl_if->ctrl_if[vif_id]->wpa_s == wpa_s) {
            return global_ctrl_if->ctrl_if[vif_id];
        }
    }

    return NULL;
}

struct ctrl_iface_priv* wpa_supplicant_ctrl_iface_init(struct wpa_supplicant *wpa_s)
{
    struct ctrl_iface_priv* priv = NULL;
    int vif_id = 0;

    if (!global_ctrl_if) {
        wpa_printf(MSG_ERROR, "Failed to init ctrl_iface: global_ctrl_if is NULL.");
        return NULL;
    }

    if (os_strcmp(ESWIN_WIFI_IF_NAME_0, wpa_s->ifname) == 0) {
        vif_id = 0;
    } else if (os_strcmp(ESWIN_WIFI_IF_NAME_1, wpa_s->ifname) == 0) {
        vif_id = 1;
    } else {
        wpa_printf(MSG_ERROR, "%s: Unknown interface name (%s)", __func__, wpa_s->ifname);
        return NULL;
    }

    priv = (struct ctrl_iface_priv *) os_zalloc(sizeof(*priv));
    if (!priv) {
        wpa_printf(MSG_ERROR, "Failed to allocate ctrl_iface");
        return NULL;
    }

    priv->wpa_s = wpa_s;
    priv->status = SYS_ERR; /* not ready */
    priv->mutex = os_mutex_create();
    priv->finish = os_sem_create(1, 0);

    global_ctrl_if->ctrl_if[vif_id] = priv;

    return priv;
}

struct ctrl_iface_global_priv* wpa_supplicant_global_ctrl_iface_init(struct wpa_global *global)
{
    if (global_ctrl_if) {
        os_free(global_ctrl_if);
        global_ctrl_if = NULL;
    }

    global_ctrl_if = (struct ctrl_iface_global_priv *) os_zalloc(sizeof(*global_ctrl_if));
    if(!global_ctrl_if) {
        wpa_printf(MSG_ERROR, "Failed to allocate ctrl_iface global");
        return NULL;
    }

    return global_ctrl_if;
}

/* --------------------------------------------------------------------------
 * Synchronous eloop work execution
 *
 * Execute handler synchronously in eloop thread and block caller until
 * handler completes. This is a BLOCKING call.
 *
 * @param priv         Control interface private data
 * @param handler      Handler function to execute in eloop thread
 * @param eloop_data   First parameter for handler
 * @param user_ctx     Second parameter for handler (stack data is SAFE)
 * @return             Result from handler (priv->status)
 *
 * Memory Management:
 * - Caller's stack variables remain valid throughout execution
 * - user_ctx can safely point to stack-allocated data
 * - No dynamic allocation needed
 *
 * Use Cases:
 * - When immediate result is required (e.g., get operations)
 * - When caller needs to wait for completion
 * - Most common pattern in this file
 *
 * Example:
 *   struct my_ctx ctx = { .field = value };
 *   ret = execute_eloop_work_sync(priv, handler, wpa_s, &ctx);
 *   // ctx is still valid here, handler has completed
 * -------------------------------------------------------------------------- */
static sys_err_t execute_eloop_work_sync(struct ctrl_iface_priv *priv,
                                         void (*handler)(void *eloop_data, void *user_ctx),
                                         void *eloop_data, void *user_ctx)
{
    int ret;

    if (!priv)
        return SYS_ERR_INVALID_ARG;

    if (!priv->mutex || !priv->finish)
        return SYS_ERR_INVALID_STATE;

    os_mutex_lock(priv->mutex, WAIT_FOREVER);

    ret = eloop_register_timeout(0, 0, handler, eloop_data, user_ctx);
    if (ret < 0) {
        os_mutex_unlock(priv->mutex);
        return SYS_ERR;
    }

    /* BLOCKING: Wait for handler completion */
    os_sem_wait(priv->finish, WAIT_FOREVER);
    os_mutex_unlock(priv->mutex);

    return priv->status;
}

/* --------------------------------------------------------------------------
 * Asynchronous eloop work execution
 *
 * Schedule handler to execute in eloop thread and return immediately
 * without waiting. This is a NON-BLOCKING call.
 *
 * @param priv         Control interface private data
 * @param handler      Handler function to execute in eloop thread
 * @param eloop_data   First parameter for handler
 * @param user_ctx     Second parameter for handler (MUST be heap-allocated)
 * @return             SYS_OK if scheduled successfully, error otherwise
 *
 * CRITICAL Memory Management Rules:
 * - user_ctx MUST be heap-allocated via os_malloc()
 * - Handler MUST call os_free(user_ctx) when done
 * - Handler MUST NOT call os_sem_post(priv->finish)
 * - Caller's stack data will be INVALID when handler runs
 *
 * Use Cases:
 * - Fire-and-forget operations (e.g., event notifications)
 * - When caller doesn't need to wait for result
 * - Asynchronous event processing
 *
 * Example:
 *   struct my_ctx *ctx = os_malloc(sizeof(*ctx));
 *   if (ctx) {
 *       ctx->field = value;
 *       ret = execute_eloop_work_async(priv, async_handler, wpa_s, ctx);
 *       // ctx may NOT be valid here, handler owns it
 *   }
 *
 *   // In async_handler:
 *   void async_handler(void *eloop_data, void *user_ctx) {
 *       struct my_ctx *ctx = user_ctx;
 *       // ... process ...
 *       os_free(ctx);  // MUST free
 *       // DO NOT call os_sem_post(priv->finish)
 *   }
 *
 * WARNING: Do NOT use this function unless you fully understand the
 *          memory management implications. Most cases should use
 *          execute_eloop_work_sync() instead.
 * -------------------------------------------------------------------------- */
__attribute__((unused))
static sys_err_t execute_eloop_work_async(struct ctrl_iface_priv *priv,
                                          void (*handler)(void *eloop_data, void *user_ctx),
                                          void *eloop_data, void *user_ctx)
{
    int ret;

    if (!priv)
        return SYS_ERR_INVALID_ARG;

    if (!priv->mutex)
        return SYS_ERR_INVALID_STATE;

    os_mutex_lock(priv->mutex, WAIT_FOREVER);

    ret = eloop_register_timeout(0, 0, handler, eloop_data, user_ctx);

    os_mutex_unlock(priv->mutex);

    /* NON-BLOCKING: Return immediately without waiting */
    return (ret < 0) ? SYS_ERR : SYS_OK;
}

/* ===========================================================================
 * Unified Handler Processing Pattern:
 * 1. Get priv = wpas_internal_get_priv(wpa_s)
 * 2. Call lower-layer function (functions in wpas_struct_iface.c)
 * 3. Set priv->status = return value from lower-layer function
 * 4. os_sem_post(priv->finish)
 *
 * Note: Type conversion is handled by upper layer system_wifi.c, this layer uses WPA types directly
 * =========================================================================== */

/**
 * @brief Network configuration handler
 * @param user_ctx Pointer to wpa_network_config_t (already converted by upper layer)
 */
static void handler_configure_network(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        return;
    }

    /* Use WPA type directly, no conversion needed */
    priv->status = wpas_core_network_config(wpa_s,
                                                 (const wpa_network_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

/**
 * @brief Scan handler
 * @param user_ctx Pointer to wpa_scan_config_t (already converted by upper layer)
 */
static void handler_scan_start(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        return;
    }

    /* Use WPA type directly, no conversion needed */
    priv->status = wpas_core_scan_start(wpa_s, (const wpa_scan_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_scan_start_checked(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    if (!priv) {
        return;
    }
    priv->status = wpas_core_scan_start_checked(wpa_s, (const wpa_scan_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_scan_stop(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    (void)user_ctx;

    if (!priv) {
        return;
    }

    priv->status = wpas_core_scan_stop(wpa_s);
    os_sem_post(priv->finish);
}

/* wrapper for remove network: existing function doesn't post priv->finish,
   so do it here after call */
static void handler_remove_network(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!wpa_s || !priv) {
        return;
    }

    wpas_core_network_remove(wpa_s);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

static void handler_disconnect_network(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    (void)user_ctx;

    if (!wpa_s || !priv) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_network_disconnect(wpa_s);
    os_sem_post(priv->finish);
}

static void handler_reconnect_network(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!wpa_s || !priv) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_network_reconnect(wpa_s,
                        (const wpa_network_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_network_scan_result(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct wpas_network_scan_result *out = user_ctx;
    struct wpa_ssid *ssid = wpa_s->conf->ssid;

    priv->status = SYS_ERR_INVALID_STATE;
    if (ssid && ssid->ssid_len <= sizeof(out->ssid)) {
        os_memset(out, 0, sizeof(*out));
        out->selected_freq_2g = ssid->selected_freq_2g;
        out->selected_freq_5g = ssid->selected_freq_5g;
        out->connected = wpa_s->current_ssid == ssid && wpa_s->wpa_state == WPA_COMPLETED;
        out->frequency = ssid->frequency;
        out->candidate_frequency = ssid->candidate_frequency;
        out->ssid_len = ssid->ssid_len;
        os_memcpy(out->ssid, ssid->ssid, ssid->ssid_len);
        os_memcpy(out->bssid, wpa_s->bssid, ETH_ALEN);
        out->bssid_set = ssid->bssid_set;
        priv->status = SYS_OK;
    }
    os_sem_post(priv->finish);
}

static void handler_network_scan_hint(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    const wpas_network_scan_hint_t *hint = user_ctx;
    struct wpa_ssid *ssid = wpa_s->conf->ssid;

    priv->status = SYS_ERR_INVALID_STATE;
    if (ssid) {
        ssid->frequency = hint->frequency;
        ssid->candidate_frequency = hint->candidate_frequency != hint->frequency ?
                                    hint->candidate_frequency : 0;
        priv->status = SYS_OK;
    }
    os_sem_post(priv->finish);
}

static void handler_network_has_pmksa(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!wpa_s || !priv) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_network_has_pmksa(wpa_s,
                        (const wpa_network_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_pmk_cache_clean(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    (void)user_ctx;

    if (!wpa_s || !priv) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_pmk_cache_clean(wpa_s);
    os_sem_post(priv->finish);
}

#ifdef CONFIG_WPA_KVR_SUPPORT
static void wpas_ctrl_rrm_neighbor_rep_cb(void *ctx, struct wpabuf *neighbor_rep)
{
    struct wpa_supplicant *wpa_s = ctx;

    (void)wpa_s;

    if (!neighbor_rep || wpabuf_len(neighbor_rep) == 0) {
        wpa_msg(wpa_s, MSG_INFO, RRM_EVENT_NEIGHBOR_REP_FAILED);
        goto out;
    }

    wpa_msg(wpa_s, MSG_INFO, RRM_EVENT_NEIGHBOR_REP_RXED "len=%u",
            (unsigned int)wpabuf_len(neighbor_rep));
    wpa_hexdump(MSG_DEBUG, "RRM: Neighbor Report",
                wpabuf_head(neighbor_rep), wpabuf_len(neighbor_rep));

out:
    wpabuf_free(neighbor_rep);
}

static void handler_rrm_neighbor_req(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct rrm_neighbor_req_ctx *ctx = (struct rrm_neighbor_req_ctx *)user_ctx;
    struct wpa_ssid_value ssid;
    struct wpa_ssid_value *ssid_p = NULL;
    int ret;

    if (!wpa_s || !priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (wpa_s->wpa_state != WPA_COMPLETED) {
        priv->status = SYS_ERR_WIFI_NOT_CONNECTED;
        os_sem_post(priv->finish);
        return;
    }

    if (ctx->ssid) {
        if (ctx->ssid_len == 0 || ctx->ssid_len > SSID_MAX_LEN) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
            return;
        }

        os_memset(&ssid, 0, sizeof(ssid));
        os_memcpy(ssid.ssid, ctx->ssid, ctx->ssid_len);
        ssid.ssid_len = ctx->ssid_len;
        ssid_p = &ssid;
    }

    ret = wpa_kvr_rrm_send_neighbor_rep_request(wpa_s, ssid_p, 0, 0,
                                             wpas_ctrl_rrm_neighbor_rep_cb,
                                             wpa_s);
    priv->status = (ret == 0) ? SYS_OK : SYS_ERR;
    os_sem_post(priv->finish);
}

static void handler_wnm_bss_query(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct wnm_bss_query_ctx *ctx = (struct wnm_bss_query_ctx *)user_ctx;
    int ret;

    if (!wpa_s || !priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (wpa_s->wpa_state != WPA_COMPLETED) {
        priv->status = SYS_ERR_WIFI_NOT_CONNECTED;
        os_sem_post(priv->finish);
        return;
    }

    ret = wpa_kvr_wnm_bss_query(wpa_s, ctx->reason, ctx->cand_list);
    priv->status = (ret == 0) ? SYS_OK : SYS_ERR;
    os_sem_post(priv->finish);
}

static void handler_sta_roam(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct sta_roam_ctx *ctx = (struct sta_roam_ctx *)user_ctx;
    struct wpa_ssid *ssid;
    struct wpa_bss *bss;
    int ret;

    if (!wpa_s || !priv || !ctx || !ctx->bssid) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (wpa_s->wpa_state != WPA_COMPLETED) {
        priv->status = SYS_ERR_WIFI_NOT_CONNECTED;
        os_sem_post(priv->finish);
        return;
    }

    ssid = wpa_s->current_ssid;
    if (!ssid) {
        priv->status = SYS_ERR_INVALID_STATE;
        os_sem_post(priv->finish);
        return;
    }

    bss = wpa_bss_get(wpa_s, ctx->bssid, ssid->ssid, ssid->ssid_len);
	if (!bss) {
        priv->status = SYS_ERR_NOT_FOUND;
        os_sem_post(priv->finish);
		return;
	}

	wpa_kvr_roam_abort_transaction(wpa_s);
	ret = wpa_kvr_roam_connect_candidate(wpa_s, bss, ssid);
    priv->status = (ret == 0) ? SYS_OK : SYS_ERR;
    os_sem_post(priv->finish);
}
#endif /* CONFIG_WPA_KVR_SUPPORT */

/**
 * @brief SoftAP CSA handler
 */
static void handler_softap_config_csa(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        return;
    }

    priv->status = wpas_core_softap_config_csa(wpa_s,
                                                 (const wpa_csa_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_station_get_info(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        return;
    }

    priv->status = wpas_core_station_get_info(wpa_s,
                                                 (wpa_wifi_info_t *)user_ctx);
    os_sem_post(priv->finish);
}

static void handler_scan_get_result(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct get_scan_result_ctx *ctx = (struct get_scan_result_ctx *)user_ctx;
    struct wpa_bss *bss;
    struct dl_list *scan_res_min __attribute__((unused));
    struct wpa_rwnx_driver_scan_res_min *cur __attribute__((unused));
    unsigned int __attribute__((unused)) count = 0;
    unsigned int i = 0;

    if (!priv || !ctx || !ctx->info) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    memset(ctx->info, 0, sizeof(*ctx->info));

#if defined(CONFIG_ECR6600_WIFI) || defined(CONFIG_ECW6280_WIFI)
    if (1 /*wpa_rwnx_driver_get_glance_scan(wpa_s->drv_priv)*/) {
        scan_res_min = wpa_drv_get_glance_scan_results2(wpa_s);
        count = dl_list_len(scan_res_min);

        if (!count) {
            priv->status = SYS_ERR_NOT_FOUND;
            os_sem_post(priv->finish);
            return;
        }

        dl_list_for_each(cur, scan_res_min, struct wpa_rwnx_driver_scan_res_min, list) {
            if (ctx->index <= count && i++ == ctx->index) {
                memcpy(ctx->info->bssid, cur->res.bssid, 6);
                memcpy(ctx->info->ssid, cur->res.ssid, cur->res.ssid_len);
                ctx->info->ssid_len = cur->res.ssid_len;
                ctx->info->channel = cur->res.channel;
                ctx->info->rssi = (int8_t)(cur->res.rssi);
                ctx->info->auth = cur->res.auth_method;
                ctx->info->cipher = cur->res.cipher;
                ctx->info->group_cipher = cur->res.group_cipher;
                ctx->info->wifi_protocol = cur->res.wifi_protocol;
                ctx->info->wps = cur->res.wps;
                priv->status = SYS_OK;
                os_sem_post(priv->finish);
                return;
            }
        }
    }
    else
#endif
    {
        dl_list_for_each(bss, &wpa_s->bss_id, struct wpa_bss, list_id) {
            if (i++ == ctx->index) {
                memcpy(ctx->info->bssid, bss->bssid, 6);
                memcpy(ctx->info->ssid, bss->ssid, bss->ssid_len);
                ctx->info->ssid_len = bss->ssid_len;
                ieee80211_freq_to_chan(bss->freq, &ctx->info->channel);
                ctx->info->rssi = (int8_t)(bss->level);
                wpas_core_bss_get_mode(bss, &ctx->info->auth, &ctx->info->cipher);
                wpas_core_bss_get_extra_info(bss, &ctx->info->group_cipher,
                    &ctx->info->wifi_protocol, &ctx->info->wps);
                priv->status = SYS_OK;
                os_sem_post(priv->finish);
                return;
            }
        }
    }

    priv->status = SYS_ERR_NOT_FOUND;
    os_sem_post(priv->finish);
}

static void handler_get_scan_result_by_ssid(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct get_scan_by_ssid_ctx *ctx = (struct get_scan_by_ssid_ctx *)user_ctx;
    struct wpa_bss *bss;

    if (!priv || !ctx || !ctx->ssid || !ctx->info) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    memset(ctx->info, 0, sizeof(*ctx->info));

    dl_list_for_each(bss, &wpa_s->bss_id, struct wpa_bss, list_id) {
        if (strcmp((char*)bss->ssid, ctx->ssid) == 0) {
            memcpy(ctx->info->bssid, bss->bssid, 6);
            memcpy(ctx->info->ssid, bss->ssid, bss->ssid_len);
            ctx->info->ssid_len = bss->ssid_len;
            ieee80211_freq_to_chan(bss->freq, &ctx->info->channel);
            ctx->info->rssi = (int8_t)(bss->level);
            wpas_core_bss_get_mode(bss, &ctx->info->auth, &ctx->info->cipher);
            wpas_core_bss_get_extra_info(bss, &ctx->info->group_cipher,
                &ctx->info->wifi_protocol, &ctx->info->wps);
            priv->status = SYS_OK;
            os_sem_post(priv->finish);
            return;
        }
    }

    priv->status = SYS_ERR_NOT_FOUND;
    os_sem_post(priv->finish);
}

/**
 * @brief Get AP information handler (SSID, password, channel)
 * @param user_ctx Pointer to wpa_wifi_info_t
 */
static void handler_softap_get_config(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        return;
    }

    /* Use WPA type directly, no conversion needed */
    priv->status = wpas_core_softap_get_config(wpa_s, (wpa_network_config_t *)user_ctx);
    os_sem_post(priv->finish);
}

static int softap_get_sta_callback(struct sta_info *sta, void *ctx)
{
    struct get_sta_list_ctx *sta_ctx = (struct get_sta_list_ctx *)ctx;

    if (!sta) {
        return 1;
    }

    if (!(sta->flags & WLAN_STA_ASSOC)) {
        return 0;
    }

    if (sta_ctx->current >= *sta_ctx->num) {
        return 1;
    }

    memcpy(sta_ctx->info[sta_ctx->current].mac_addr, sta->addr, 6);
    sta_ctx->info[sta_ctx->current].aid = sta->aid;
    sta_ctx->current++;

    return 0;
}

static void handler_softap_get_sta_list(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct get_sta_list_ctx *ctx = (struct get_sta_list_ctx *)user_ctx;

    if (!priv || !ctx || !ctx->info || !ctx->num) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    ctx->current = 0;
    ap_for_each_sta_wrap(wpa_s, softap_get_sta_callback, ctx);

    *ctx->num = ctx->current;
    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/* Handler for deauth */
static void handler_softap_deauth_sta(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    const char *txtaddr = (const char *)user_ctx;

    if (!wpa_s || !priv || !txtaddr) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (ap_ctrl_iface_sta_deauthenticate(wpa_s, txtaddr) < 0) {
        priv->status = SYS_ERR;
    } else {
        priv->status = SYS_OK;
    }

    os_sem_post(priv->finish);
}

static void handler_iface_dump_status(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    char print_buf[256];
    unsigned int rlen;

    if (!priv) {
        priv->status = SYS_ERR_INVALID_ARG;
        return;
    }

    rlen = wpas_core_iface_dump_status(wpa_s, NULL,print_buf, sizeof(print_buf));
    print_buf[rlen] = '\0';
    os_printf(LL_INFO, "%s", print_buf);

    priv->status =SYS_OK;
    os_sem_post(priv->finish);
}

static void handler_iface_update_mac(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);

    if (!priv) {
        priv->status = SYS_ERR_INVALID_ARG;
        return;
    }

    wpas_core_iface_update_mac(wpa_s, (const char *)user_ctx);

    priv->status =SYS_OK;
    os_sem_post(priv->finish);
}

static void handler_iface_dump_list_networks(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    char print_buf[512];
    unsigned int rlen;

    if (!priv) {
        return;
    }

    rlen = wpas_core_iface_dump_list_networks(wpa_s, print_buf, sizeof(print_buf));
    print_buf[rlen] = '\0';
    os_printf(LL_INFO, "%s", print_buf);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Get scan count handler (P0)
 */
static void handler_scan_get_count(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct scan_count_ctx *ctx = (struct scan_count_ctx *)user_ctx;
    struct dl_list *list __attribute__((unused));

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    ctx->count = 0;

#if defined(CONFIG_ECR6600_WIFI) || defined(CONFIG_ECW6280_WIFI)
    if (1 /*wpa_rwnx_driver_get_glance_scan(wpa_s->drv_priv)*/) {
        list = wpa_drv_get_glance_scan_results2(wpa_s);
        if (list) {
            ctx->count = dl_list_len(list);
        }
    }
    else
#endif
    {
        ctx->count = dl_list_len(&wpa_s->bss_id);
    }

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set maximum STA count handler (P1)
 */
static void handler_softap_set_max_sta(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct set_max_sta_ctx *ctx = (struct set_max_sta_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    /* Validate max_sta range: 1 to hardware capability limit
     * Only modify user configuration value (max_num_sta)
     * max_stations is hardware capability and should remain unchanged */
    if (ctx->max_sta < 1 || ctx->max_sta > fhost_get_sta_max_cnt()) {
        wpa_printf(MSG_ERROR, "max sta para error, set default value:%d\n",
                   fhost_get_sta_max_cnt());
        wpa_s->conf->max_num_sta = fhost_get_sta_max_cnt();
        priv->status = SYS_ERR_INVALID_ARG;
    } else {
        wpa_s->conf->max_num_sta = ctx->max_sta;
        priv->status = SYS_OK;
    }

    os_sem_post(priv->finish);
}

/* Called only in the WPA eloop, after a global country/channel change. */
static int refresh_hw_features(void)
{
    int ret = SYS_OK;

    for (int vif = 0; vif < ESWIN_WIFI_IF_NUM; vif++) {
        struct wpa_supplicant *wpa_s = wpas_internal_get_iface(vif);
        u16 num_modes = 0, flags = 0;

        if (!wpa_s) {
            continue;
        }
        wpa_s->hw.modes = wpa_drv_get_hw_feature_data(wpa_s, &num_modes, &flags);
        wpa_s->hw.num_modes = wpa_s->hw.modes ? num_modes : 0;
        wpa_s->hw.flags = wpa_s->hw.modes ? flags : 0;
        if (!wpa_s->hw.modes) {
            ret = SYS_ERR;
        }
    }
    if (ret != SYS_OK) {
        /* Channel storage is shared: reject partially refreshed snapshots. */
        for (int vif = 0; vif < ESWIN_WIFI_IF_NUM; vif++) {
            struct wpa_supplicant *wpa_s = wpas_internal_get_iface(vif);

            if (wpa_s) {
                wpa_s->hw.modes = NULL;
                wpa_s->hw.num_modes = 0;
                wpa_s->hw.flags = 0;
            }
        }
        os_printf(LL_ERR, "WPA: Failed to refresh country channel caches\n");
    }
    return ret;
}

/**
 * @brief Set country code handler (P1)
 */
static void handler_system_set_country(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct country_code_ctx *ctx = (struct country_code_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpa_drv_set_country(wpa_s, ctx->country_code);
    if (priv->status == SYS_OK) {
        priv->status = refresh_hw_features();
    }
    os_sem_post(priv->finish);
}

/**
 * @brief Set STA connect policy handler
 */
static void handler_sta_set_connect_policy(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct sta_connect_policy_ctx *ctx = (struct sta_connect_policy_ctx *)user_ctx;
    bool auto_reconnect_enabled;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    /* Validate parameters */
    if (ctx->interval_sec < 0 || ctx->repeat_count < 0) {
        wpa_printf(MSG_ERROR,
                   "Failed to set connect policy: invalid parameters "
                   "(interval=%d, count=%d)",
                   ctx->interval_sec, ctx->repeat_count);
        priv->status = SYS_ERR_INVALID_ARG;
        os_sem_post(priv->finish);
        return;
    }

    /* Apply configuration */
    wpa_s->conf->sta_reconnect_interval_sec = ctx->interval_sec;
    wpa_s->conf->sta_reconnect_count = ctx->repeat_count;

    /*
     * AT compatibility:
     * interval_sec > 0 enables auto-reconnect (repeat_count controls limit).
     * interval_sec == 0 disables auto-reconnect regardless of repeat_count.
     */
    auto_reconnect_enabled = (ctx->interval_sec > 0);
    wpa_s->disconnected = auto_reconnect_enabled ? 0 : 1;

    /* Reset current reconnection state */
    wpa_s->reconnect_attempt_count = 0;
    wpa_s->reconnect_limit_reached = auto_reconnect_enabled ? 0 : 1;

    wpa_msg(wpa_s, MSG_INFO,
            "WPA: Set connect policy - interval=%ds, count=%d, auto_reconnect=%d",
            ctx->interval_sec, ctx->repeat_count, auto_reconnect_enabled);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Get STA connect policy handler
 */
static void handler_sta_get_connect_policy(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct sta_connect_policy_get_ctx *ctx = (struct sta_connect_policy_get_ctx *)user_ctx;

    if (!priv || !ctx || !ctx->interval_sec || !ctx->repeat_count) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    /* Retrieve configuration */
    *(ctx->interval_sec) = wpa_s->conf->sta_reconnect_interval_sec;
    *(ctx->repeat_count) = wpa_s->conf->sta_reconnect_count;

    wpa_msg(wpa_s, MSG_DEBUG,
            "WPA: Get connect policy - interval=%ds, count=%d",
            *(ctx->interval_sec), *(ctx->repeat_count));

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Refresh hardware feature cache handler
 *
 * This handler updates the cached hardware feature data (wpa_s->hw.modes)
 * after runtime configuration changes (protocol_bitmap).
 *
 * Architecture:
 * - drv->hw_modes is per-VIF independent storage
 * - wpa_s->hw.modes points to drv->hw_modes
 * - Re-calling wpa_drv_get_hw_feature_data updates drv->hw_modes
 * - Since wpa_s->hw.modes points to same memory, data auto-syncs
 * - Save the returned pointer too, so an invalidated cache can recover
 */
static void handler_refresh_hw_feature(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct hostapd_hw_modes *modes;
    u16 num_modes = 0, flags = 0;

    if (!priv) {
        return;
    }

    /* Re-read hardware feature data from driver
     * This calls wpa_rwnx_driver_get_hw_feature_data which:
     * 1. Reads current protocol_bitmap from fhost layer (VIF-aware)
     * 2. Re-fills drv->hw_modes based on new protocol config
     * 3. Returns pointer to drv->hw_modes
     */
    modes = wpa_drv_get_hw_feature_data(wpa_s, &num_modes, &flags);
    wpa_s->hw.modes = modes;
    wpa_s->hw.num_modes = modes ? num_modes : 0;
    wpa_s->hw.flags = modes ? flags : 0;
    if (!modes) {
        wpa_msg(wpa_s, MSG_WARNING,
                "WPA: Failed to refresh hw_feature_data");
        priv->status = SYS_ERR;
        os_sem_post(priv->finish);
        return;
    }

    wpa_msg(wpa_s, MSG_INFO,
            "WPA: Hardware feature cache refreshed (num_modes=%u)",
            num_modes);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Get country code handler (P2)
 */
static void handler_system_get_country(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct country_code_ctx *ctx = (struct country_code_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    wpa_drv_get_country(wpa_s, ctx->country_code);
    ctx->country_code[2] = '\0';

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set country information handler (for 2.4G + 5G support)
 */
static void handler_system_set_country_info(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct country_info_ctx *ctx = (struct country_info_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpa_drv_set_country_info(wpa_s, &ctx->info);
    if (priv->status == SYS_OK) {
        priv->status = refresh_hw_features();
    }
    os_sem_post(priv->finish);
}

/* NULL country means STA disconnected: restore the configured country. */
static void handler_sta_country_changed(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    const wifi_country_info_t *ap_country = user_ctx;
    wifi_country_info_t before = {0}, after = {0};

    if (!priv) {
        return;
    }
    priv->status = wpa_drv_get_country_info(wpa_s, &before);
    if (priv->status == SYS_OK) {
        priv->status = ap_country ? wifi_country_ctx_on_sta_connected(ap_country) :
                                   wifi_country_ctx_on_sta_disconnected();
    }
    if (priv->status == SYS_OK) {
        priv->status = wpa_drv_get_country_info(wpa_s, &after);
        int refresh = priv->status != SYS_OK || memcmp(&before, &after, sizeof(before));

        for (int vif = 0; vif < ESWIN_WIFI_IF_NUM && !refresh; vif++) {
            struct wpa_supplicant *iface = wpas_internal_get_iface(vif);

            if (iface && (!iface->hw.modes || !iface->hw.num_modes)) {
                refresh = 1;
            }
        }
        if (refresh) {
            int ret = refresh_hw_features();

            if (priv->status == SYS_OK) {
                priv->status = ret;
            }
        }
    }
    os_sem_post(priv->finish);
}

/**
 * @brief Get country information handler (for 2.4G + 5G support)
 */
static void handler_system_get_country_info(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct country_info_ctx *ctx = (struct country_info_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpa_drv_get_country_info(wpa_s, &ctx->info);
    os_sem_post(priv->finish);
}

static void handler_station_get_ap_country_info(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct get_ap_country_info_ctx *ctx = (struct get_ap_country_info_ctx *)user_ctx;
    const u8 *country_ie;

    if (!priv || !ctx || !ctx->info) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    memset(ctx->info, 0, sizeof(*ctx->info));

    if (!wpa_s || !wpa_s->current_bss) {
        priv->status = SYS_ERR_INVALID_STATE;
        os_sem_post(priv->finish);
        return;
    }

    country_ie = wpa_bss_get_ie(wpa_s->current_bss, WLAN_EID_COUNTRY);
    if (!country_ie || country_ie[1] < 3) {
        priv->status = SYS_ERR_NOT_FOUND;
        os_sem_post(priv->finish);
        return;
    }

    ctx->info->country_code[0] = country_ie[2];
    ctx->info->country_code[1] = country_ie[3];
    ctx->info->country_code[2] = '\0';

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set log level handler (P2)
 */
static void handler_system_set_log_level(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct log_level_ctx *ctx = (struct log_level_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (ctx->level >= MSG_EXCESSIVE && ctx->level <= MSG_ERROR) {
        wpa_debug_level = ctx->level;
        priv->status = SYS_OK;
    } else {
        priv->status = SYS_ERR_INVALID_ARG;
    }

    os_sem_post(priv->finish);
}

/**
 * @brief Get beacon interval handler (P3)
 */
static void handler_softap_get_beacon_interval(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct beacon_interval_ctx *ctx = (struct beacon_interval_ctx *)user_ctx;
    extern uint16_t g_beacon_int;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    ctx->interval = g_beacon_int;
    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set SoftAP beacon vendor specific IE handler
 */
static void handler_softap_set_vendor_elements(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct vendor_elements_ctx *ctx = (struct vendor_elements_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_softap_set_vendor_elements(wpa_s, ctx->data, ctx->len);
    os_sem_post(priv->finish);
}

/**
 * @brief Set SoftAP beacon interval handler
 */
static void handler_softap_set_beacon_interval(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct beacon_interval_ctx *ctx = (struct beacon_interval_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    priv->status = wpas_core_softap_set_beacon_interval(wpa_s, ctx->interval);
    os_sem_post(priv->finish);
}

/**
 * @brief Set scan expire age handler
 */
static void handler_scan_set_expire(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct scan_expire_ctx *ctx = (struct scan_expire_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (wpa_supplicant_set_bss_expiration_age(wpa_s, ctx->age) != 0) {
        priv->status = SYS_ERR;
    } else {
        priv->status = SYS_OK;
    }
    os_sem_post(priv->finish);
}

/**
 * @brief Flush BSS scan results handler
 */
static void handler_scan_flush_bss(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct scan_flush_ctx *ctx = (struct scan_flush_ctx *)user_ctx;

    if (!priv || !ctx) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    if (wpas_core_scan_flush_bss(wpa_s, ctx->flush_age) != 0) {
        priv->status = SYS_ERR;
    } else {
        priv->status = SYS_OK;
    }
    os_sem_post(priv->finish);
}

/**
 * @brief Set PCI mode handler
 */
static void handler_sta_set_pci_mode(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct pci_mode_ctx *ctx = (struct pci_mode_ctx *)user_ctx;

    if (!priv || !ctx || !wpa_s->conf) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    wpa_s->conf->pci_mode = ctx->pci_enable;

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Get PCI mode handler
 */
static void handler_sta_get_pci_mode(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct pci_mode_ctx *ctx = (struct pci_mode_ctx *)user_ctx;

    if (!priv || !ctx || !wpa_s->conf) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    ctx->pci_enable = wpa_s->conf->pci_mode;

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set station authentication mode threshold handler
 */
static void handler_sta_set_authmode_threshold(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    struct authmode_threshold_ctx *ctx =
        (struct authmode_threshold_ctx *)user_ctx;

    if (!priv || !ctx || !wpa_s->conf) {
        if (priv) {
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
        }
        return;
    }

    wpa_s->conf->authmode_threshold = ctx->authmode_threshold;

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/* Note: Implementation of wpa_configure_network_struct is in wpas_struct_iface.c */

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

/**
 * @brief Configure network (unified interface - using WPA types)
 * @param vif_id Virtual interface ID
 * @param config WPA network configuration (wpa_network_config_t*)
 * @return sys_err_t Execution result
 *
 * Note: Upper layer system_wifi.c needs to convert wifi_sta_config_t to wpa_network_config_t
 */
sys_err_t wpas_ctrl_network_start(uint8_t vif_id, const wpa_network_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!config) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    /* Directly pass WPA type, no conversion needed */
    return execute_eloop_work_sync(priv, handler_configure_network, priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_network_stop(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;
    struct wpa_supplicant *wpa_s;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    wpa_s = priv->wpa_s;

    return execute_eloop_work_sync(priv, handler_remove_network, (void *)wpa_s, NULL);
}

sys_err_t wpas_ctrl_network_disconnect(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_disconnect_network, priv->wpa_s, NULL);
}

sys_err_t wpas_ctrl_network_reconnect(unsigned char vif_id,
                                       const wpa_network_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_reconnect_network, priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_network_get_scan_result(unsigned char vif_id,
                        struct wpas_network_scan_result *result)
{
    struct ctrl_iface_priv *priv;
    if (!result || vif_id >= ESWIN_WIFI_IF_NUM)
        return SYS_ERR_INVALID_ARG;
    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s)
        return SYS_ERR_INVALID_STATE;
    priv = global_ctrl_if->ctrl_if[vif_id];
    return execute_eloop_work_sync(priv, handler_network_scan_result,
                                  priv->wpa_s, result);
}

sys_err_t wpas_ctrl_network_update_scan_hint(unsigned char vif_id,
                        const wpas_network_scan_hint_t *hint)
{
    struct ctrl_iface_priv *priv;
    if (!hint || vif_id >= ESWIN_WIFI_IF_NUM ||
        (hint->candidate_frequency && !hint->frequency))
        return SYS_ERR_INVALID_ARG;
    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s)
        return SYS_ERR_INVALID_STATE;
    priv = global_ctrl_if->ctrl_if[vif_id];
    return execute_eloop_work_sync(priv, handler_network_scan_hint,
                                  priv->wpa_s, (void *)hint);
}

sys_err_t wpas_ctrl_network_has_pmksa(unsigned char vif_id,
                                       const wpa_network_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_network_has_pmksa,
                                   priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_pmk_cache_clean(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_pmk_cache_clean, priv->wpa_s, NULL);
}

/* Implementation of wpa_start_scan_struct is in wpas_struct_iface.c */

/* ============================================================================
 * Scan Operations
 * ============================================================================ */

/**
 * @brief Start scan (using WPA types)
 * @param vif_id Virtual interface ID
 * @param config WPA scan configuration (wpa_scan_config_t*)
 * @return sys_err_t Execution result
 *
 * Note: Upper layer system_wifi.c needs to convert wifi_scan_config_t to wpa_scan_config_t
 */
sys_err_t wpas_ctrl_scan_start(uint8_t vif_id, const wpa_scan_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!config) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    /* Directly pass WPA type, no conversion needed */
    return execute_eloop_work_sync(priv, handler_scan_start, priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_scan_start_checked(uint8_t vif_id, const wpa_scan_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!config) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    /* Directly pass WPA type, no conversion needed */
    return execute_eloop_work_sync(priv, handler_scan_start_checked, priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_scan_stop(uint8_t vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_scan_stop, priv->wpa_s, NULL);
}

/**
 * @brief Get scan result count
 * @param vif_id Virtual interface ID
 * @return int Scan result count
 */
sys_err_t wpas_ctrl_scan_get_count_checked(uint8_t vif_id, unsigned int *count)
{
    struct ctrl_iface_priv *priv;
    struct scan_count_ctx ctx;
    sys_err_t ret;

    if (!count || vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.count = 0;

    ret = execute_eloop_work_sync(priv, handler_scan_get_count,
                              priv->wpa_s, (void *)&ctx);

    if (ret != SYS_OK) {
        return ret;
    }

    *count = ctx.count;
    return SYS_OK;
}

int wpas_ctrl_scan_get_count(uint8_t vif_id)
{
    unsigned int count = 0;
    return wpas_ctrl_scan_get_count_checked(vif_id, &count) == SYS_OK ? (int)count : 0;
}

/**
 * @brief Get scan result by index (using WPA types)
 * @param vif_id Virtual interface ID
 * @param index Index
 * @param info Output: WPA scan result information
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_scan_get_result_by_index(unsigned char  vif_id, unsigned int index,
                                    wpa_wifi_info_t *info)
{
    struct ctrl_iface_priv *priv;
    struct get_scan_result_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.index = index;
    ctx.info = info;

    return execute_eloop_work_sync(priv, handler_scan_get_result, priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Get scan result by SSID (using WPA types)
 * @param vif_id Virtual interface ID
 * @param ssid SSID
 * @param info Output: WPA scan result information
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_scan_get_result_by_ssid(unsigned char  vif_id, const char *ssid,
                                            wpa_wifi_info_t *info)
{
    struct ctrl_iface_priv *priv;
    struct get_scan_by_ssid_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!ssid || !info) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.ssid = ssid;
    ctx.info = info;

    return execute_eloop_work_sync(priv, handler_get_scan_result_by_ssid, priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Set scan result expiration age
 * @param vif_id Virtual interface ID
 * @param age Expiration age in seconds (minimum 10 seconds)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_scan_set_expire(unsigned char vif_id, unsigned int age)
{
    struct ctrl_iface_priv *priv;
    struct scan_expire_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Minimum age is 10 seconds per WPA supplicant requirement */
    if (age < 10) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.age = age;

    return execute_eloop_work_sync(priv, handler_scan_set_expire, priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Flush BSS scan results
 * @param vif_id Virtual interface ID
 * @param flush_age Age in seconds (0 = flush all, >0 = flush entries older than this)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_scan_flush_bss(unsigned char vif_id, int flush_age)
{
    struct ctrl_iface_priv *priv;
    struct scan_flush_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.flush_age = flush_age;

    return execute_eloop_work_sync(priv, handler_scan_flush_bss, priv->wpa_s, (void *)&ctx);
}

/* Note: Implementation of wpa_config_softap_csa_struct is in wpas_struct_iface.c */

/* ============================================================================
 * SoftAP Management
 * ============================================================================ */

sys_err_t wpas_ctrl_softap_config_csa(uint8_t vif_id, const wpa_csa_config_t *config)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if(!config) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_softap_config_csa, priv->wpa_s, (void *)config);
}

sys_err_t wpas_ctrl_softap_set_max_sta(unsigned short max_sta)
{
    struct ctrl_iface_priv *priv;
    struct set_max_sta_ctx ctx;

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[1] ||
        !global_ctrl_if->ctrl_if[1]->wpa_s) {
        wpa_printf(MSG_ERROR, "wpa_s is NULL\n");
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[1];
    ctx.max_sta = max_sta;

    return execute_eloop_work_sync(priv, handler_softap_set_max_sta,
                              priv->wpa_s, (void *)&ctx);
}

sys_err_t wpas_ctrl_softap_deauth_sta(uint8_t vif_id, const unsigned char  *sta_mac, unsigned short reason)
{
    struct ctrl_iface_priv *priv;
    static char txtaddr[64];

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!sta_mac) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    /* Format MAC address and reason: "xx:xx:xx:xx:xx:xx reason=N" */
    snprintf(txtaddr, sizeof(txtaddr), "%02x:%02x:%02x:%02x:%02x:%02x reason=%d",
             sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], reason);

    return execute_eloop_work_sync(priv, handler_softap_deauth_sta, priv->wpa_s, (void *)txtaddr);
}

/* ============================================================================
 * Station Information
 * ============================================================================ */

/**
 * @brief Get current WiFi connection information (using WPA types)
 * @param vif_id Virtual interface ID
 * @param info Output: WPA WiFi information
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_station_get_info(unsigned char  vif_id, wpa_wifi_info_t *info)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_station_get_info, priv->wpa_s, (void *)info);
}

sys_err_t wpas_ctrl_station_get_ap_country_info(unsigned char vif_id, wifi_country_info_t *info)
{
    struct ctrl_iface_priv *priv;
    struct get_ap_country_info_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.info = info;

    return execute_eloop_work_sync(priv, handler_station_get_ap_country_info,
                                   priv->wpa_s, (void *)&ctx);
}

sys_err_t wpas_ctrl_rrm_neighbor_req(unsigned char vif_id,
                                     const unsigned char *ssid,
                                     unsigned int ssid_len)
{
#ifdef CONFIG_WPA_KVR_SUPPORT
    struct ctrl_iface_priv *priv;
    struct rrm_neighbor_req_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if ((!ssid && ssid_len) || (ssid && (ssid_len == 0 || ssid_len > SSID_MAX_LEN))) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.ssid = ssid;
    ctx.ssid_len = ssid_len;

    return execute_eloop_work_sync(priv, handler_rrm_neighbor_req,
                                   priv->wpa_s, (void *)&ctx);
#else /* CONFIG_WPA_KVR_SUPPORT */
    (void)vif_id;
    (void)ssid;
    (void)ssid_len;
    return SYS_ERR_NOT_SUPPORTED;
#endif /* CONFIG_WPA_KVR_SUPPORT */
}

sys_err_t wpas_ctrl_wnm_bss_query(unsigned char vif_id,
                                  unsigned char reason,
                                  int cand_list)
{
#ifdef CONFIG_WPA_KVR_SUPPORT
    struct ctrl_iface_priv *priv;
    struct wnm_bss_query_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.reason = reason;
    ctx.cand_list = cand_list ? 1 : 0;

    return execute_eloop_work_sync(priv, handler_wnm_bss_query,
                                   priv->wpa_s, (void *)&ctx);
#else /* CONFIG_WPA_KVR_SUPPORT */
    (void)vif_id;
    (void)reason;
    (void)cand_list;
    return SYS_ERR_NOT_SUPPORTED;
#endif /* CONFIG_WPA_KVR_SUPPORT */
}

sys_err_t wpas_ctrl_sta_roam(unsigned char vif_id,
                             const unsigned char *bssid)
{
#ifdef CONFIG_WPA_KVR_SUPPORT
    struct ctrl_iface_priv *priv;
    struct sta_roam_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!bssid) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.bssid = bssid;

    return execute_eloop_work_sync(priv, handler_sta_roam,
                                   priv->wpa_s, (void *)&ctx);
#else /* CONFIG_WPA_KVR_SUPPORT */
    (void)vif_id;
    (void)bssid;
    return SYS_ERR_NOT_SUPPORTED;
#endif /* CONFIG_WPA_KVR_SUPPORT */
}

/**
 * @brief Get SoftAP SSID, password and channel
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param info Output: WPA WiFi information (contains ssid/pwd/channel)
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_get_config(unsigned char  vif_id, wpa_network_config_t *info)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_softap_get_config, priv->wpa_s, (void *)info);
}

/**
 * @brief Get SoftAP connected STA list (using WPA types and unified handler)
 * @param vif_id Virtual interface ID (usually WIFI_VIF_AP)
 * @param info Output: STA information array
 * @param num Input/Output: array size / actual STA count
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_softap_get_sta_list(unsigned char vif_id, wpa_softap_sta_info_t *info,
                                 unsigned int *num)
{
    struct ctrl_iface_priv *priv;
    struct get_sta_list_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!info || !num) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.info = info;
    ctx.num = num;
    ctx.current = 0;

    return execute_eloop_work_sync(priv, handler_softap_get_sta_list, priv->wpa_s, (void *)&ctx);
}

/* ============================================================================
 * System Configuration
 * ============================================================================ */

/**
 * @brief Get country code
 * @param country_code Pointer to country code buffer
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_system_get_country(char *country_code)
{
    struct ctrl_iface_priv *priv;
    struct country_code_ctx ctx;
    sys_err_t ret;

    if (!country_code) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[0];
    memset(ctx.country_code, 0, sizeof(ctx.country_code));

    ret = execute_eloop_work_sync(priv, handler_system_get_country,
                              priv->wpa_s, (void *)&ctx);

    if (ret == SYS_OK) {
        memcpy(country_code, ctx.country_code, 3);
    }

    return ret;
}

/**
 * @brief Set country code
 * @param country_code Pointer to country code string
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_system_set_country(const char *country_code)
{
    struct ctrl_iface_priv *priv;
    struct country_code_ctx ctx;

    if (!country_code || strlen(country_code) != 2) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[0];
    memcpy(ctx.country_code, country_code, 2);
    ctx.country_code[2] = '\0';

    return execute_eloop_work_sync(priv, handler_system_set_country,
                              priv->wpa_s, (void *)&ctx);
}
sys_err_t wpas_ctrl_system_set_log_level(int level)
{
    struct ctrl_iface_priv *priv;
    struct log_level_ctx ctx;

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[0];
    ctx.level = level;

    return execute_eloop_work_sync(priv, handler_system_set_log_level,
                              priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Set PCI mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pci_enable PCI mode: 0=allow OPEN/WEP, 1=reject OPEN/WEP
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_sta_set_pci_mode(uint8_t vif_id, int pci_enable)
{
    struct ctrl_iface_priv *priv;
    struct pci_mode_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.pci_enable = pci_enable;

    return execute_eloop_work_sync(priv, handler_sta_set_pci_mode, priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Get PCI mode for station
 * @param vif_id Virtual interface ID (should be WIFI_VIF_STA)
 * @param pci_enable Output: current PCI mode
 * @return sys_err_t Execution result
 */
sys_err_t wpas_ctrl_sta_get_pci_mode(uint8_t vif_id, int *pci_enable)
{
    struct ctrl_iface_priv *priv;
    struct pci_mode_ctx ctx;
    sys_err_t ret;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!pci_enable) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.pci_enable = 0;

    ret = execute_eloop_work_sync(priv, handler_sta_get_pci_mode, priv->wpa_s, (void *)&ctx);

    if (ret == SYS_OK) {
        *pci_enable = ctx.pci_enable;
    }

    return ret;
}

/**
 * @brief Set the weakest authentication mode accepted by station
 */
sys_err_t wpas_ctrl_sta_set_authmode_threshold(
    uint8_t vif_id, wpa_auth_mode_e authmode_threshold)
{
    struct ctrl_iface_priv *priv;
    struct authmode_threshold_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM ||
        authmode_threshold < WPA_CTRL_AUTH_OPEN ||
        authmode_threshold > WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.authmode_threshold = authmode_threshold;

    return execute_eloop_work_sync(priv, handler_sta_set_authmode_threshold,
                                   priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Set WiFi country information (supports 2.4G + 5G)
 * @param info Pointer to country information structure
 * @return 0 on success, -1 on failure
 */
int wpas_set_country_info(const wifi_country_info_t *info)
{
    struct ctrl_iface_priv *priv;
    struct country_info_ctx ctx;
    sys_err_t ret;

    if (!info) {
        return -1;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return -1;
    }

    priv = global_ctrl_if->ctrl_if[0];
    memcpy(&ctx.info, info, sizeof(wifi_country_info_t));

    ret = execute_eloop_work_sync(priv, handler_system_set_country_info,
                                  priv->wpa_s, (void *)&ctx);

    return (ret == SYS_OK) ? 0 : -1;
}

int wpas_ctrl_update_country_on_sta_event(const wifi_country_info_t *ap_country)
{
    struct ctrl_iface_priv *priv;

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }
    priv = global_ctrl_if->ctrl_if[0];
    return execute_eloop_work_sync(priv, handler_sta_country_changed,
                                   priv->wpa_s, (void *)ap_country);
}

/**
 * @brief Get WiFi country information
 * @param info Pointer to output buffer for country information
 * @return 0 on success, -1 on failure
 */
int wpas_get_country_info(wifi_country_info_t *info)
{
    struct ctrl_iface_priv *priv;
    struct country_info_ctx ctx;
    sys_err_t ret;

    if (!info) {
        return -1;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[0] ||
        !global_ctrl_if->ctrl_if[0]->wpa_s) {
        return -1;
    }

    priv = global_ctrl_if->ctrl_if[0];
    memset(&ctx.info, 0, sizeof(wifi_country_info_t));

    ret = execute_eloop_work_sync(priv, handler_system_get_country_info,
                                  priv->wpa_s, (void *)&ctx);

    if (ret == SYS_OK) {
        memcpy(info, &ctx.info, sizeof(wifi_country_info_t));
    }

    return (ret == SYS_OK) ? 0 : -1;
}

/* ============================================================================
 * Interface Control
 * ============================================================================ */

sys_err_t wpas_ctrl_iface_dump_status(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_iface_dump_status, priv->wpa_s, (void *)NULL);
}

sys_err_t wpas_ctrl_iface_update_mac(unsigned char vif_id, unsigned char * mac)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_iface_update_mac, priv->wpa_s, (void *)mac);
}

sys_err_t wpas_ctrl_iface_dump_list_networks(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] || !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    return execute_eloop_work_sync(priv, handler_iface_dump_list_networks, priv->wpa_s, NULL);
}

unsigned int wpas_ctrl_softap_get_beacon_interval(void)
{
    struct ctrl_iface_priv *priv;
    struct beacon_interval_ctx ctx;
    sys_err_t ret;

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[1] ||
        !global_ctrl_if->ctrl_if[1]->wpa_s) {
        extern uint16_t g_beacon_int;
        return g_beacon_int;  // fallback to direct access if eloop unavailable
    }

    priv = global_ctrl_if->ctrl_if[1];
    ctx.interval = 0;

    ret = execute_eloop_work_sync(priv, handler_softap_get_beacon_interval,
                              priv->wpa_s, (void *)&ctx);

    if (ret != SYS_OK) {
        extern uint16_t g_beacon_int;
        return g_beacon_int;  // fallback on error
    }

    return ctx.interval;
}

sys_err_t wpas_ctrl_softap_set_vendor_elements(unsigned char vif_id,
                                               const unsigned char *data,
                                               unsigned int len)
{
    struct ctrl_iface_priv *priv;
    struct vendor_elements_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_WIFI_AP_NOT_STARTED;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.data = data;
    ctx.len = len;

    return execute_eloop_work_sync(priv, handler_softap_set_vendor_elements,
                                   priv->wpa_s, (void *)&ctx);
}

sys_err_t wpas_ctrl_softap_set_beacon_interval(unsigned char vif_id,
                                               unsigned int interval)
{
    struct ctrl_iface_priv *priv;
    struct beacon_interval_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_WIFI_AP_NOT_STARTED;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.interval = interval;

    return execute_eloop_work_sync(priv, handler_softap_set_beacon_interval,
                                   priv->wpa_s, (void *)&ctx);
}


sys_err_t wpas_ctrl_update_wifi_rssi(signed char rssi)
{
    struct wpa_supplicant *wpa_s;
    struct wpa_bss  *bss = NULL;

    wpa_s = wpas_internal_get_iface(0);

    if (wpa_s)
        bss = wpa_s->current_bss;

    if (bss)
        bss->level = rssi;

    return SYS_OK;
}

int wpas_core_get_glance_scan_mode(const u8 *ies, size_t len, u16 caps, uint8_t *auth, uint8_t *cipher)
{
    const u8 *ie = NULL, *ie2 = NULL;
    const u8 *end, *pos;
    struct wpa_ie_data data;

    if (!auth || !cipher)
        return -1;

    *auth = WPA_CTRL_AUTH_MAX;
    *cipher = WPA_CTRL_CIPHER_NONE;
    if (!ies)
        return -1;
#ifdef CONFIG_WIFI_WAPI
    ie = get_ie(ies, len, WLAN_EID_WAPI);
    if (ie) {
        if (wpa_parse_wapi_ie(ie, ie[1] + 2, &data) < 0)
            return -1;
        *auth = (data.key_mgmt & WPA_KEY_MGMT_WAPI_PSK) ?
            WPA_CTRL_AUTH_WAPI_PSK : WPA_CTRL_AUTH_WAPI_CERT;
        *cipher = WPA_CTRL_CIPHER_SMS4;
        return SYS_OK;
    }
#endif

    pos = (const u8 *)(ies);
    end = pos + len;

    while (end - pos > 1) {
        if (2 + pos[1] > end - pos)
            break;
        if (pos[0] == WLAN_EID_VENDOR_SPECIFIC && pos[1] >= 4 && WPA_IE_VENDOR_TYPE == WPA_GET_BE32(&pos[2]))
            ie = pos;
        pos += 2 + pos[1];
    }
    ie2 = (const u8 *)get_ie(ies, len, WLAN_EID_RSN);

    if (ie || ie2) {
        if (ie && !ie2) {
            if (wpa_parse_wpa_ie(ie, ie[1] + 2, &data) < 0)
                return -1;

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256)
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            else if (data.pairwise_cipher & WPA_CIPHER_CCMP)
                *cipher = WPA_CTRL_CIPHER_CCMP;
            else
                *cipher = WPA_CTRL_CIPHER_TKIP;

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            *auth = wpas_core_classify_enterprise_auth(data.key_mgmt,
                                                       data.capabilities,
                                                       WPA_PROTO_WPA);
            if (*auth == WPA_CTRL_AUTH_OPEN) {
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
                *auth = WPA_CTRL_AUTH_WPA_PSK;
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            }
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
        } else if (!ie && ie2) {
            if (wpa_parse_wpa_ie(ie2, ie2[1] + 2, &data) < 0)
                return -1;

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256)
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            else if (data.pairwise_cipher & WPA_CIPHER_CCMP)
                *cipher = WPA_CTRL_CIPHER_CCMP;
            else
                *cipher = WPA_CTRL_CIPHER_TKIP;

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            *auth = wpas_core_classify_enterprise_auth(data.key_mgmt,
                                                       data.capabilities,
                                                       WPA_PROTO_RSN);
            if (*auth != WPA_CTRL_AUTH_OPEN) {
                /* Enterprise mode already classified from AKM and MFPR. */
            } else
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
            if (data.key_mgmt & WPA_KEY_MGMT_SAE && data.key_mgmt & WPA_KEY_MGMT_PSK) {
                *auth = WPA_CTRL_AUTH_WPA2_WPA3_PSK;
            } else if (data.key_mgmt & WPA_KEY_MGMT_SAE) {
                *auth = WPA_CTRL_AUTH_WPA3_SAE;
            } else {
                *auth = WPA_CTRL_AUTH_WPA2_PSK;
            }
        } else  // if(ie && ie2)
        {
            if (wpa_parse_wpa_ie(ie2, ie2[1] + 2, &data) < 0)
                return -1;

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256)
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            else if (data.pairwise_cipher & WPA_CIPHER_CCMP)
                *cipher = WPA_CTRL_CIPHER_CCMP;
            else
                *cipher = WPA_CTRL_CIPHER_TKIP;

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            *auth = wpas_core_classify_enterprise_auth(data.key_mgmt,
                                                       data.capabilities,
                                                       WPA_PROTO_WPA | WPA_PROTO_RSN);
            if (*auth != WPA_CTRL_AUTH_OPEN) {
                /* Enterprise mode already classified from AKM and MFPR. */
            } else
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
            {
                *auth = WPA_CTRL_AUTH_WPA_WPA2_PSK;
            }
        }
    } else if (caps & IEEE80211_CAP_PRIVACY) {
        *auth = WPA_CTRL_AUTH_WEP;
        *cipher = WPA_CTRL_CIPHER_WEP40;
    } else {
        *auth = WPA_CTRL_AUTH_OPEN;
        *cipher = WPA_CTRL_CIPHER_NONE;
    }

    return SYS_OK;
}

int wpas_core_get_glance_scan_extra_info(const u8 *ies, size_t len, u16 caps,
                                         int freq,
                                         uint8_t *group_cipher,
                                         uint8_t *wifi_protocol,
                                         uint8_t *wps_flag)
{
    const u8 *ie, *ie2, *pos, *end;
    struct wpa_ie_data data;

    if (!group_cipher || !wifi_protocol || !wps_flag)
        return -1;

    *group_cipher = WPA_CTRL_CIPHER_NONE;
    *wifi_protocol = 0;
    *wps_flag = 0;

    /* --- group_cipher --- */
    ie = NULL;
    pos = ies;
    end = pos + len;
    while (end - pos > 1) {
        if (2 + pos[1] > end - pos)
            break;
        if (pos[0] == WLAN_EID_VENDOR_SPECIFIC && pos[1] >= 4 &&
            WPA_IE_VENDOR_TYPE == WPA_GET_BE32(&pos[2]))
            ie = pos;
        pos += 2 + pos[1];
    }
    ie2 = (const u8 *)get_ie(ies, len, WLAN_EID_RSN);

#ifdef CONFIG_WIFI_WAPI
    const u8 *wapi_ie = get_ie(ies, len, WLAN_EID_WAPI);
    if (wapi_ie) {
        if (wpa_parse_wapi_ie(wapi_ie, wapi_ie[1] + 2, &data) == 0)
            *group_cipher = WPA_CTRL_CIPHER_SMS4;
    } else
#endif
    if (ie2) {
        if (wpa_parse_wpa_ie(ie2, ie2[1] + 2, &data) == 0) {
            if (data.group_cipher & WPA_CIPHER_CCMP)
                *group_cipher = WPA_CTRL_CIPHER_CCMP;
            else if (data.group_cipher & WPA_CIPHER_TKIP)
                *group_cipher = WPA_CTRL_CIPHER_TKIP;
            else if (data.group_cipher & WPA_CIPHER_WEP104)
                *group_cipher = WPA_CTRL_CIPHER_WEP104;
            else if (data.group_cipher & WPA_CIPHER_WEP40)
                *group_cipher = WPA_CTRL_CIPHER_WEP40;
        }
    } else if (ie) {
        if (wpa_parse_wpa_ie(ie, ie[1] + 2, &data) == 0) {
            if (data.group_cipher & WPA_CIPHER_CCMP)
                *group_cipher = WPA_CTRL_CIPHER_CCMP;
            else if (data.group_cipher & WPA_CIPHER_TKIP)
                *group_cipher = WPA_CTRL_CIPHER_TKIP;
            else if (data.group_cipher & WPA_CIPHER_WEP104)
                *group_cipher = WPA_CTRL_CIPHER_WEP104;
            else if (data.group_cipher & WPA_CIPHER_WEP40)
                *group_cipher = WPA_CTRL_CIPHER_WEP40;
        }
    } else if (caps & IEEE80211_CAP_PRIVACY) {
        *group_cipher = WPA_CTRL_CIPHER_WEP40;
    }

    /* --- wifi_protocol --- */
    if (freq > 0 && freq < 4000) {
        /* 2.4 GHz: 802.11b + 802.11g */
        *wifi_protocol |= BIT(0) | BIT(1);
    } else if (freq >= 5000) {
        /* 5 GHz: 802.11a */
        *wifi_protocol |= BIT(5);
    }
    if (get_ie(ies, len, WLAN_EID_HT_CAP))
        *wifi_protocol |= BIT(2);   /* 802.11n */
    if (get_ie(ies, len, WLAN_EID_VHT_CAP))
        *wifi_protocol |= BIT(6);   /* 802.11ac */
    /* 802.11ax: extension element with ext_id = HE Capabilities */
    pos = ies;
    end = pos + len;
    while (end - pos > 1) {
        if (2 + pos[1] > end - pos)
            break;
        if (pos[0] == WLAN_EID_EXTENSION && pos[1] >= 1 &&
            pos[2] == WLAN_EID_EXT_HE_CAPABILITIES) {
            *wifi_protocol |= BIT(4);  /* 802.11ax */
            break;
        }
        pos += 2 + pos[1];
    }

    /* --- wps --- */
    pos = ies;
    end = pos + len;
    while (end - pos > 1) {
        if (2 + pos[1] > end - pos)
            break;
        if (pos[0] == WLAN_EID_VENDOR_SPECIFIC && pos[1] >= 4 &&
            WPS_IE_VENDOR_TYPE == WPA_GET_BE32(&pos[2])) {
            *wps_flag = 1;
            break;
        }
        pos += 2 + pos[1];
    }

    return 0;
}

/**
 * @brief Timeout handler for external radio work
 *
 * Called when an external radio work times out without completion.
 * Cleans up the work item and frees associated resources.
 *
 * @param eloop_ctx  Pointer to wpa_radio_work structure
 * @param timeout_ctx  Timeout context (unused)
 */
static void wpas_ctrl_radio_work_timeout(void *eloop_ctx, void *timeout_ctx)
{
    struct wpa_radio_work *work = eloop_ctx;
    struct wpa_external_work *ework = work->ctx;

    wpa_dbg(work->wpa_s, MSG_DEBUG,
            "Timing out external radio work %u (%s)",
            ework->id, work->type);
    wpa_msg(work->wpa_s, MSG_INFO, EXT_RADIO_WORK_TIMEOUT "%u", ework->id);
    work->wpa_s->ext_work_in_progress = 0;
    radio_work_done(work);
    os_free(ework);
}

/**
 * @brief Flush all external radio work items
 *
 * This function flushes all external radio work items from the wpa_supplicant
 * work queue. It is called when the interface is being removed to ensure
 * all pending external work is properly cleaned up.
 *
 * Originally defined in ctrl_iface.c, moved here as we replaced ctrl_iface
 * with direct struct interface (wpas_cntrl_api).
 *
 * @param wpa_s  wpa_supplicant instance
 */
void wpas_ctrl_radio_work_flush(struct wpa_supplicant *wpa_s)
{
    struct wpa_radio_work *work, *tmp;

    if (!wpa_s || !wpa_s->radio)
        return;

    dl_list_for_each_safe(work, tmp, &wpa_s->radio->work,
                          struct wpa_radio_work, list) {
        struct wpa_external_work *ework;

        if (os_strncmp(work->type, "ext:", 4) != 0)
            continue;
        ework = work->ctx;
        wpa_dbg(wpa_s, MSG_DEBUG,
                "Flushing%s external radio work %u (%s)",
                work->started ? " started" : "", ework->id,
                ework->type);
        if (work->started)
            eloop_cancel_timeout(wpas_ctrl_radio_work_timeout,
                                work, NULL);
        radio_work_done(work);
        os_free(ework);
    }
}


/* ============================================================================
 * STA Connection Policy
 * ============================================================================ */

/**
 * @brief Set STA connection policy
 * @param vif_id Virtual interface ID
 * @param interval_sec Reconnection interval in seconds (0 = use default)
 * @param repeat_count Maximum reconnection attempts (0 = unlimited)
 * @return sys_err_t Execution result
 */
int wpas_ctrl_sta_set_connect_policy(unsigned char vif_id,
                                      int interval_sec,
                                      int repeat_count)
{
    struct ctrl_iface_priv *priv;
    struct sta_connect_policy_ctx ctx;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (interval_sec < 0 || repeat_count < 0) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.interval_sec = interval_sec;
    ctx.repeat_count = repeat_count;

    return execute_eloop_work_sync(priv, handler_sta_set_connect_policy,
                                   priv->wpa_s, (void *)&ctx);
}

/**
 * @brief Get current STA connection policy
 * @param vif_id Virtual interface ID
 * @param interval_sec Output parameter for reconnection interval (seconds)
 * @param repeat_count Output parameter for maximum reconnection attempts
 * @return sys_err_t Execution result
 */
int wpas_ctrl_sta_get_connect_policy(unsigned char vif_id,
                                      int *interval_sec,
                                      int *repeat_count)
{
    struct ctrl_iface_priv *priv;
    struct sta_connect_policy_get_ctx ctx;
    sys_err_t ret;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return SYS_ERR_INVALID_ARG;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return SYS_ERR_INVALID_STATE;
    }

    if (!interval_sec || !repeat_count) {
        return SYS_ERR_INVALID_ARG;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];
    ctx.interval_sec = interval_sec;
    ctx.repeat_count = repeat_count;

    ret = execute_eloop_work_sync(priv, handler_sta_get_connect_policy,
                                   priv->wpa_s, (void *)&ctx);

    return ret;
}

/* ============================================================================
 * Hardware Feature Management
 * ============================================================================ */

/**
 * @brief Refresh hardware feature cache for specific VIF
 * @param vif_id Virtual interface ID (0=STA, 1=AP)
 * @return 0 on success, negative on error
 *
 * This function updates the cached hardware feature data (wpa_s->hw.modes)
 * after runtime configuration changes (protocol_bitmap).
 *
 * Call this after wifi_set_protocol to ensure next AP startup uses new config.
 */
int wpas_ctrl_refresh_hw_feature(unsigned char vif_id)
{
    struct ctrl_iface_priv *priv;

    if (vif_id >= ESWIN_WIFI_IF_NUM) {
        return -1;
    }

    if (!global_ctrl_if || !global_ctrl_if->ctrl_if[vif_id] ||
        !global_ctrl_if->ctrl_if[vif_id]->wpa_s) {
        return -1;
    }

    priv = global_ctrl_if->ctrl_if[vif_id];

    /* Execute refresh in WPA eloop context */
    return execute_eloop_work_sync(priv, handler_refresh_hw_feature,
                                   priv->wpa_s, NULL);
}

/**
 * @brief Get disconnect reason (simplified enum)
 * @return wifi_discon_reason_e Simplified disconnect reason
 */
uint16_t wpas_ctrl_sta_get_disconnect_reason(void)
{
    return wpa_disconnect_reason();
}

/**
 * @brief Get disconnect reason code (raw IEEE 802.11 code)
 * @return uint16_t Raw IEEE 802.11 reason code
 */
uint16_t wpas_ctrl_sta_get_disconnect_reason_code(void)
{
    return wpa_get_raw_disconnect_reason_code();
}

/* ============================================================================
 * PMF (Protected Management Frames) Control
 * ============================================================================ */

/**
 * @brief Worker function to set PMF mode in eloop thread context
 */
static void wpas_ctrl_sta_set_pmf_work(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    int pmf_mode = *(int *)user_ctx;
    enum mfp_options wpa_pmf;

    if (!priv) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf_work: priv is NULL");
        return;
    }

    if (!wpa_s->conf) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf_work: wpa_s->conf is NULL");
        priv->status = SYS_ERR_INVALID_STATE;
        os_sem_post(priv->finish);
        return;
    }

    /* Convert int to enum mfp_options */
    switch (pmf_mode) {
        case 0:
            wpa_pmf = NO_MGMT_FRAME_PROTECTION;
            break;
        case 1:
            wpa_pmf = MGMT_FRAME_PROTECTION_OPTIONAL;
            break;
        case 2:
            wpa_pmf = MGMT_FRAME_PROTECTION_REQUIRED;
            break;
        default:
            wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf_work: Invalid PMF mode %d", pmf_mode);
            priv->status = SYS_ERR_INVALID_ARG;
            os_sem_post(priv->finish);
            return;
    }

    wpa_s->conf->pmf = wpa_pmf;
    wpa_printf(MSG_DEBUG, "PMF mode set to %d (wpa_pmf=%d)", pmf_mode, wpa_pmf);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Set PMF mode for STA
 */
sys_err_t wpas_ctrl_sta_set_pmf(uint8_t vif_id, int pmf_mode)
{
    struct wpa_supplicant *wpa_s = wpas_internal_get_iface(vif_id);
    struct ctrl_iface_priv *priv;

    if (!wpa_s) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf: wpa_s is NULL for vif_id %d", vif_id);
        return SYS_ERR_INVALID_ARG;
    }

    priv = wpas_internal_get_priv(wpa_s);
    if (!priv) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf: priv is NULL");
        return SYS_ERR_INVALID_STATE;
    }

    /* Validate parameters */
    if (pmf_mode < 0 || pmf_mode > 2) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_set_pmf: Invalid PMF mode %d (valid: 0-2)", pmf_mode);
        return SYS_ERR_INVALID_ARG;
    }

    /* Schedule execution in eloop thread */
    os_mutex_lock(priv->mutex, WAIT_FOREVER);
    eloop_register_timeout(0, 0, wpas_ctrl_sta_set_pmf_work, wpa_s, &pmf_mode);
    os_sem_wait(priv->finish, WAIT_FOREVER);
    sys_err_t ret = priv->status;
    os_mutex_unlock(priv->mutex);

    return ret;
}

/**
 * @brief Worker function to get PMF mode in eloop thread context
 */
static void wpas_ctrl_sta_get_pmf_work(void *eloop_data, void *user_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_data;
    struct ctrl_iface_priv *priv = wpas_internal_get_priv(wpa_s);
    int *pmf_mode = (int *)user_ctx;

    if (!priv) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf_work: priv is NULL");
        return;
    }

    if (!wpa_s->conf) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf_work: wpa_s->conf is NULL");
        *pmf_mode = 0;
        priv->status = SYS_ERR_INVALID_STATE;
        os_sem_post(priv->finish);
        return;
    }

    /* Convert enum mfp_options to int */
    switch (wpa_s->conf->pmf) {
        case NO_MGMT_FRAME_PROTECTION:
            *pmf_mode = 0;
            break;
        case MGMT_FRAME_PROTECTION_OPTIONAL:
            *pmf_mode = 1;
            break;
        case MGMT_FRAME_PROTECTION_REQUIRED:
            *pmf_mode = 2;
            break;
        default:
            wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf_work: Unknown PMF value %d", wpa_s->conf->pmf);
            *pmf_mode = 0;
            priv->status = SYS_ERR_INVALID_STATE;
            os_sem_post(priv->finish);
            return;
    }

    wpa_printf(MSG_DEBUG, "Current PMF mode: %d (wpa_pmf=%d)", *pmf_mode, wpa_s->conf->pmf);

    priv->status = SYS_OK;
    os_sem_post(priv->finish);
}

/**
 * @brief Get current PMF mode for STA
 */
sys_err_t wpas_ctrl_sta_get_pmf(uint8_t vif_id, int *pmf_mode)
{
    struct wpa_supplicant *wpa_s = wpas_internal_get_iface(vif_id);
    struct ctrl_iface_priv *priv;

    if (!wpa_s) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf: wpa_s is NULL for vif_id %d", vif_id);
        return SYS_ERR_INVALID_ARG;
    }

    priv = wpas_internal_get_priv(wpa_s);
    if (!priv) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf: priv is NULL");
        return SYS_ERR_INVALID_STATE;
    }

    if (!pmf_mode) {
        wpa_printf(MSG_ERROR, "wpas_ctrl_sta_get_pmf: pmf_mode pointer is NULL");
        return SYS_ERR_INVALID_ARG;
    }

    /* Schedule execution in eloop thread */
    os_mutex_lock(priv->mutex, WAIT_FOREVER);
    eloop_register_timeout(0, 0, wpas_ctrl_sta_get_pmf_work, wpa_s, pmf_mode);
    os_sem_wait(priv->finish, WAIT_FOREVER);
    sys_err_t ret = priv->status;
    os_mutex_unlock(priv->mutex);

    return ret;
}
