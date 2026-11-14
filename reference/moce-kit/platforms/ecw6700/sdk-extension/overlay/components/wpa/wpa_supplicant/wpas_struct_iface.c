/**
 * @file wpas_struct_iface.c
 * @brief WPA Supplicant Core Processing Layer Implementation
 *
 * @author WiFi Software Team
 * @date 2025-12
 * @version V1.0
 *
 * @copyright Copyright (c) 2025 Eswin Computing. All rights reserved.
 */

#include "reg_macro_def.h"
#include "utils/common.h"
#include "wpas_struct_iface.h"
#include "wpa_supplicant_i.h"
#include "bss.h"
#include "common/wpa_common.h"
#include "rsn_supp/wpa.h"
#include "ctrl_iface.h"
#include "ap.h"
#include "scan.h"
#include "notify.h"
#include "common/ieee802_11_defs.h"
#include "common/ieee802_11_common.h"
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
#include "crypto/crypto.h"
#include "crypto/sha256.h"
#include "system_wifi_eap_tls_cred.h"
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
#include "config.h"
#include "driver_i.h"
#include "eapol_supp/eapol_supp_sm.h"
#include "eap_peer/eap.h"
#include "eloop.h"
#ifdef CONFIG_AP
#include "../src/ap/hostapd.h"
#include "../src/ap/sta_info.h"
#include "../src/ap/ap_drv_ops.h"
#include "../src/ap/beacon.h"
#endif

/* Macro definitions - avoid including system_wifi.h */
#ifndef IS_ZERO_MAC
#define IS_ZERO_MAC(a)  (!((a)[0] | (a)[1] | (a)[2] | (a)[3] | (a)[4] | (a)[5]))
#endif

extern uint16_t g_beacon_int;
void wpa_update_mac_addr(struct wpa_supplicant *wpa_s, unsigned char * mac);

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
static int wpas_core_is_enterprise_key_mgmt(int key_mgmt)
{
    return wpa_key_mgmt_wpa_ieee8021x(key_mgmt);
}

#if defined(IEEE8021X_EAPOL) && !defined(CONFIG_NO_WPA)
#define WPAS_ENT_CONTEXT_DIGEST_LEN 16
#define WPAS_ENT_CONTEXT_FORMAT_VERSION 1
#define WPAS_ENT_CONTEXT_METADATA_U32_COUNT 7
#define WPAS_ENT_CONTEXT_METADATA_LEN \
    (1 + WPAS_ENT_CONTEXT_METADATA_U32_COUNT * sizeof(u32))

enum wpas_enterprise_context_part {
    WPAS_ENT_CONTEXT_PART_METADATA,
    WPAS_ENT_CONTEXT_PART_SSID,
    WPAS_ENT_CONTEXT_PART_IDENTITY,
    WPAS_ENT_CONTEXT_PART_PASSWORD,
    WPAS_ENT_CONTEXT_PART_PHASE1,
    WPAS_ENT_CONTEXT_PART_PHASE2,
    WPAS_ENT_CONTEXT_PART_COUNT,
};

struct wpas_enterprise_pmksa_snapshot {
    int valid;
    u8 digest[WPAS_ENT_CONTEXT_DIGEST_LEN];
};

static struct wpas_enterprise_pmksa_snapshot g_enterprise_pmksa_snapshot;

static void wpas_core_enterprise_digest_put_u32(u8 **pos, u32 value)
{
    WPA_PUT_BE32(*pos, value);
    *pos += sizeof(value);
}

static int wpas_core_enterprise_context_digest(
    struct wpa_ssid *ssid, u8 digest[WPAS_ENT_CONTEXT_DIGEST_LEN])
{
    const u8 empty = 0;
    u8 metadata[WPAS_ENT_CONTEXT_METADATA_LEN];
    u8 hash[SHA256_MAC_LEN];
    u8 *pos = metadata;
    const u8 *password = &empty;
    const u8 *phase1;
    const u8 *phase2 = &empty;
    size_t password_len = 0;
    size_t phase1_len;
    size_t phase2_len = 0;

    if (!ssid || !ssid->ssid || !ssid->eap.eap_methods ||
        !ssid->eap.identity || !ssid->eap.phase1) {
        return -1;
    }

    if (ssid->eap.password && ssid->eap.password_len) {
        password = ssid->eap.password;
        password_len = ssid->eap.password_len;
    }
    phase1 = (const u8 *)ssid->eap.phase1;
    phase1_len = os_strlen(ssid->eap.phase1);
    if (ssid->eap.phase2) {
        phase2 = (const u8 *)ssid->eap.phase2;
        phase2_len = os_strlen(ssid->eap.phase2);
    }

    *pos++ = WPAS_ENT_CONTEXT_FORMAT_VERSION;
    wpas_core_enterprise_digest_put_u32(&pos, ssid->ssid_len);
    wpas_core_enterprise_digest_put_u32(
        &pos, ssid->eap.eap_methods[0].vendor);
    wpas_core_enterprise_digest_put_u32(
        &pos, ssid->eap.eap_methods[0].method);
    wpas_core_enterprise_digest_put_u32(&pos, ssid->eap.identity_len);
    wpas_core_enterprise_digest_put_u32(&pos, password_len);
    wpas_core_enterprise_digest_put_u32(&pos, phase1_len);
    wpas_core_enterprise_digest_put_u32(&pos, phase2_len);

    const u8 *addr[WPAS_ENT_CONTEXT_PART_COUNT] = {
        [WPAS_ENT_CONTEXT_PART_METADATA] = metadata,
        [WPAS_ENT_CONTEXT_PART_SSID] = ssid->ssid,
        [WPAS_ENT_CONTEXT_PART_IDENTITY] = ssid->eap.identity,
        [WPAS_ENT_CONTEXT_PART_PASSWORD] = password,
        [WPAS_ENT_CONTEXT_PART_PHASE1] = phase1,
        [WPAS_ENT_CONTEXT_PART_PHASE2] = phase2,
    };
    const size_t len[WPAS_ENT_CONTEXT_PART_COUNT] = {
        [WPAS_ENT_CONTEXT_PART_METADATA] = sizeof(metadata),
        [WPAS_ENT_CONTEXT_PART_SSID] = ssid->ssid_len,
        [WPAS_ENT_CONTEXT_PART_IDENTITY] = ssid->eap.identity_len,
        [WPAS_ENT_CONTEXT_PART_PASSWORD] = password_len,
        [WPAS_ENT_CONTEXT_PART_PHASE1] = phase1_len,
        [WPAS_ENT_CONTEXT_PART_PHASE2] = phase2_len,
    };

    if (sha256_vector(ARRAY_SIZE(addr), addr, len, hash) < 0) {
        return -1;
    }

    os_memcpy(digest, hash, WPAS_ENT_CONTEXT_DIGEST_LEN);
    os_memset(hash, 0, sizeof(hash));
    return 0;
}

static void wpas_core_enterprise_pmksa_snapshot_reset(void)
{
    os_memset(&g_enterprise_pmksa_snapshot, 0,
              sizeof(g_enterprise_pmksa_snapshot));
}

static void wpas_core_enterprise_pmksa_snapshot_discard(
    struct wpa_supplicant *wpa_s)
{
    if (wpa_s && wpa_s->wpa) {
        wpa_sm_pmksa_cache_flush(wpa_s->wpa,
                                 &g_enterprise_pmksa_snapshot);
    }
    wpas_core_enterprise_pmksa_snapshot_reset();
}

static void wpas_core_enterprise_pmksa_snapshot_save(
    struct wpa_supplicant *wpa_s, struct wpa_ssid *ssid)
{
    const u8 *bssid = NULL;

    wpas_core_enterprise_pmksa_snapshot_discard(wpa_s);
    if (!wpa_s || !wpa_s->wpa || !ssid ||
        !wpas_core_is_enterprise_key_mgmt(ssid->key_mgmt) ||
        !ssid->eap.eap_methods ||
        ssid->eap.eap_methods[0].vendor != EAP_VENDOR_IETF ||
        ssid->eap.eap_methods[0].method == EAP_TYPE_NONE) {
        return;
    }

    if (!IS_ZERO_MAC(wpa_s->bssid)) {
        bssid = wpa_s->bssid;
    } else if (wpa_s->current_bss) {
        bssid = wpa_s->current_bss->bssid;
    }

    if (!bssid ||
        wpas_core_enterprise_context_digest(ssid, g_enterprise_pmksa_snapshot.digest) < 0 ||
        wpa_sm_pmksa_cache_rebind_network_ctx( wpa_s->wpa, bssid, ssid->key_mgmt, ssid, &g_enterprise_pmksa_snapshot) < 0) {
        wpa_sm_pmksa_cache_flush(wpa_s->wpa, ssid);
        wpas_core_enterprise_pmksa_snapshot_reset();
        return;
    }

    g_enterprise_pmksa_snapshot.valid = 1;
    wpa_sm_pmksa_cache_flush(wpa_s->wpa, ssid);
    wpa_dbg(wpa_s, MSG_DEBUG,
            "Enterprise PMKSA moved to reconnect snapshot");
}

int wpas_core_enterprise_pmksa_snapshot_rebind(struct wpa_supplicant *wpa_s,
                                               struct wpa_ssid *ssid,
                                               const u8 *bssid)
{
    u8 digest[WPAS_ENT_CONTEXT_DIGEST_LEN];

    if (!g_enterprise_pmksa_snapshot.valid) {
        return 0;
    }

    if (!wpa_s || !wpa_s->wpa || !ssid || !bssid ||
        wpas_core_enterprise_context_digest(ssid, digest) < 0 ||
        os_memcmp_const(digest, g_enterprise_pmksa_snapshot.digest, sizeof(digest)) != 0 ||
        wpa_sm_pmksa_cache_rebind_network_ctx(
            wpa_s->wpa, bssid, ssid->key_mgmt,
            &g_enterprise_pmksa_snapshot, ssid) < 0) {
        wpas_core_enterprise_pmksa_snapshot_discard(wpa_s);
        return 0;
    }

    wpas_core_enterprise_pmksa_snapshot_reset();
    return 1;
}
#else
static void wpas_core_enterprise_pmksa_snapshot_save(
    struct wpa_supplicant *wpa_s, struct wpa_ssid *ssid)
{
}

int wpas_core_enterprise_pmksa_snapshot_rebind(struct wpa_supplicant *wpa_s,
                                               struct wpa_ssid *ssid,
                                               const u8 *bssid)
{
    return 0;
}
#endif

wpa_auth_mode_e wpas_core_classify_enterprise_auth(int key_mgmt,
                                                   int capabilities,
                                                   int proto)
{
    if (key_mgmt & WPA_KEY_MGMT_IEEE8021X_SUITE_B_192) {
        return WPA_CTRL_AUTH_WPA3_ENT_192;
    }

    if (key_mgmt & WPA_KEY_MGMT_IEEE8021X_SUITE_B) {
        return WPA_CTRL_AUTH_WPA3_ENTERPRISE;
    }

    if ((key_mgmt & WPA_KEY_MGMT_IEEE8021X) &&
        (key_mgmt & WPA_KEY_MGMT_IEEE8021X_SHA256)) {
        return WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE;
    }

    if ((key_mgmt & WPA_KEY_MGMT_IEEE8021X) &&
        (proto & WPA_PROTO_WPA)) {
        return WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE;
    }

    if (key_mgmt & WPA_KEY_MGMT_IEEE8021X_SHA256) {
        return (capabilities & WPA_CAPABILITY_MFPR) ?
            WPA_CTRL_AUTH_WPA3_ENTERPRISE : WPA_CTRL_AUTH_WPA2_ENTERPRISE;
    }

    if (key_mgmt & WPA_KEY_MGMT_IEEE8021X) {
        return WPA_CTRL_AUTH_WPA2_ENTERPRISE;
    }

    return WPA_CTRL_AUTH_OPEN;
}

static int wpas_core_eap_type_from_method(wpa_eap_method_e method)
{
    switch (method) {
    case WPA_CTRL_EAP_TLS:
        return EAP_TYPE_TLS;
    case WPA_CTRL_EAP_PEAP:
        return EAP_TYPE_PEAP;
    case WPA_CTRL_EAP_TTLS:
        return EAP_TYPE_TTLS;
    case WPA_CTRL_EAP_LEAP:
        return EAP_TYPE_LEAP;
    default:
        return EAP_TYPE_NONE;
    }
}

#ifndef CONFIG_ECR6600_DBGTRIM
static const char *wpas_core_eap_method_txt(wpa_eap_method_e method)
{
    switch (method) {
    case WPA_CTRL_EAP_TLS:
        return "TLS";
    case WPA_CTRL_EAP_PEAP:
        return "PEAP";
    case WPA_CTRL_EAP_TTLS:
        return "TTLS";
    case WPA_CTRL_EAP_LEAP:
        return "LEAP";
    default:
        return "NONE";
    }
}
#endif

static sys_err_t wpas_core_set_eap_methods(struct eap_peer_config *eap,
                                           wpa_eap_method_e method)
{
    int eap_type;

    if (!eap) {
        return SYS_ERR_INVALID_ARG;
    }

    eap_type = wpas_core_eap_type_from_method(method);
    if (eap_type == EAP_TYPE_NONE) {
        return SYS_ERR_INVALID_ARG;
    }

    os_free(eap->eap_methods);
    eap->eap_methods = os_calloc(2, sizeof(*eap->eap_methods));
    if (!eap->eap_methods) {
        return SYS_ERR_NO_MEM;
    }

    eap->eap_methods[0].vendor = EAP_VENDOR_IETF;
    eap->eap_methods[0].method = eap_type;
    eap->eap_methods[1].vendor = EAP_VENDOR_IETF;
    eap->eap_methods[1].method = EAP_TYPE_NONE;

    return SYS_OK;
}

static sys_err_t wpas_core_set_eap_identity(struct eap_peer_config *eap,
                                            const char *identity,
                                            size_t identity_len)
{
#ifdef IEEE8021X_EAPOL
    if (!eap || !identity || !identity_len) {
        return SYS_ERR_INVALID_ARG;
    }

    if (eap->identity && eap->identity_len == identity_len &&
        os_memcmp(eap->identity, identity, identity_len) == 0) {
        return SYS_OK;
    }

    bin_clear_free(eap->identity, eap->identity_len);
    eap->identity = (u8 *) dup_binstr(identity, identity_len);
    if (!eap->identity) {
        eap->identity_len = 0;
        return SYS_ERR_NO_MEM;
    }
    eap->identity_len = identity_len;

    return SYS_OK;
#else
    return SYS_ERR_NOT_SUPPORTED;
#endif
}

static sys_err_t wpas_core_set_eap_password(struct eap_peer_config *eap,
                                            const char *password,
                                            size_t password_len)
{
#ifdef IEEE8021X_EAPOL
    if (!eap || !password || !password_len) {
        return SYS_ERR_INVALID_ARG;
    }

    if (eap->password && eap->password_len == password_len &&
        os_memcmp(eap->password, password, password_len) == 0) {
        return SYS_OK;
    }

    bin_clear_free(eap->password, eap->password_len);
    eap->password = (u8 *) dup_binstr(password, password_len);
    if (!eap->password) {
        eap->password_len = 0;
        return SYS_ERR_NO_MEM;
    }
    eap->password_len = password_len;

    return SYS_OK;
#else
    return SYS_ERR_NOT_SUPPORTED;
#endif
}

static sys_err_t wpas_core_set_eap_phase2(struct eap_peer_config *eap,
                                          const char *phase2)
{
#ifdef IEEE8021X_EAPOL
    char *new_phase2;

    if (!eap || !phase2 || !phase2[0]) {
        return SYS_ERR_INVALID_ARG;
    }

    if (eap->phase2 && os_strcmp(eap->phase2, phase2) == 0) {
        return SYS_OK;
    }

    new_phase2 = os_strdup(phase2);
    if (!new_phase2) {
        return SYS_ERR_NO_MEM;
    }

    os_free(eap->phase2);
    eap->phase2 = new_phase2;

    return SYS_OK;
#else
    return SYS_ERR_NOT_SUPPORTED;
#endif
}

static sys_err_t wpas_core_prepare_eap_config(struct wpa_ssid *ssid,
                                              const wpa_network_config_t *config)
{
#ifdef IEEE8021X_EAPOL
    sys_err_t ret;

    if (!ssid || !config || config->eap_method == WPA_CTRL_EAP_NONE ||
        !config->identity || !config->identity_len) {
        return SYS_ERR_INVALID_ARG;
    }

    ret = wpas_core_set_eap_methods(&ssid->eap, config->eap_method);
    if (ret != SYS_OK) {
        return ret;
    }

    if (ssid->eap.fragment_size <= 0) {
        ssid->eap.fragment_size = DEFAULT_FRAGMENT_SIZE;
    }

    ret = wpas_core_set_eap_identity(&ssid->eap,
                                     config->identity,
                                     config->identity_len);
    if (ret != SYS_OK) {
        return ret;
    }

    if (config->eap_method == WPA_CTRL_EAP_PEAP ||
        config->eap_method == WPA_CTRL_EAP_TTLS ||
        config->eap_method == WPA_CTRL_EAP_LEAP) {
        ret = wpas_core_set_eap_password(&ssid->eap,
                                         config->eap_password,
                                         config->eap_password_len);
        if (ret != SYS_OK) {
            return ret;
        }
    }

    if (config->eap_method == WPA_CTRL_EAP_PEAP ||
        config->eap_method == WPA_CTRL_EAP_TTLS) {
        ret = wpas_core_set_eap_phase2(&ssid->eap, config->phase2);
        if (ret != SYS_OK) {
            return ret;
        }
    }


    wpa_printf(MSG_DEBUG, "EAP: method=%s identity_len=%u fragment_size=%d",
               wpas_core_eap_method_txt(config->eap_method),
         (unsigned int) config->identity_len, ssid->eap.fragment_size);
    return SYS_OK;
#else
    return SYS_ERR_NOT_SUPPORTED;
#endif
}

#ifdef IEEE8021X_EAPOL
static int wpas_core_pick_enterprise_cipher(int advertised, int fallback)
{
    if (advertised & WPA_CIPHER_GCMP_256) {
        return WPA_CIPHER_GCMP_256;
    }

    if (advertised & WPA_CIPHER_CCMP) {
        return WPA_CIPHER_CCMP;
    }

    return fallback;
}

static int wpas_core_pick_enterprise_group_cipher(int advertised, int fallback)
{
    if (advertised & WPA_CIPHER_CCMP) {
        return WPA_CIPHER_CCMP;
    }

    if (advertised & WPA_CIPHER_TKIP) {
        return WPA_CIPHER_TKIP;
    }

    return fallback;
}
#endif

sys_err_t wpas_core_apply_enterprise_auth_ie(struct wpa_ssid *ssid,
                                             wpa_auth_mode_e auth_mode,
                                             const struct wpa_ie_data *ie)
{
#ifdef IEEE8021X_EAPOL
    int phase1_len;
    char phase1[128];
    char *new_phase1;
    wifi_eap_tls_cred_profile_t cred_profile;
    int allow_tls13;
    int force_tls12;
    int is_peap;

    if (!ssid || !ssid->eap.identity || !ssid->eap.identity_len) {
        return SYS_ERR_INVALID_ARG;
    }

    if (auth_mode == WPA_CTRL_AUTH_WPA3_ENT_192) {
        cred_profile = WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT_192;
    } else if (auth_mode == WPA_CTRL_AUTH_WPA3_ENTERPRISE ||
                auth_mode == WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE) {
        cred_profile = WIFI_EAP_TLS_CRED_PROFILE_WPA3_ENT;
    } else if (auth_mode == WPA_CTRL_AUTH_WPA2_ENTERPRISE ||
               auth_mode == WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE) {
        cred_profile = WIFI_EAP_TLS_CRED_PROFILE_WPA2;
    } else {
        return SYS_ERR_INVALID_ARG;
    }

    is_peap = ssid->eap.eap_methods &&
              ssid->eap.eap_methods[0].vendor == EAP_VENDOR_IETF &&
              ssid->eap.eap_methods[0].method == EAP_TYPE_PEAP;

    force_tls12 = ssid->eap.eap_methods &&
                  ssid->eap.eap_methods[0].vendor == EAP_VENDOR_IETF &&
                  ssid->eap.eap_methods[0].method == EAP_TYPE_TTLS;

    allow_tls13 = is_peap ||
                  (!force_tls12 &&
                   (auth_mode == WPA_CTRL_AUTH_WPA3_ENT_192 ||
                    auth_mode == WPA_CTRL_AUTH_WPA3_ENTERPRISE ||
                    auth_mode == WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE));

    phase1_len = os_snprintf(phase1, sizeof(phase1),
                             "tls_disable_time_checks=1%s%s eswin_eap_tls_cred=%s",
                             auth_mode == WPA_CTRL_AUTH_WPA3_ENT_192 ?
                             " tls_suiteb=1" : "",
                             force_tls12 ?
                             " tls_disable_tlsv1_3=1 tls_disable_tlsv1_2=0" :
                             (allow_tls13 ? " tls_disable_tlsv1_3=0" : ""),
                             wifi_eap_tls_cred_profile_name(cred_profile));
    if (phase1_len < 0 || phase1_len >= (int)sizeof(phase1)) {
        return SYS_ERR_NO_MEM;
    }

    new_phase1 = os_strdup(phase1);
    if (!new_phase1) {
        return SYS_ERR_NO_MEM;
    }

    os_free(ssid->eap.phase1);
    ssid->eap.phase1 = new_phase1;
    wifi_eap_tls_cred_set_current_profile(cred_profile);

    if (auth_mode == WPA_CTRL_AUTH_WPA3_ENT_192) {
        ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X_SUITE_B_192;
        ssid->pairwise_cipher = WPA_CIPHER_GCMP_256;
        ssid->group_cipher = WPA_CIPHER_GCMP_256;
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_REQUIRED;
    } else if (auth_mode == WPA_CTRL_AUTH_WPA3_ENTERPRISE) {
        if (ie && (ie->key_mgmt & WPA_KEY_MGMT_IEEE8021X_SUITE_B)) {
            ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X_SUITE_B;
        } else {
            ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X_SHA256;
        }
        ssid->pairwise_cipher = wpas_core_pick_enterprise_cipher(
            ie ? ie->pairwise_cipher : 0, WPA_CIPHER_CCMP);
        ssid->group_cipher = wpas_core_pick_enterprise_group_cipher(
            ie ? ie->group_cipher : 0, WPA_CIPHER_CCMP);
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_REQUIRED;
    } else if (auth_mode == WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE) {
        ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X | WPA_KEY_MGMT_IEEE8021X_SHA256;
        ssid->pairwise_cipher = WPA_CIPHER_CCMP;
        ssid->group_cipher = wpas_core_pick_enterprise_group_cipher(
            ie ? ie->group_cipher : 0, WPA_CIPHER_CCMP);
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_REQUIRED;
    } else if (auth_mode == WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE) {
        ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X;
        ssid->pairwise_cipher = WPA_CIPHER_CCMP;
        ssid->group_cipher = wpas_core_pick_enterprise_group_cipher(
            ie ? ie->group_cipher : 0, WPA_CIPHER_CCMP);
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_OPTIONAL;
    } else {
        ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X | WPA_KEY_MGMT_IEEE8021X_SHA256;
        ssid->pairwise_cipher = WPA_CIPHER_CCMP;
        ssid->group_cipher = wpas_core_pick_enterprise_group_cipher(
            ie ? ie->group_cipher : 0, WPA_CIPHER_CCMP);
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_OPTIONAL;
    }
    ssid->proto = auth_mode == WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE ?
                  (WPA_PROTO_WPA | WPA_PROTO_RSN) : WPA_PROTO_RSN;

    return SYS_OK;
#else
    return SYS_ERR_NOT_SUPPORTED;
#endif
}

sys_err_t wpas_core_apply_enterprise_auth(struct wpa_ssid *ssid,
                                          wpa_auth_mode_e auth_mode)
{
    return wpas_core_apply_enterprise_auth_ie(ssid, auth_mode, NULL);
}
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

/*
 * Note: This file contains only core processing functions without vif_id conversion
 * All functions accept struct wpa_supplicant* parameter
 * High-level APIs (with vif_id conversion and eloop scheduling) are in wpas_cntrl_api.c
 */

/**
 * @brief Start WiFi scan (struct-based approach) - complete implementation
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config Scan configuration structure (optional, can be NULL)
 * @return sys_err_t Execution result
 *
 * Reference: ctrl_iface.c::wpas_ctrl_scan (lines 5835-5947)
 */
 
static int wpa_supplicant_ctrl_iface_max_scan_bss(struct wpa_supplicant *wpa_s, int max)
{
    wpa_s->conf->bss_max_count = max;
    return 0;
}

static void wpa_supplicant_ctrl_iface_bss_flush(struct wpa_supplicant *wpa_s, int flush_age)
{
    if (flush_age == 0)
        wpa_bss_flush(wpa_s);
    else
        wpa_bss_flush_by_age(wpa_s, flush_age);
}

/**
 * @brief Flush BSS scan results (exported wrapper)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param flush_age Age in seconds (0 = flush all)
 * @return 0 on success, -1 on invalid argument
 */
int wpas_core_scan_flush_bss(struct wpa_supplicant *wpa_s, int flush_age)
{
    if (!wpa_s) {
        return -1;
    }
    wpa_supplicant_ctrl_iface_bss_flush(wpa_s, flush_age);
    return 0;
}

void hang_back_scan(struct wpa_supplicant *wpa_s, wpa_manual_scan_cfg_t *scan_cfg);

static void hang_back_scan_eloop(void *eloop_ctx, void *timeout_ctx)
{
    struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)eloop_ctx;
    wpa_manual_scan_cfg_t *scan_cfg = (wpa_manual_scan_cfg_t *)timeout_ctx;

    if (!wpa_s) {
        return ;
    }

    wpa_supplicant_ctrl_iface_bss_flush(wpa_s, 0);
    /* 1. Check for pending scan tasks */
    if (radio_work_pending(wpa_s, "scan")) {
        os_printf(LL_WARN, "Pending scan scheduled, reject again.\n");
        return ;
    }

    /* 2. Free old scan configuration */
    if (wpa_s->manual_scan_cfg) {
        os_free(wpa_s->manual_scan_cfg);
        wpa_s->manual_scan_cfg = NULL;
    }

    /* 3. Check WiFi state and trigger scan */
    if (!wpa_s->scanning &&
        ((wpa_s->wpa_state <= WPA_SCANNING) ||
         (wpa_s->wpa_state == WPA_COMPLETED))) {

        wpa_s->manual_scan_cfg = scan_cfg;
        wpa_s->normal_scans = 0;
        wpa_s->scan_req = MANUAL_SCAN_REQ;
        wpa_supplicant_req_scan(wpa_s, 0, 0);

        return ;
    } else {
        /* Scan busy or state not allowed */
        os_printf(LL_DBG, "Ongoing scan action %d, delay again.\n", wpa_s->wpa_state);
        hang_back_scan(wpa_s, scan_cfg);
    }

}

void hang_back_scan(struct wpa_supplicant *wpa_s, wpa_manual_scan_cfg_t *scan_cfg)
{
    eloop_register_timeout(0, 100000, hang_back_scan_eloop, (void *)wpa_s, (void *)scan_cfg);
}

/* ============================================================================
 * Scan Operations
 * ============================================================================ */

sys_err_t wpas_core_scan_start(struct wpa_supplicant *wpa_s, const wpa_scan_config_t *config)
{
    wpa_manual_scan_cfg_t *scan_cfg = NULL;
    unsigned int i;

    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

    //wpa_supplicant_ctrl_iface_bss_flush(wpa_s, 0);
    /* 1. Check for pending scan tasks */
    if (radio_work_pending(wpa_s, "scan")) {
        os_printf(LL_WARN, "Pending scan scheduled, reject new request\n");
        return SYS_ERR_BUSY;
    }

    /* 2. Free old scan configuration */
    if (wpa_s->manual_scan_cfg) {
        os_free(wpa_s->manual_scan_cfg);
        wpa_s->manual_scan_cfg = NULL;
    }

    /* 3. Create new scan configuration */
    scan_cfg = os_zalloc(sizeof(wpa_manual_scan_cfg_t));
    if (!scan_cfg) {
        return SYS_ERR_NO_MEM;
    }

    /* 4. Set scan parameters */
    if (config) {
        /* Scan time */
        if (config->scan_time > 0) {
            scan_cfg->scan_time = config->scan_time;
        }

        /* Maximum scan results */
        if (config->max_item > 0) {
            wpa_supplicant_ctrl_iface_max_scan_bss(wpa_s, config->max_item);
        }

        /* Frequency list */
        if (config->freq_count > 0 && config->freq_list) {
            wpa_printf(MSG_INFO, "[SCAN_DEBUG] wpas_core: freq_count=%d, freq_list[0]=%d",
                      config->freq_count, config->freq_list[0]);
            for (i = 0; i < config->freq_count && i < CHANNEL_MAX_NUM; i++) {
                scan_cfg->freq[i] = config->freq_list[i];
                wpa_printf(MSG_INFO, "[SCAN_DEBUG] wpas_core: scan_cfg->freq[%d]=%d", i, scan_cfg->freq[i]);
            }
        } else {
            wpa_printf(MSG_INFO, "[SCAN_DEBUG] wpas_core: NO freq_list! freq_count=%d, freq_list=%p",
                      config ? config->freq_count : -1, config ? config->freq_list : NULL);
        }

        /* Passive scan */
        scan_cfg->passive = config->passive;

        /* Fast scan mode */
        scan_cfg->glance = 1;  /* Use fast scan by default */

        /* Specific SSID scan */
        if (config->ssid[0] != '\0') {
            size_t ssid_len = os_strlen((char *)config->ssid);
            if (ssid_len > 0 && ssid_len <= SSID_MAX_LEN) {
                memcpy(scan_cfg->ssid, config->ssid, ssid_len);
                scan_cfg->ssid_len = ssid_len;
            }
        }

        /* Specific BSSID scan */
        if (!IS_ZERO_MAC(config->bssid)) {
            memcpy(scan_cfg->bssid, config->bssid, 6);
            scan_cfg->bssid_set = 1;
        }
    } else {
        /* No configuration - use default fast scan */
        scan_cfg->glance = 1;
    }

    /* 5. Check WiFi state and trigger scan */
    if (!wpa_s->scanning &&
        ((wpa_s->wpa_state <= WPA_SCANNING) ||
         (wpa_s->wpa_state == WPA_COMPLETED))) {

        wpa_s->manual_scan_cfg = scan_cfg;
        wpa_s->normal_scans = 0;
        wpa_s->scan_req = MANUAL_SCAN_REQ;
        wpa_supplicant_req_scan(wpa_s, 0, 0);

        return SYS_OK;
    } else {
        /* Scan busy or state not allowed */
        os_printf(LL_WARN, "Ongoing scan action %d, delay request.\n", wpa_s->wpa_state);
        hang_back_scan(wpa_s, scan_cfg);
        return SYS_ERR_BUSY;
    }
}

/* BSP-only admission: execute in the WPA eloop, so the checks and start are
 * serialized with native scan state. The legacy delayed-scan API is unchanged. */
sys_err_t wpas_core_scan_start_checked(struct wpa_supplicant *wpa_s,
                                      const wpa_scan_config_t *config)
{
    if (!wpa_s || !config) {
        return SYS_ERR_INVALID_ARG;
    }
    if (radio_work_pending(wpa_s, "scan") || wpa_s->scanning ||
        wpa_supplicant_is_scan_scheduled(wpa_s) ||
        !((wpa_s->wpa_state <= WPA_SCANNING) ||
          (wpa_s->wpa_state == WPA_COMPLETED))) {
        return SYS_ERR_BUSY;
    }
    return wpas_core_scan_start(wpa_s, config);
}

sys_err_t wpas_core_scan_stop(struct wpa_supplicant *wpa_s)
{
    int pending_scan;
    int active_scan;
    int scheduled_scan;
    int manual_scheduled_scan;
    int manual_radio_scan;
    int notify_scan_done = 0;

    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

    pending_scan = wpa_s->radio && radio_work_pending(wpa_s, "scan");
    active_scan = wpa_s->scan_work != NULL;
    scheduled_scan = wpa_supplicant_is_scan_scheduled(wpa_s);
    manual_scheduled_scan = wpa_s->manual_scan_cfg &&
        scheduled_scan && !wpa_s->scan_work && !pending_scan &&
        (wpa_s->scan_req == MANUAL_SCAN_REQ);
    manual_radio_scan = wpa_s->manual_scan_cfg &&
        (wpa_s->scan_work || pending_scan) &&
        (wpa_s->last_scan_req == MANUAL_SCAN_REQ);

    if (!manual_scheduled_scan && !manual_radio_scan) {
        return SYS_OK;
    }

    if (manual_scheduled_scan) {
        wpa_dbg(wpa_s, MSG_DEBUG, "Cancel scheduled manual scan");
        wpa_supplicant_cancel_scan(wpa_s);
        wpa_s->scan_req = NORMAL_SCAN_REQ;
        wpa_s->last_scan_req = MANUAL_SCAN_REQ;
        notify_scan_done = 1;
    }

    if (manual_radio_scan && active_scan) {
        wpa_dbg(wpa_s, MSG_DEBUG, "Abort ongoing manual scan");
        if (wpa_drv_abort_scan(wpa_s) != 0) {
            wpa_dbg(wpa_s, MSG_DEBUG,
                    "abort_scan failed/unsupported, let scan complete naturally");
            return SYS_ERR;
        }
        wpa_s->own_scan_running = 0;
    } else if (manual_radio_scan && !wpa_s->scan_work) {
        notify_scan_done = 1;
    }

    if (manual_radio_scan && wpa_s->radio) {
        radio_remove_works(wpa_s, "scan", 0);
    }

    if (wpa_s->manual_scan_cfg) {
        os_free(wpa_s->manual_scan_cfg);
        wpa_s->manual_scan_cfg = NULL;
    }

    if (notify_scan_done) {
        wpas_notify_scan_done(wpa_s, 0);
    }

    return SYS_OK;
}

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

/**
 * @brief Configure network (STA/AP) - core processing function
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config Network configuration structure
 * @return sys_err_t Execution result
 *
 * Note: This function is called within handlers in wpas_cntrl_api.c
 *       Should NOT be called directly by system_wifi.c
 */
sys_err_t wpas_core_network_config(struct wpa_supplicant *wpa_s, const wpa_network_config_t *config)
{
    struct wpa_ssid *ssid;
    sys_err_t ret = SYS_OK;
    int enterprise_auto;
    int enterprise_fixed;

    if (!wpa_s || !config) {
        return SYS_ERR_INVALID_ARG;
    }

    if (config->ssid_len > sizeof(config->ssid) ||
        (config->candidate_frequency && (!config->frequency || config->mode != 0))) {
        return SYS_ERR_INVALID_ARG;
    }

    enterprise_auto = config->auth_mode == WPA_CTRL_AUTH_ENTERPRISE_AUTO;
    enterprise_fixed = config->auth_mode == WPA_CTRL_AUTH_WPA2_ENTERPRISE || config->auth_mode == WPA_CTRL_AUTH_WPA2_WPA3_ENTERPRISE ||
                       config->auth_mode == WPA_CTRL_AUTH_WPA3_ENT_192 || config->auth_mode == WPA_CTRL_AUTH_WPA3_ENTERPRISE ||
                       config->auth_mode == WPA_CTRL_AUTH_WPA_WPA2_ENTERPRISE;

#ifndef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    if (enterprise_auto || enterprise_fixed ||
        config->eap_method != WPA_CTRL_EAP_NONE) {
        return SYS_ERR_NOT_SUPPORTED;
    }
#endif

    /* 2. Add new network */
    ssid = wpa_supplicant_add_network(wpa_s);
    if (!ssid) {
        return SYS_ERR_NO_MEM;
    }
    wpa_dbg(wpa_s, MSG_DEBUG,"config->ssid_len=%d %s %d %s",config->ssid_len,config->ssid, config->password_len,config->password);
    /* 3. Set SSID - support BSSID-only connection when ssid_len == 0 */
    if (config->ssid_len > 0) {
        /* Normal case: SSID provided */
        ssid->ssid = (u8 *)dup_binstr((char *)config->ssid, config->ssid_len);
        ssid->ssid_len = config->ssid_len;
    } else {
        /* BSSID-only connection: no SSID provided, will be obtained from scan results */
        ssid->ssid = NULL;
        ssid->ssid_len = 0;
    }

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    if (config->eap_method != WPA_CTRL_EAP_NONE) {
        ret = wpas_core_prepare_eap_config(ssid, config);
        if (ret != SYS_OK) {
            goto fail;
        }
    }

    if (enterprise_auto) {
        ssid->key_mgmt = WPA_KEY_MGMT_IEEE8021X | WPA_KEY_MGMT_IEEE8021X_SHA256 |
                         WPA_KEY_MGMT_IEEE8021X_SUITE_B | WPA_KEY_MGMT_IEEE8021X_SUITE_B_192;
        ssid->proto = WPA_PROTO_RSN;
        ssid->pairwise_cipher = WPA_CIPHER_CCMP | WPA_CIPHER_GCMP_256;
        ssid->group_cipher = WPA_CIPHER_CCMP | WPA_CIPHER_GCMP_256;
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_OPTIONAL;
    } else if (enterprise_fixed) {
        ret = wpas_core_apply_enterprise_auth(ssid, config->auth_mode);
        if (ret != SYS_OK) {
            goto fail;
        }
    } else
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
    if(config->auth_mode == WPA_CTRL_AUTH_WEP && config->password_len > 0) {
        if (config->password_len == 5 || config->password_len == 13) {
            /* WEP */
            ssid->key_mgmt = WPA_KEY_MGMT_NONE;
            memcpy(ssid->wep_key[0], config->password, config->password_len);
            ssid->wep_key_len[0] = config->password_len;
        } else{
           wpa_dbg(wpa_s, MSG_ERROR,"WEP key length invalid %d",config->password_len);
           ret = SYS_ERR_INVALID_ARG;
           goto fail;
        }
    } else if (config->password_len > 0) {
        /* WPA/WPA2/WPA3 - Auto-adapt using DEFAULT_KEY_MGMT */
        ssid->key_mgmt = DEFAULT_KEY_MGMT;
        if (config->password_len == 64) {
            /* PMK hex */
            if (hexstr2bin((const char*)config->password, ssid->psk, 32) < 0) {
                ret = SYS_ERR_INVALID_ARG;
                goto fail;
            }
            ssid->psk_set = 1;
        } else {
            /* Plaintext password */
            ssid->passphrase = dup_binstr((char *)config->password, os_strlen((char *)config->password));
            if (!ssid->passphrase) {
                ret = SYS_ERR_NO_MEM;
                goto fail;
            }
        }
    } else {
        ssid->key_mgmt = WPA_KEY_MGMT_NONE;
    }

    if (!enterprise_auto && !enterprise_fixed) {
        /* 4. Set proto field - support WPA/WPA2/WPA3/WAPI auto-adaptation */
        switch (config->auth_mode) {
            case WPA_CTRL_AUTH_WPA_PSK:
                ssid->proto = WPA_PROTO_WPA;
                break;
            case WPA_CTRL_AUTH_WPA2_PSK:
                ssid->proto = WPA_PROTO_RSN;
                break;
            case WPA_CTRL_AUTH_WPA_WPA2_PSK:
                ssid->proto = WPA_PROTO_WPA | WPA_PROTO_RSN;
                break;
            case WPA_CTRL_AUTH_WPA3_SAE:
            case WPA_CTRL_AUTH_WPA2_WPA3_PSK:
                /* Support all protocols for auto-adaptation including WAPI */
                ssid->proto = DEFAULT_PROTO;
                break;
            default:
                /* OPEN/WEP - no proto needed */
                ssid->proto = 0;
                break;
        }
    }

    if (!enterprise_auto && !enterprise_fixed) {
        /* 5. Set cipher mode - support auto-adaptation CCMP/TKIP */
        switch (config->pairwise) {
        case WPA_CTRL_CIPHER_CCMP:
            ssid->pairwise_cipher = WPA_CIPHER_CCMP;
            ssid->group_cipher = WPA_CIPHER_CCMP;
            break;
        case WPA_CTRL_CIPHER_GCMP_256:
            ssid->pairwise_cipher = WPA_CIPHER_GCMP_256;
            ssid->group_cipher = WPA_CIPHER_GCMP_256;
            break;
        case WPA_CTRL_CIPHER_TKIP:
            ssid->pairwise_cipher = WPA_CIPHER_TKIP;
            ssid->group_cipher = WPA_CIPHER_TKIP;
            break;
        case WPA_CTRL_CIPHER_NONE:
            /* fall through */
        default:
            /*
             * Auto-adapt: keep WPA/WPA2 compatibility and, for the WPA2/WPA3
             * auto profile, allow GCMP-256 so the existing suite selection
             * logic can prefer SAE + GCMP-256 when the AP advertises it and
             * fall back to CCMP/TKIP otherwise.
             */
            if (config->auth_mode == WPA_CTRL_AUTH_WPA2_WPA3_PSK) {
                ssid->pairwise_cipher = DEFAULT_PAIRWISE | WPA_CIPHER_GCMP_256;
                ssid->group_cipher = DEFAULT_GROUP | WPA_CIPHER_GCMP_256;
            } else {
                ssid->pairwise_cipher = DEFAULT_PAIRWISE;
                ssid->group_cipher = DEFAULT_GROUP;
            }
        }
    }

    if (config->auth_mode == WPA_CTRL_AUTH_WPA3_SAE) {
        ssid->ieee80211w = MGMT_FRAME_PROTECTION_REQUIRED;
        ssid->key_mgmt = WPA_KEY_MGMT_SAE;
    }

    /* 6. Set BSSID (optional) */
    if (!IS_ZERO_MAC(config->bssid)) {
        memcpy(ssid->bssid, config->bssid, 6);
        ssid->bssid_set = 1;
    }

    /* 7. Set frequency */
    if (config->frequency > 0) {
        ssid->frequency = config->frequency;
    }

    ssid->candidate_frequency = config->candidate_frequency != config->frequency ?
                                config->candidate_frequency : 0;

    /* 8. Set scan_ssid - enable active scan for hidden SSID or BSSID-only connection */
    if (config->scan_ssid || (config->ssid_len == 0 && !IS_ZERO_MAC(config->bssid))) {
        /* Enable scan when:
         * - Explicitly requested (hidden SSID)
         * - BSSID-only connection (no SSID provided)
         */
        ssid->scan_ssid = 1;
    } else {
        ssid->scan_ssid = 0;
    }

    /* 9. AP mode settings */
    if (config->mode == 2) {
        ssid->mode = WPAS_MODE_AP;
        ssid->ignore_broadcast_ssid = config->hidden_ssid;

        if(config->beacon_int > 0) {
            g_beacon_int = config->beacon_int;
        }

        if(config->max_connect > 0) {
            /* Only modify user configuration value, not hardware capability
             * max_stations is hardware capability and should not be modified
             * Constrain max_num_sta to hardware limit */
            if (config->max_connect > wpa_s->max_stations) {
                wpa_printf(MSG_WARNING, "max_connect %d exceeds hardware limit %d, using %d\n",
                           config->max_connect, wpa_s->max_stations, wpa_s->max_stations);
                wpa_s->conf->max_num_sta = wpa_s->max_stations;
            } else {
                wpa_s->conf->max_num_sta = config->max_connect;
            }
        }

        /* AP client inactive timeout (0 = use default 300s) */
        if(config->ap_inactive_time > 0) {
            ssid->ap_max_inactivity = config->ap_inactive_time;
        }
    }

    wpa_dbg(wpa_s, MSG_DEBUG, "Configured network id %d ssid '%.*s' mode %s",
            ssid->id, (int)ssid->ssid_len, ssid->ssid,
            ssid->mode == WPAS_MODE_AP ? "AP" : "STA");
    wpa_dbg(wpa_s, MSG_DEBUG,
            " key_mgmt 0x%x proto 0x%x auth_mode %d pairwise_cipher 0x%x group_cipher 0x%x frequency %d",
            ssid->key_mgmt, ssid->proto, config->auth_mode,
            ssid->pairwise_cipher, ssid->group_cipher, ssid->frequency);
    /* 9. Activate network */
    wpa_supplicant_select_network(wpa_s, ssid);
#ifdef CONFIG_AP
    /* AP creation runs synchronously; do not retain a failed network. */
    if (ssid->mode == WPAS_MODE_AP && !wpa_s->ap_iface) {
        ret = SYS_ERR_WIFI_FAIL;
        goto fail;
    }
#endif /* CONFIG_AP */

    /*
     * Reset reconnection state for new connection attempt.
     *
     * This ensures that each new network configuration starts with a fresh
     * reconnection counter, preventing issues where previous failed attempts
     * would limit the new connection's reconnection attempts.
     *
     * Critical scenarios this fixes:
     * - User tries to connect to AP1, fails N times (reaches limit)
     * - User then tries to connect to AP2 (different SSID)
     * - Without this reset, AP2 connection would immediately fail
     */
    wpa_s->reconnect_attempt_count = 0;
    wpa_s->reconnect_limit_reached = 0;

    wpa_msg(wpa_s, MSG_DEBUG,
            "WPA: Reset reconnection state for new network configuration");

    return SYS_OK;

fail:
    wpa_supplicant_remove_network(wpa_s, ssid->id);
    return ret;
}

/* ============================================================================
 * SoftAP Management
 * ============================================================================ */

/**
 * @brief Configure Channel Switch Announcement (CSA) - core processing function
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param config CSA configuration parameters
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_softap_config_csa(struct wpa_supplicant *wpa_s, const wpa_csa_config_t *config)
{
    struct csa_settings settings;

    if (!wpa_s || !config) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(&settings, 0, sizeof(settings));
    settings.cs_count = config->csa_count;
    settings.block_tx = config->blocktx;
    settings.freq_params.freq = config->freq;

    if (ap_switch_channel(wpa_s, &settings)) {
        return SYS_ERR_NOT_SUPPORTED;
    }

    return SYS_OK;
}

/**
 * @brief Set SoftAP beacon vendor specific IE (runtime) - core processing function
 */
sys_err_t wpas_core_softap_set_vendor_elements(struct wpa_supplicant *wpa_s,
                                                const unsigned char *data,
                                                unsigned int len)
{
    struct hostapd_data *hapd;
    struct wpabuf *new_ie = NULL;

    if (!wpa_s || !wpa_s->ap_iface || !wpa_s->ap_iface->bss[0]) {
        return SYS_ERR_WIFI_AP_NOT_STARTED;
    }

    hapd = wpa_s->ap_iface->bss[0];
    if (!hapd->conf) {
        return SYS_ERR_INVALID_STATE;
    }

    if (data && len) {
        new_ie = wpabuf_alloc_copy(data, len);
        if (!new_ie) {
            return SYS_ERR_NO_MEM;
        }
    }

    wpabuf_free(hapd->conf->vendor_elements);
    hapd->conf->vendor_elements = new_ie;

    if (ieee802_11_set_beacon(hapd) < 0) {
        return SYS_ERR_WIFI_FAIL;
    }

    return SYS_OK;
}

/**
 * @brief Set SoftAP beacon interval (runtime) - core processing function
 */
sys_err_t wpas_core_softap_set_beacon_interval(struct wpa_supplicant *wpa_s,
                                                unsigned int interval)
{
    struct hostapd_data *hapd;

    if (!wpa_s || !wpa_s->ap_iface || !wpa_s->ap_iface->bss[0]) {
        return SYS_ERR_WIFI_AP_NOT_STARTED;
    }

    hapd = wpa_s->ap_iface->bss[0];
    if (!hapd->iconf) {
        return SYS_ERR_INVALID_STATE;
    }

    g_beacon_int = (uint16_t)interval;
    hapd->iconf->beacon_int = (u16)interval;

    if (ieee802_11_set_beacon(hapd) < 0) {
        return SYS_ERR_WIFI_FAIL;
    }

    return SYS_OK;
}

/**
 * @brief Remove all network configurations - core processing function
 * @param wpa_s Pointer to wpa_supplicant instance
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_network_remove(struct wpa_supplicant *wpa_s)
{
    int id;
    struct wpa_ssid *ssid;
    int stop_connection_scan = 0;

    if (!wpa_s)
        return -1;

    wpa_printf(MSG_DEBUG, "CTRL_IFACE: REMOVE_NETWORK all");

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    wpas_core_enterprise_pmksa_snapshot_save(wpa_s, wpa_s->current_ssid);

    #ifdef IEEE8021X_EAPOL
    if (wpa_s->eapol) {
        for (ssid = wpa_s->conf->ssid; ssid; ssid = ssid->next) {
            if (ssid->eap.eap_methods &&
                ssid->eap.eap_methods[0].vendor == EAP_VENDOR_IETF &&
                ssid->eap.eap_methods[0].method != EAP_TYPE_NONE) {
                wpa_dbg(wpa_s, MSG_DEBUG,
                        "Enterprise network removed - invalidate cached EAP session");
                eapol_sm_invalidate_cached_session(wpa_s->eapol);
                break;
            }
        }
    }
#endif /* IEEE8021X_EAPOL */
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

    if (wpa_s->current_ssid) {
#ifdef CONFIG_SME
        wpa_s->sme.prev_bssid_set = 0;
#endif /* CONFIG_SME */
        wpa_sm_set_config(wpa_s->wpa, NULL);
        if (wpa_s->wpa_state >= WPA_AUTHENTICATING)
            wpa_s->own_disconnect_req = 1;
        wpa_supplicant_deauthenticate(wpa_s, WLAN_REASON_DEAUTH_LEAVING);
    }

    /*
    * Stop scan quickly on STA stop so the next connect can start without
    * waiting for the current scan to finish. Use radio_remove_works() so the
    * scan callback runs with deinit=1 and clears scan state consistently.
    *
    * Do not touch user-triggered manual scans. Only stop scans used for
    * connection/reconnection flow.
    *
    * If the driver does not implement abort_scan (returns -1), let the
    * firmware scan complete naturally via EVENT_SCAN_RESULTS to avoid
    * desynchronizing firmware and software state.
    */
    stop_connection_scan = (wpa_s->last_scan_req != MANUAL_SCAN_REQ);

    if (wpa_s->radio && stop_connection_scan) {
        if (wpa_s->scan_work) {
            wpa_dbg(wpa_s, MSG_DEBUG, "Abort ongoing scan on network removal");
            if (wpa_drv_abort_scan(wpa_s) == 0) {
                wpa_s->own_scan_running = 0;
                radio_remove_works(wpa_s, "scan", 0);
            } else {
                wpa_dbg(wpa_s, MSG_DEBUG,
                        "abort_scan failed/unsupported, let scan complete naturally");
            }
        } else if (radio_work_pending(wpa_s, "scan")) {
            /* Pending only, no firmware scan running - safe to remove */
            radio_remove_works(wpa_s, "scan", 0);
        }
    } else if (wpa_s->radio &&
                (wpa_s->scan_work || radio_work_pending(wpa_s, "scan"))) {
        wpa_dbg(wpa_s, MSG_DEBUG,
                "Keep manual scan running on network removal");
    }

    ssid = wpa_s->conf->ssid;
    while (ssid) {
        id = ssid->id;
        ssid = ssid->next;
        wpa_config_remove_network(wpa_s->conf, id);
    }

    if (!wpa_supplicant_enabled_networks(wpa_s) &&
        (!wpa_s->radio ||
         (!wpa_s->scan_work && !radio_work_pending(wpa_s, "scan"))) &&
        wpa_s->wpa_state != WPA_INACTIVE &&
        wpa_s->wpa_state != WPA_INTERFACE_DISABLED) {
        wpa_supplicant_set_state(wpa_s, WPA_INACTIVE);
    }

    /*
     * Reset reconnection state when network is stopped by user.
     *
     * This ensures that when user explicitly stops the connection
     * (e.g., via sys_wifi_sta_stop()), the reconnection counter is
     * reset, providing a clean slate for the next connection attempt.
     *
     * Critical scenarios this fixes:
     * - User connects to AP, fails 3 times (counter=3)
     * - User calls stop(), then start() again
     * - Without this reset, user would only have (limit-3) attempts left
     *
     * This matches user expectations: stop() should completely terminate
     * and clean up connection state, not carry over failure counters.
     */
    wpa_s->reconnect_attempt_count = 0;
    wpa_s->reconnect_limit_reached = 0;

    wpa_msg(wpa_s, MSG_DEBUG,
            "WPA: Reset reconnection state on network removal");

    return 0;
}

sys_err_t wpas_core_network_disconnect(struct wpa_supplicant *wpa_s)
{
    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Save assoc frequency to network config for fast reconnect scan.
     * Must be done BEFORE wpas_request_disconnection() which clears
     * assoc_freq synchronously via driver deauth callback.
     * The saved frequency enables single-channel scan optimization
     * in wpa_supplicant_scan() when normal_scans < 2. */
    struct wpa_ssid *ssid = wpa_s->conf->ssid;
    if (ssid && !ssid->candidate_frequency &&
        wpa_s->assoc_freq > 0) {
        ssid->frequency = wpa_s->assoc_freq;
    }

    wpas_request_disconnection(wpa_s);
    wpa_s->reconnect_attempt_count = 0;
    wpa_s->reconnect_limit_reached = 0;

    wpa_msg(wpa_s, MSG_DEBUG,
            "WPA: Reset reconnection state on network disconnect");

    return SYS_OK;
}

static sys_err_t wpas_core_network_config_matches(struct wpa_ssid *ssid,
                                                  const wpa_network_config_t *config)
{
    if (!ssid || !config) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Compare SSID */
    if (ssid->ssid_len != config->ssid_len ||
        os_memcmp(ssid->ssid, config->ssid, config->ssid_len) != 0) {
        return SYS_ERR_WIFI_FAIL;
    }

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    if (config->eap_method != WPA_CTRL_EAP_NONE) {
        int eap_type = wpas_core_eap_type_from_method(config->eap_method);

        if (!ssid->eap.eap_methods ||
            ssid->eap.eap_methods[0].vendor != EAP_VENDOR_IETF ||
            ssid->eap.eap_methods[0].method != eap_type ||
            !ssid->eap.identity ||
            ssid->eap.identity_len != config->identity_len ||
            os_memcmp(ssid->eap.identity, config->identity,
                      config->identity_len) != 0) {
            return SYS_ERR_WIFI_FAIL;
        }

        if ((config->eap_method == WPA_CTRL_EAP_PEAP ||
             config->eap_method == WPA_CTRL_EAP_TTLS) &&
            (!ssid->eap.password ||
             ssid->eap.password_len != config->eap_password_len ||
             os_memcmp(ssid->eap.password, config->eap_password,
                       config->eap_password_len) != 0 ||
             !ssid->eap.phase2 ||
             !config->phase2 ||
             os_strcmp(ssid->eap.phase2, config->phase2) != 0)) {
            return SYS_ERR_WIFI_FAIL;
        }
    } else if (ssid->eap.eap_methods) {
        return SYS_ERR_WIFI_FAIL;
    }
#else
    if (config->eap_method != WPA_CTRL_EAP_NONE) {
        return SYS_ERR_NOT_SUPPORTED;
    }
    if (ssid->eap.eap_methods) {
        return SYS_ERR_WIFI_FAIL;
    }
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

    /* Compare password */
    if (config->password_len == 0) {
        /* OPEN: current network should also be OPEN */
        if (ssid->passphrase || ssid->psk_set ||
            ssid->wep_key_len[0] > 0) {
            return SYS_ERR_WIFI_FAIL;
        }
    } else if (config->password_len == 5 || config->password_len == 13) {
        /* WEP */
        if (ssid->wep_key_len[0] != config->password_len ||
            os_memcmp(ssid->wep_key[0], config->password,
                      config->password_len) != 0) {
            return SYS_ERR_WIFI_FAIL;
        }
    } else if (config->password_len == sizeof(config->password)) {
        u8 psk[sizeof(ssid->psk)];
        if (!ssid->psk_set ||
            hexstr2bin((const char *)config->password, psk, sizeof(psk)) < 0 ||
            os_memcmp(ssid->psk, psk, sizeof(psk)) != 0) {
            return SYS_ERR_WIFI_FAIL;
        }
    } else {
        /* WPA: compare passphrase */
        if (!ssid->passphrase ||
            os_strlen(ssid->passphrase) != config->password_len ||
            os_memcmp(ssid->passphrase, config->password,
                      config->password_len) != 0) {
            return SYS_ERR_WIFI_FAIL;
        }
    }
    /* Compare channel: only when user explicitly specifies one.
     * config->frequency == 0 means "don't care", skip comparison.
     * ssid->frequency may come from original config or assoc_freq
     * saved during disconnect. */
    if (config->frequency > 0 && ssid->frequency > 0 &&
        (int)config->frequency != ssid->frequency) {
        return SYS_ERR_WIFI_FAIL;
    }
    /* Compare BSSID: only when user explicitly specifies one.
     * If new config specifies a BSSID but old config didn't
     * (bssid_set==0), treat as mismatch — user's intent changed. */
    if (IS_ZERO_MAC(config->bssid) && ssid->bssid_set)
        return SYS_ERR_WIFI_FAIL;
    if (!IS_ZERO_MAC(config->bssid)) {
        if (!ssid->bssid_set ||
            os_memcmp(ssid->bssid, config->bssid, ETH_ALEN) != 0) {
            return SYS_ERR_WIFI_FAIL;
        }
    }

    return SYS_OK;
}

sys_err_t wpas_core_network_reconnect(struct wpa_supplicant *wpa_s,
                                       const wpa_network_config_t *config)
{
    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Get the currently configured network */
    struct wpa_ssid *ssid = wpa_s->conf->ssid;
    if (!ssid) {
        return SYS_ERR_WIFI_FAIL;  /* No network to reconnect */
    }

    /* If config provided, compare key fields */
    if (config) {
        sys_err_t ret = wpas_core_network_config_matches(ssid, config);
        if (ret != SYS_OK)
            return ret;
        if (config->candidate_frequency && !config->frequency)
            return SYS_ERR_INVALID_ARG;
        ssid->frequency = config->frequency;
        ssid->candidate_frequency = config->candidate_frequency != config->frequency ?
                                    config->candidate_frequency : 0;
    }

#ifdef IEEE8021X_EAPOL
    if (wpa_s->eapol) {
        eapol_sm_notify_portValid(wpa_s->eapol, false);
        eapol_sm_notify_portEnabled(wpa_s->eapol, false);
    }
#endif /* IEEE8021X_EAPOL */

    /* Fast reconnect */
    wpas_request_connection(wpa_s);

    wpa_s->reconnect_attempt_count = 0;
    wpa_s->reconnect_limit_reached = 0;
    wpa_msg(wpa_s, MSG_DEBUG, "WPA: Network reconnect initiated");

    return SYS_OK;
}

sys_err_t wpas_core_network_has_pmksa(struct wpa_supplicant *wpa_s,
                                       const wpa_network_config_t *config)
{
    struct wpa_ssid *ssid;
    const u8 *bssid = NULL;

    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

    ssid = wpa_s->conf->ssid;
    if (!ssid) {
        return SYS_ERR_WIFI_FAIL;
    }

    if (config) {
        sys_err_t ret = wpas_core_network_config_matches(ssid, config);
        if (ret != SYS_OK) {
            return ret;
        }

        if (!IS_ZERO_MAC(config->bssid)) {
            bssid = config->bssid;
        }
    }

    if (!bssid && ssid->bssid_set) {
        bssid = ssid->bssid;
    }
    if (!bssid && !IS_ZERO_MAC(wpa_s->bssid)) {
        bssid = wpa_s->bssid;
    }
    if (!bssid && !IS_ZERO_MAC(wpa_s->pending_bssid)) {
        bssid = wpa_s->pending_bssid;
    }

    if (wpa_sm_pmksa_cache_has_entry(wpa_s->wpa, bssid, ssid)) {
        return SYS_OK;
    }

    return SYS_ERR_WIFI_FAIL;
}

sys_err_t wpas_core_pmk_cache_clean(struct wpa_supplicant *wpa_s)
{
    if (!wpa_s) {
        return SYS_ERR_INVALID_ARG;
    }

#ifdef IEEE8021X_EAPOL
    if (wpa_s->eapol) {
        eapol_sm_invalidate_cached_session(wpa_s->eapol);
    }
#endif /* IEEE8021X_EAPOL */

#ifndef CONFIG_NO_WPA
    if (wpa_s->wpa) {
        wpa_sm_pmksa_cache_flush(wpa_s->wpa, NULL);
    }
#endif /* CONFIG_NO_WPA */

    return SYS_OK;
}

/* ============================================================================
 * Station Information
 * ============================================================================ */

/**
 * @brief Get WiFi connection information - core processing function
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param user_ctx Output buffer: wpa_wifi_info_t*
 * @return sys_err_t Execution result
 */
sys_err_t wpas_core_station_get_info(struct wpa_supplicant *wpa_s, void *user_ctx)
{
    wpa_wifi_info_t *info = (wpa_wifi_info_t *)user_ctx;
    struct wpa_ssid *ssid;
    struct wpa_bss  *bss;

    if (!info) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    bss = wpa_s->current_bss;
    if (wpa_s->wpa_state == WPA_COMPLETED) {
        ssid = wpa_s->current_ssid;
    } else {
        ssid = wpa_s->conf->ssid;
    }

    if (!bss || !ssid) {
        return SYS_ERR_INVALID_STATE;
    }

    /* Fill WPA WiFi information */
    memcpy(info->bssid, bss->bssid, 6);
    memcpy(info->ssid, bss->ssid, bss->ssid_len);
    info->ssid_len = bss->ssid_len;
    ieee80211_freq_to_chan(bss->freq, &info->channel);
#ifdef CONFIG_WIFI_PRODUCT_FHOST
    wpa_drv_get_connect_ap_rssi(wpa_s, (int8_t *)&(bss->level));
#endif
    info->rssi = bss->level;
    wpas_core_bss_get_mode(bss, &info->auth, &info->cipher);

    if (ssid->passphrase) {
        info->pwd_len = strlen(ssid->passphrase);
        memcpy(info->pwd, ssid->passphrase, info->pwd_len);
    } else if (ssid->psk_set) {
        char psk_hex[sizeof(ssid->psk) * 2 + 1];
        wpa_snprintf_hex(psk_hex, sizeof(psk_hex), ssid->psk, sizeof(ssid->psk));
        info->pwd_len = sizeof(ssid->psk) * 2;
        memcpy(info->pwd, psk_hex, info->pwd_len);
    } else if (ssid->wep_key_len[0] == 5 || ssid->wep_key_len[0] == 13) {
        info->pwd_len = ssid->wep_key_len[0];
        memcpy(info->pwd, &ssid->wep_key[0], info->pwd_len);
    }

    return SYS_OK;
}

/**
 * @brief Get effective SoftAP key management from running hostapd config
 * @param wpa_s WPA supplicant interface
 * @param ssid Current SSID configuration
 * @return Effective WPA_KEY_MGMT_* bitmask
 */
static int wpas_core_get_softap_key_mgmt(const struct wpa_supplicant *wpa_s,
                                         const struct wpa_ssid *ssid)
{
    if (wpa_s && wpa_s->ap_iface && wpa_s->ap_iface->bss &&
        wpa_s->ap_iface->bss[0] && wpa_s->ap_iface->bss[0]->conf) {
        return wpa_s->ap_iface->bss[0]->conf->wpa_key_mgmt;
    }

    if (ssid && (ssid->proto & (WPA_PROTO_WPA | WPA_PROTO_RSN)) &&
        (ssid->passphrase || ssid->psk_set)) {
        return WPA_KEY_MGMT_PSK;
    }

    return ssid ? ssid->key_mgmt : 0;
}

/**
 * @brief Convert SoftAP security settings to upper layer auth mode enum
 * @param ssid Current SSID configuration
 * @param key_mgmt Effective WPA key management bitmask
 * @param proto WPA protocol version bitmask
 * @return wpa_auth_mode_e Authentication mode enum value
 */
static wpa_auth_mode_e wpas_core_convert_auth_mode(const struct wpa_ssid *ssid,
                                                   int key_mgmt, int proto)
{
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    wpa_auth_mode_e enterprise_auth;
#endif

    if (!ssid) {
        return WPA_CTRL_AUTH_OPEN;
    }

    if (ssid->wep_key_len[0] || ssid->wep_key_len[1] ||
        ssid->wep_key_len[2] || ssid->wep_key_len[3]) {
        return WPA_CTRL_AUTH_WEP;
    }

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
    enterprise_auth = wpas_core_classify_enterprise_auth(key_mgmt, 0, proto);
    if (enterprise_auth != WPA_CTRL_AUTH_OPEN) {
        return enterprise_auth;
    }
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */

    /* WPA3 SAE detection */
    if (key_mgmt & WPA_KEY_MGMT_SAE) {
        if (key_mgmt & (WPA_KEY_MGMT_PSK | WPA_KEY_MGMT_PSK_SHA256)) {
            return WPA_CTRL_AUTH_WPA2_WPA3_PSK;  /* Mixed mode */
        } else {
            return WPA_CTRL_AUTH_WPA3_SAE;  /* Pure WPA3 */
        }
    }

    /* WPA/WPA2 mixed mode */
    if ((proto & WPA_PROTO_WPA) && (proto & WPA_PROTO_RSN)) {
        return WPA_CTRL_AUTH_WPA_WPA2_PSK;
    }

    /* Pure WPA2 */
    if (proto & WPA_PROTO_RSN) {
        return WPA_CTRL_AUTH_WPA2_PSK;
    }

    /* Pure WPA */
    if (proto & WPA_PROTO_WPA) {
        return WPA_CTRL_AUTH_WPA_PSK;
    }

    /* OPEN or WEP */
    /* Default return OPEN */
    return WPA_CTRL_AUTH_OPEN;
}

sys_err_t wpas_core_softap_get_config(struct wpa_supplicant *wpa_s, void *user_ctx)
{
    wpa_network_config_t *info = (wpa_network_config_t *)user_ctx;
    struct wpa_ssid *ssid;
    int key_mgmt;

    if (!wpa_s || !info) {
        return SYS_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    ssid = wpa_s->current_ssid;
    if (!ssid) {
        return SYS_ERR_INVALID_STATE;
    }

    /* Fill AP information */
    if (ssid->ssid[0] != 0) {
        memcpy(info->ssid, ssid->ssid, ssid->ssid_len);
        info->ssid_len = ssid->ssid_len;

        if (ssid->passphrase) {
            info->password_len = strlen(ssid->passphrase);
            memcpy(info->password, ssid->passphrase, info->password_len);
        } else if (ssid->psk_set) {
            char psk_hex[sizeof(ssid->psk) * 2 + 1];
            wpa_snprintf_hex(psk_hex, sizeof(psk_hex), ssid->psk, sizeof(ssid->psk));
            info->password_len = sizeof(ssid->psk) * 2;
            memcpy(info->password, psk_hex, info->password_len);
        }

        ieee80211_freq_to_chan(ssid->frequency, &info->channel);
    }

    info->hidden_ssid = ssid->ignore_broadcast_ssid;
    info->beacon_int = g_beacon_int;
    info->ap_inactive_time = ssid->ap_max_inactivity;
    if (wpa_s->conf && wpa_s->conf->max_num_sta > 0) {
        info->max_connect = wpa_s->conf->max_num_sta;
    } else {
        info->max_connect = wpa_s->max_stations;
    }

    /* Convert authentication mode */
    key_mgmt = wpas_core_get_softap_key_mgmt(wpa_s, ssid);
    info->auth_mode = wpas_core_convert_auth_mode(ssid, key_mgmt, ssid->proto);

    return SYS_OK;
}

/* -------------------------------------------------------------------------- */
/* Existing functions (adjusted to set priv->status using SYS_* codes)      */
/* -------------------------------------------------------------------------- */

sys_err_t wpas_core_bss_get_mode(struct wpa_bss *bss, uint8_t *auth, unsigned char *cipher)
{
    const u8 *ie, *ie2;
    struct wpa_ie_data data;

    if(!bss || !auth || !cipher) {
        return SYS_ERR_INVALID_ARG;
    }

    *auth = WPA_CTRL_AUTH_MAX;
    *cipher = WPA_CTRL_CIPHER_NONE;
#ifdef CONFIG_WIFI_WAPI
    ie = wpa_bss_get_ie(bss, WLAN_EID_WAPI);
    if (ie) {
        if (wpa_parse_wapi_ie(ie, ie[1] + 2, &data) < 0)
            return SYS_ERR_INVALID_RESPONSE;
        *auth = (data.key_mgmt & WPA_KEY_MGMT_WAPI_PSK) ?
            WPA_CTRL_AUTH_WAPI_PSK : WPA_CTRL_AUTH_WAPI_CERT;
        *cipher = WPA_CTRL_CIPHER_SMS4;
        return SYS_OK;
    }
#endif

    ie  = wpa_bss_get_vendor_ie(bss, WPA_IE_VENDOR_TYPE);
    ie2 = wpa_bss_get_ie(bss, WLAN_EID_RSN);

    if(ie || ie2) {
        if(ie && !ie2) {
            if (wpa_parse_wpa_ie(ie, ie[1]+2, &data) < 0) {
                return SYS_ERR_INVALID_RESPONSE;
            }

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256) {
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            } else if(data.pairwise_cipher & WPA_CIPHER_CCMP) {
                *cipher = WPA_CTRL_CIPHER_CCMP;
            } else {
                *cipher = WPA_CTRL_CIPHER_TKIP;
            }

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
        } else if(!ie && ie2) {
            if (wpa_parse_wpa_ie(ie2, ie2[1]+2, &data) < 0) {
                return SYS_ERR_INVALID_RESPONSE;
            }

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256) {
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            } else if(data.pairwise_cipher & WPA_CIPHER_CCMP) {
                *cipher = WPA_CTRL_CIPHER_CCMP;
            } else {
                *cipher = WPA_CTRL_CIPHER_TKIP;
            }

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            *auth = wpas_core_classify_enterprise_auth(data.key_mgmt,
                                                       data.capabilities,
                                                       WPA_PROTO_RSN);
            if (*auth != WPA_CTRL_AUTH_OPEN) {
                /* Enterprise mode already classified from AKM and MFPR. */
            } else
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
            if (data.key_mgmt & WPA_KEY_MGMT_SAE
            && ((data.key_mgmt & WPA_KEY_MGMT_PSK) || (data.key_mgmt & WPA_KEY_MGMT_PSK_SHA256))) {
                *auth = WPA_CTRL_AUTH_WPA2_WPA3_PSK;
            } else if (data.key_mgmt & WPA_KEY_MGMT_SAE) {
                *auth = WPA_CTRL_AUTH_WPA3_SAE;
            } else {
                *auth = WPA_CTRL_AUTH_WPA2_PSK;
            }
        } else {
#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            int proto = WPA_PROTO_RSN;
            struct wpa_ie_data wpa_data;
#endif

            if (wpa_parse_wpa_ie(ie2, ie2[1]+2, &data) < 0) {
                return SYS_ERR_INVALID_RESPONSE;
            }

            if (data.pairwise_cipher & WPA_CIPHER_GCMP_256) {
                *cipher = WPA_CTRL_CIPHER_GCMP_256;
            } else if(data.pairwise_cipher & WPA_CIPHER_CCMP) {
                *cipher = WPA_CTRL_CIPHER_CCMP;
            } else {
                *cipher = WPA_CTRL_CIPHER_TKIP;
            }

#ifdef CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT
            if (wpa_parse_wpa_ie(ie, ie[1]+2, &wpa_data) == 0 &&
                wpas_core_is_enterprise_key_mgmt(wpa_data.key_mgmt)) {
                proto |= WPA_PROTO_WPA;
            }

            *auth = wpas_core_classify_enterprise_auth(data.key_mgmt,
                                                       data.capabilities,
                                                       proto);
            if (*auth != WPA_CTRL_AUTH_OPEN) {
                /* Enterprise mode already classified from AKM and MFPR. */
            } else
#endif /* CONFIG_WIRELESS_WPA_ENTERPRISE_SUPPORT */
            {
                *auth = WPA_CTRL_AUTH_WPA_WPA2_PSK;
            }
        }
    } else if(bss->caps & IEEE80211_CAP_PRIVACY) {
        *auth   = WPA_CTRL_AUTH_WEP;
        *cipher = WPA_CTRL_CIPHER_WEP40;
    } else {
        *auth   = WPA_CTRL_AUTH_OPEN;
        *cipher = WPA_CTRL_CIPHER_NONE;
    }

    return SYS_OK;
}

sys_err_t wpas_core_bss_get_extra_info(struct wpa_bss *bss, uint8_t *group_cipher,
                                       uint8_t *wifi_protocol, uint8_t *wps_flag)
{
    const u8 *ie, *ie2;
    struct wpa_ie_data data;
    const u8 *ies;
    size_t ies_len;

    if (!bss || !group_cipher || !wifi_protocol || !wps_flag)
        return SYS_ERR_INVALID_ARG;

    *group_cipher = WPA_CTRL_CIPHER_NONE;
    *wifi_protocol = 0;
    *wps_flag = 0;

    ies = (const u8 *)(bss + 1);
    ies_len = bss->ie_len;

    /* --- group_cipher --- */
    ie = wpa_bss_get_vendor_ie(bss, WPA_IE_VENDOR_TYPE);
    ie2 = wpa_bss_get_ie(bss, WLAN_EID_RSN);

#ifdef CONFIG_WIFI_WAPI
    const u8 *wapi_ie = wpa_bss_get_ie(bss, WLAN_EID_WAPI);
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
    } else if (bss->caps & IEEE80211_CAP_PRIVACY) {
        *group_cipher = WPA_CTRL_CIPHER_WEP40;
    }

    /* --- wifi_protocol --- */
    if (bss->freq > 0 && bss->freq < 4000) {
        *wifi_protocol |= BIT(0) | BIT(1);  /* 802.11b + 802.11g */
    } else if (bss->freq >= 5000) {
        *wifi_protocol |= BIT(5);  /* 802.11a */
    }
    if (wpa_bss_get_ie(bss, WLAN_EID_HT_CAP))
        *wifi_protocol |= BIT(2);   /* 802.11n */
    if (wpa_bss_get_ie(bss, WLAN_EID_VHT_CAP))
        *wifi_protocol |= BIT(6);   /* 802.11ac */
    /* 802.11ax: scan for extension element with HE Capabilities */
    if (ies && ies_len > 0) {
        const u8 *pos = ies;
        const u8 *end = pos + ies_len;
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
    }

    /* --- wps --- */
    if (wpa_bss_get_vendor_ie(bss, WPS_IE_VENDOR_TYPE))
        *wps_flag = 1;

    return SYS_OK;
}

/* ============================================================================
 * Interface Control
 * ============================================================================ */

int wpas_core_iface_dump_status(struct wpa_supplicant *wpa_s,
    const char *params,
    char *buf, size_t buflen)
    {
    //char *pos, *end, tmp[30];
    char *pos, *end;
    //int res, verbose, wps, ret;
    int verbose, ret;
#ifdef CONFIG_HS20
    int res;
    const u8 *hs20;
#endif /* CONFIG_HS20 */
    //const u8 *sess_id;
    //size_t sess_id_len;

#if 0
    if (os_strcmp(params, "-DRIVER") == 0)
    return wpa_drv_status(wpa_s, buf, buflen);
    verbose = os_strcmp(params, "-VERBOSE") == 0;
    wps = os_strcmp(params, "-WPS") == 0;
#else
    verbose = 0;
    //wps = 0;
#endif
    pos = buf;
    end = buf + buflen;
    if (wpa_s->wpa_state >= WPA_ASSOCIATED) {
        struct wpa_ssid *ssid = wpa_s->current_ssid;
        ret = os_snprintf(pos, end - pos, "bssid=" MACSTR "\n",MAC2STR(wpa_s->bssid));
        if (os_snprintf_error(end - pos, ret))
            return pos - buf;
        pos += ret;
        ret = os_snprintf(pos, end - pos, "freq=%u\n",
        wpa_s->assoc_freq);
        if (os_snprintf_error(end - pos, ret))
            return pos - buf;
        pos += ret;
        if (ssid) {
            u8 *_ssid = ssid->ssid;
            size_t ssid_len = ssid->ssid_len;
            u8 ssid_buf[SSID_MAX_LEN];
            if (ssid_len == 0) {
                int _res = wpa_drv_get_ssid(wpa_s, ssid_buf);
                if (_res < 0)
                    ssid_len = 0;
                else
                    ssid_len = _res;
                _ssid = ssid_buf;
            }
            ret = os_snprintf(pos, end - pos, "ssid=%s\nid=%d\n",wpa_ssid_txt(_ssid, ssid_len),ssid->id);
            if (os_snprintf_error(end - pos, ret))
                return pos - buf;
            pos += ret;

            switch (ssid->mode) {
            case WPAS_MODE_INFRA:
            ret = os_snprintf(pos, end - pos,
            "mode=station\n");
            break;
            /*case WPAS_MODE_IBSS:
            ret = os_snprintf(pos, end - pos,
            "mode=IBSS\n");
            break;*/
            case WPAS_MODE_AP:
            ret = os_snprintf(pos, end - pos,
            "mode=AP\n");
            break;
            /*case WPAS_MODE_P2P_GO:
            ret = os_snprintf(pos, end - pos,
            "mode=P2P GO\n");
            break;
            case WPAS_MODE_P2P_GROUP_FORMATION:
            ret = os_snprintf(pos, end - pos,
            "mode=P2P GO - group "
            "formation\n");
            break;
            case WPAS_MODE_MESH:
            ret = os_snprintf(pos, end - pos,
            "mode=mesh\n");
            break;*/
            default:
            ret = 0;
            break;
            }
            if (os_snprintf_error(end - pos, ret))
            return pos - buf;
            pos += ret;
        }

#ifdef CONFIG_AP
        if (wpa_s->ap_iface) {
        pos += ap_ctrl_iface_wpa_get_status(wpa_s, pos,
        end - pos,
        verbose);
        } else
#endif /* CONFIG_AP */
        pos += wpa_sm_get_status(wpa_s->wpa, pos, end - pos, verbose);
        }
#ifdef CONFIG_SAE
    if (wpa_s->wpa_state >= WPA_ASSOCIATED &&
#ifdef CONFIG_AP
    !wpa_s->ap_iface &&
#endif /* CONFIG_AP */
    wpa_s->sme.sae.state == SAE_ACCEPTED) {
        ret = os_snprintf(pos, end - pos, "sae_group=%d\n",
        wpa_s->sme.sae.group);
        if (os_snprintf_error(end - pos, ret))
        return pos - buf;
        pos += ret;
    }
#endif /* CONFIG_SAE */
    ret = os_snprintf(pos, end - pos, "wpa_state=%s\n",
    wpa_supplicant_state_txt(wpa_s->wpa_state));
    if (os_snprintf_error(end - pos, ret))
        return pos - buf;
    pos += ret;

#ifdef CONFIG_P2P
    if (wpa_s->global->p2p) {
    ret = os_snprintf(pos, end - pos, "p2p_device_address=" MACSTR
    "\n", MAC2STR(wpa_s->global->p2p_dev_addr));
    if (os_snprintf_error(end - pos, ret))
    return pos - buf;
    pos += ret;
    }
#endif /* CONFIG_P2P */

    ret = os_snprintf(pos, end - pos, "address=" MACSTR "\n",
    MAC2STR(wpa_s->own_addr));
    if (os_snprintf_error(end - pos, ret))
    return pos - buf;
    pos += ret;

#ifdef CONFIG_WPS
    {
    char uuid_str[100];
    uuid_bin2str(wpa_s->wps->uuid, uuid_str, sizeof(uuid_str));
    ret = os_snprintf(pos, end - pos, "uuid=%s\n", uuid_str);
    if (os_snprintf_error(end - pos, ret))
    return pos - buf;
    pos += ret;
    }
#endif /* CONFIG_WPS */

    return pos - buf;
}

sys_err_t wpas_core_iface_update_mac(struct wpa_supplicant *wpa_s, const char *mac)
{
    wpa_update_mac_addr(wpa_s, (unsigned char *)mac);
    return SYS_OK;
}

#ifdef CONFIG_AP
/**
 * @brief Dump AP mode connected STA list (simplified version without IP)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param buf Output buffer
 * @param buflen Buffer size
 * @return int Number of bytes written
 *
 * Based on hostapd_ctrl_iface_sta() but simplified to exclude IP address
 */
static int dump_ap_sta_list(struct wpa_supplicant *wpa_s, char *buf, size_t buflen)
{
    struct hostapd_data *hapd;
    struct sta_info *sta;
    char *pos, *end;
    int ret;
    struct os_reltime now;

    if (!wpa_s || !wpa_s->ap_iface) {
        return 0;
    }

    hapd = wpa_s->ap_iface->bss[0];
    if (!hapd) {
        return 0;
    }

    pos = buf;
    end = buf + buflen;

    /* Print header */
    ret = os_snprintf(pos, end - pos, "AID / MAC / ListenInterval / ConnectedTime\n");
    if (os_snprintf_error(end - pos, ret))
        return pos - buf;
    pos += ret;

    os_get_reltime(&now);

    /* Iterate through all connected STAs */
    for (sta = hapd->sta_list; sta != NULL; sta = sta->next) {
        ret = os_snprintf(pos, end - pos, "%d\t" MACSTR "\t%d\t%ds\n",
                          sta->aid,
                          MAC2STR(sta->addr),
                          sta->listen_interval,
                          (int)(now.sec - sta->connected_time.sec));
        if (os_snprintf_error(end - pos, ret))
            return pos - buf;
        pos += ret;
    }

    return (pos - buf);
}
#endif /* CONFIG_AP */

/**
 * @brief Get configured network list (for debugging)
 * @param wpa_s Pointer to wpa_supplicant instance
 * @param buf Output buffer for network list
 * @param buflen Size of output buffer
 * @return int Number of bytes written to buffer
 *
 * Reference: ctrl_iface.c::wpa_supplicant_ctrl_iface_list_networks (lines 469-538)
 * Output format: network id / ssid / bssid / flags
 */
int wpas_core_iface_dump_list_networks(struct wpa_supplicant *wpa_s,
                                       char *buf, size_t buflen)
{
    char *pos, *end, *prev;
    struct wpa_ssid *ssid;
    int ret;

    if (!wpa_s || !buf) {
        return 0;
    }

#ifdef CONFIG_AP
    /* In AP mode, output STA list instead of network list */
    if (wpa_s->ap_iface) {
        return dump_ap_sta_list(wpa_s, buf, buflen);
    }
#endif

    pos = buf;
    end = buf + buflen;

    /* Print table header */
    ret = os_snprintf(pos, end - pos,
                      "network id / ssid / bssid / flags\n");
    if (os_snprintf_error(end - pos, ret))
        return pos - buf;
    pos += ret;

    ssid = wpa_s->conf->ssid;

    /* Iterate through all configured networks */
    while (ssid) {
        prev = pos;

        /* Print network_id and ssid */
        ret = os_snprintf(pos, end - pos, "%d\t%s",
                          ssid->id,
                          wpa_ssid_txt(ssid->ssid, ssid->ssid_len));
        if (os_snprintf_error(end - pos, ret))
            return prev - buf;
        pos += ret;

        /* Print bssid */
        if (ssid->bssid_set) {
            ret = os_snprintf(pos, end - pos, "\t" MACSTR,
                              MAC2STR(ssid->bssid));
        } else {
            ret = os_snprintf(pos, end - pos, "\tany");
        }
        if (os_snprintf_error(end - pos, ret))
            return prev - buf;
        pos += ret;

        /* Print flags */
        ret = os_snprintf(pos, end - pos, "\t%s%s",
                          ssid == wpa_s->current_ssid ? "[CURRENT]" : "",
                          ssid->disabled ? "[DISABLED]" : "");
        if (os_snprintf_error(end - pos, ret))
            return prev - buf;
        pos += ret;

        /* Newline */
        ret = os_snprintf(pos, end - pos, "\n");
        if (os_snprintf_error(end - pos, ret))
            return prev - buf;
        pos += ret;

        ssid = ssid->next;
    }

    return pos - buf;
}

#ifdef CONFIG_AP
/**
 * @brief Deauthenticate a station from AP
 *
 * This is a simplified implementation of hostapd_ctrl_iface_deauthenticate
 * for wpas_struct_iface layer. It provides core deauth functionality without
 * test mode and P2P manager features.
 *
 * @param hapd Pointer to hostapd_data
 * @param txtaddr Station MAC address in text format (e.g., "aa:bb:cc:dd:ee:ff")
 *                Optional parameters:
 *                - " reason=N" - Set deauth reason code (default: 2)
 *                - " tx=0" - Remove STA without sending deauth frame
 * @return 0 on success, -1 on failure
 *
 * @note Broadcast address (ff:ff:ff:ff:ff:ff) disconnects all stations
 */
int hostapd_ctrl_iface_deauthenticate(struct hostapd_data *hapd,
                                       const char *txtaddr)
{
    u8 addr[ETH_ALEN];
    struct sta_info *sta;
    const char *pos;
    u16 reason = WLAN_REASON_PREV_AUTH_NOT_VALID;

    if (!hapd || !txtaddr) {
        wpa_printf(MSG_ERROR, "hostapd_ctrl_iface_deauthenticate: Invalid parameters");
        return -1;
    }

    wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "CTRL_IFACE DEAUTHENTICATE %s", txtaddr);

    /* Parse MAC address from text format */
    if (hwaddr_aton(txtaddr, addr)) {
        wpa_printf(MSG_ERROR, "hostapd_ctrl_iface_deauthenticate: Invalid MAC address");
        return -1;
    }

    /* Parse optional reason code parameter */
    pos = os_strstr(txtaddr, " reason=");
    if (pos) {
        reason = atoi(pos + 8);
        wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Deauth reason code: %u", reason);
    }

    /* Check for tx=0 option (remove STA without sending deauth frame) */
    if (os_strstr(txtaddr, " tx=0")) {
        wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Removing STA without deauth frame");
        hostapd_drv_sta_remove(hapd, addr);
    } else {
        /* Send deauth frame via driver */
        hostapd_drv_sta_deauth(hapd, addr, reason);
    }

    /* Handle station state cleanup */
    sta = ap_get_sta(hapd, addr);
    if (sta) {
        /* Deauthenticate specific station */
        ap_sta_deauthenticate(hapd, sta, reason);
        wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Station " MACSTR " deauthenticated",
                MAC2STR(addr));
    } else if (addr[0] == 0xff) {
        /* Broadcast address - disconnect all stations */
        wpa_dbg(hapd->msg_ctx, MSG_DEBUG, "Disconnecting all stations");
        hostapd_free_stas(hapd);
    } else {
        wpa_dbg(hapd->msg_ctx, MSG_DEBUG,
                "Station " MACSTR " not found in AP", MAC2STR(addr));
    }

    return 0;
}
#endif /* CONFIG_AP */
