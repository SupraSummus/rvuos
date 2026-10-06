/*
 * SPDX-FileCopyrightText: Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RVUOS_WIFI_ESP_H
#define RVUOS_WIFI_ESP_H

/*
 * What the driver and Espressif's Wi-Fi libraries agree on, as ESP-IDF 4d59230d declares it
 * in esp_wifi.h, esp_private/wifi_os_adapter.h and esp_wifi_types_generic.h (Apache-2.0);
 * only what the driver uses, with the layouts the libraries were built against.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int32_t esp_err_t;
#define ESP_OK   0
#define ESP_FAIL -1
#define ESP_ERR_WIFI_DISCARD 0x301b /* ESP_ERR_WIFI_BASE + 27: a frame dropped, which the libraries ignore */

/*
 * The OS functions the libraries call, in the order they expect them,
 * with the fields ESP-IDF declares for the ESP32-C6 alone: not phy_common_clock_enable and _disable,
 * which only the ESP32 and ESP32-S2 have, nor coex_configure_preemption_end_cb, the ESP32-S31's.
 */
#define OSI_VERSION 0x00000009
#define OSI_MAGIC   0xDEADBEAF
struct osi_funcs {
    int32_t version;
    bool (*env_is_chip)(void);
    void (*set_intr)(int32_t cpu, uint32_t source, uint32_t intr, int32_t prio);
    void (*clear_intr)(uint32_t source, uint32_t intr);
    void (*set_isr)(int32_t intr, void *f, void *arg);
    void (*ints_on)(uint32_t mask);
    void (*ints_off)(uint32_t mask);
    bool (*is_from_isr)(void);
    void *(*spin_lock_create)(void);
    void (*spin_lock_delete)(void *lock);
    uint32_t (*wifi_int_disable)(void *mux);
    void (*wifi_int_restore)(void *mux, uint32_t tmp);
    void (*task_yield_from_isr)(void);
    void *(*semphr_create)(uint32_t max, uint32_t init);
    void (*semphr_delete)(void *semphr);
    int32_t (*semphr_take)(void *semphr, uint32_t ticks);
    int32_t (*semphr_give)(void *semphr);
    void *(*wifi_thread_semphr_get)(void);
    void *(*mutex_create)(void);
    void *(*recursive_mutex_create)(void);
    void (*mutex_delete)(void *mutex);
    int32_t (*mutex_lock)(void *mutex);
    int32_t (*mutex_unlock)(void *mutex);
    void *(*queue_create)(uint32_t len, uint32_t item_size);
    void (*queue_delete)(void *queue);
    int32_t (*queue_send)(void *queue, void *item, uint32_t ticks);
    int32_t (*queue_send_from_isr)(void *queue, void *item, void *hptw);
    int32_t (*queue_send_to_back)(void *queue, void *item, uint32_t ticks);
    int32_t (*queue_send_to_front)(void *queue, void *item, uint32_t ticks);
    int32_t (*queue_recv)(void *queue, void *item, uint32_t ticks);
    uint32_t (*queue_msg_waiting)(void *queue);
    void *(*event_group_create)(void);
    void (*event_group_delete)(void *event);
    uint32_t (*event_group_set_bits)(void *event, uint32_t bits);
    uint32_t (*event_group_clear_bits)(void *event, uint32_t bits);
    uint32_t (*event_group_wait_bits)(void *event, uint32_t bits, int clear, int all, uint32_t ticks);
    int32_t (*task_create_pinned_to_core)(void *f, const char *name, uint32_t stack, void *arg, uint32_t prio,
                                          void *handle, uint32_t core);
    int32_t (*task_create)(void *f, const char *name, uint32_t stack, void *arg, uint32_t prio, void *handle);
    void (*task_delete)(void *handle);
    void (*task_delay)(uint32_t ticks);
    int32_t (*task_ms_to_tick)(uint32_t ms);
    void *(*task_get_current_task)(void);
    int32_t (*task_get_max_priority)(void);
    void *(*malloc)(size_t size);
    void (*free)(void *p);
    int32_t (*event_post)(const char *base, int32_t id, void *data, size_t size, uint32_t ticks);
    uint32_t (*get_free_heap_size)(void);
    uint32_t (*rand)(void);
    void (*dport_access_stall_other_cpu_start_wrap)(void);
    void (*dport_access_stall_other_cpu_end_wrap)(void);
    void (*wifi_pm_sleep_lock_acquire)(void);
    void (*wifi_pm_sleep_lock_release)(void);
    void (*phy_disable)(void);
    void (*phy_enable)(void);
    int (*phy_update_country_info)(const char *country);
    int (*read_mac)(uint8_t *mac, unsigned int type);
    void (*timer_arm)(void *timer, uint32_t ms, bool repeat);
    void (*timer_disarm)(void *timer);
    void (*timer_done)(void *timer);
    void (*timer_setfn)(void *timer, void *f, void *arg);
    void (*timer_arm_us)(void *timer, uint32_t us, bool repeat);
    void (*wifi_reset_mac)(void);
    void (*wifi_clock_enable)(void);
    void (*wifi_clock_disable)(void);
    void (*wifi_rtc_enable_iso)(void);
    void (*wifi_rtc_disable_iso)(void);
    int64_t (*esp_timer_get_time)(void);
    int (*nvs_set_i8)(uint32_t handle, const char *key, int8_t value);
    int (*nvs_get_i8)(uint32_t handle, const char *key, int8_t *value);
    int (*nvs_set_u8)(uint32_t handle, const char *key, uint8_t value);
    int (*nvs_get_u8)(uint32_t handle, const char *key, uint8_t *value);
    int (*nvs_set_u16)(uint32_t handle, const char *key, uint16_t value);
    int (*nvs_get_u16)(uint32_t handle, const char *key, uint16_t *value);
    int (*nvs_open)(const char *name, unsigned int mode, uint32_t *handle);
    void (*nvs_close)(uint32_t handle);
    int (*nvs_commit)(uint32_t handle);
    int (*nvs_set_blob)(uint32_t handle, const char *key, const void *value, size_t len);
    int (*nvs_get_blob)(uint32_t handle, const char *key, void *value, size_t *len);
    int (*nvs_erase_key)(uint32_t handle, const char *key);
    int (*get_random)(uint8_t *buf, size_t len);
    int (*get_time)(void *t);
    unsigned long (*random)(void);
    uint32_t (*slowclk_cal_get)(void);
    void (*log_write)(unsigned int level, const char *tag, const char *fmt, ...);
    void (*log_writev)(unsigned int level, const char *tag, const char *fmt, va_list args);
    uint32_t (*log_timestamp)(void);
    void *(*malloc_internal)(size_t size);
    void *(*realloc_internal)(void *p, size_t size);
    void *(*calloc_internal)(size_t n, size_t size);
    void *(*zalloc_internal)(size_t size);
    void *(*wifi_malloc)(size_t size);
    void *(*wifi_realloc)(void *p, size_t size);
    void *(*wifi_calloc)(size_t n, size_t size);
    void *(*wifi_zalloc)(size_t size);
    void *(*wifi_create_queue)(int len, int item_size);
    void (*wifi_delete_queue)(void *queue);
    int (*coex_init)(void);
    void (*coex_deinit)(void);
    int (*coex_enable)(void);
    void (*coex_disable)(void);
    uint32_t (*coex_status_get)(void);
    void (*coex_condition_set)(uint32_t type, bool dissatisfy);
    int (*coex_wifi_request)(uint32_t event, uint32_t latency, uint32_t duration);
    int (*coex_wifi_release)(uint32_t event);
    int (*coex_wifi_channel_set)(uint8_t primary, uint8_t secondary);
    int (*coex_event_duration_get)(uint32_t event, uint32_t *duration);
    int (*coex_pti_get)(uint32_t event, uint8_t *pti);
    void (*coex_schm_status_bit_clear)(uint32_t type, uint32_t status);
    void (*coex_schm_status_bit_set)(uint32_t type, uint32_t status);
    int (*coex_schm_interval_set)(uint32_t interval);
    uint32_t (*coex_schm_interval_get)(void);
    uint8_t (*coex_schm_curr_period_get)(void);
    void *(*coex_schm_curr_phase_get)(void);
    int (*coex_schm_process_restart)(void);
    int (*coex_schm_register_cb)(int type, int (*cb)(int));
    int (*coex_register_start_cb)(int (*cb)(void));
    void (*regdma_link_set_write_wait_content)(void *link, uint32_t value, uint32_t mask);
    void *(*sleep_retention_find_link_by_id)(int id);
    int (*coex_schm_flexible_period_set)(uint8_t period);
    uint8_t (*coex_schm_flexible_period_get)(void);
    void *(*coex_schm_get_phase_by_idx)(int idx);
    bool (*wifi_disable_ac_ax)(void);
    int32_t (*wifi_bb_sleep_retention_attach)(void);
    int32_t (*wifi_bb_sleep_retention_detach)(void);
    int32_t (*wifi_mac_sleep_retention_attach)(void);
    int32_t (*wifi_mac_sleep_retention_detach)(void);
    int32_t magic;
};

/* The crypto functions the libraries call, wpa_crypto_funcs_t of esp_wifi_crypto_types.h; crypto.c fills them. */
#define CRYPTO_VERSION 0x00000001
struct crypto_funcs {
    uint32_t size, version;
    int (*hmac_sha256_vector)(const unsigned char *key, int key_len, int num_elem, const unsigned char *addr[],
                              const int *len, unsigned char *mac);
    int (*pbkdf2_sha1)(const char *passphrase, const char *ssid, size_t ssid_len, int iterations, unsigned char *buf,
                       size_t buflen);
    int (*aes_128_encrypt)(const unsigned char *key, const unsigned char *iv, unsigned char *data, int data_len);
    int (*aes_128_decrypt)(const unsigned char *key, const unsigned char *iv, unsigned char *data, int data_len);
    int (*omac1_aes_128)(const uint8_t *key, const uint8_t *data, size_t data_len, uint8_t *mic);
    uint8_t *(*ccmp_decrypt)(const uint8_t *tk, const uint8_t *ieee80211_hdr, const uint8_t *data, size_t data_len,
                             size_t *decrypted_len, bool espnow_pkt);
    uint8_t *(*ccmp_encrypt)(const uint8_t *tk, uint8_t *frame, size_t len, size_t hdrlen, uint8_t *pn, int keyid,
                             size_t *encrypted_len);
    int (*aes_gmac)(const uint8_t *key, size_t keylen, const uint8_t *iv, size_t iv_len, const uint8_t *aad,
                    size_t aad_len, uint8_t *mic);
    int (*sha256_vector)(size_t num_elem, const uint8_t *addr[], const size_t *len, uint8_t *buf);
    int (*aes_wrap)(const unsigned char *kek, size_t kek_len, int n, const unsigned char *plain, unsigned char *cipher);
    int (*aes_unwrap)(const unsigned char *kek, size_t kek_len, int n, const unsigned char *cipher,
                      unsigned char *plain);
};

#define INIT_CONFIG_MAGIC 0x1F2F3F4F
/* A bit of feature_caps, of esp_wifi.h: the libraries authenticate with WPA3's SAE through the supplicant. */
#define FEATURE_WPA3_SAE 0x1u
struct init_config {
    struct osi_funcs *osi_funcs;
    struct crypto_funcs crypto_funcs;
    int static_rx_buf_num, dynamic_rx_buf_num;
    int tx_buf_type, static_tx_buf_num, dynamic_tx_buf_num;
    int rx_mgmt_buf_type, rx_mgmt_buf_num;
    int cache_tx_buf_num;
    int csi_enable, ampdu_rx_enable, ampdu_tx_enable, amsdu_tx_enable;
    int nvs_enable, nano_enable;
    int rx_ba_win;
    int wifi_task_core_id;
    int beacon_max_len;
    int mgmt_sbuf_num;
    uint64_t feature_caps;
    bool sta_disconnected_pm;
    int espnow_max_encrypt_num;
    int tx_hetb_queue_num;
    bool dump_hesigb_enable;
    bool privacy_enhancements;
    uint8_t rmac_auto_reset_int;
    int wifi_task_stack_size;
    int magic;
};

/* An access point's RSN element as the supplicant parses it for the libraries, esp_wifi_driver.h's wifi_wpa_ie_t. */
struct wifi_wpa_ie {
    int proto;
    int pairwise_cipher; /* a WIFI_CIPHER_ */
    int group_cipher;
    int key_mgmt;        /* WPA_KEY_MGMT_ bits, as hostap's defs.h numbers them */
    int capabilities;
    size_t num_pmkid;
    const uint8_t *pmkid;
    int mgmt_group_cipher;
    uint8_t rsnxe_capa;
};

/*
 * The supplicant's functions, which ESP-IDF's esp_supplicant_init registers after esp_wifi_init_internal,
 * from its private esp_wifi_driver.h; the arguments the driver has no use for are left untyped.
 */
struct wpa_funcs {
    bool (*wpa_sta_init)(void);
    bool (*wpa_sta_deinit)(void);
    int (*wpa_sta_connect)(uint8_t *bssid);
    void (*wpa_sta_connected_cb)(uint8_t *bssid);
    void (*wpa_sta_disconnected_cb)(uint8_t reason_code);
    int (*wpa_sta_rx_eapol)(uint8_t *src_addr, uint8_t *buf, uint32_t len);
    bool (*wpa_sta_in_4way_handshake)(void);
    void *(*wpa_ap_init)(void);
    bool (*wpa_ap_deinit)(void *data);
    bool (*wpa_ap_join)(void *join);
    bool (*wpa_ap_remove)(uint8_t *bssid);
    uint8_t *(*wpa_ap_get_wpa_ie)(size_t *len);
    bool (*wpa_ap_rx_eapol)(void *hapd_data, void *sm, uint8_t *data, size_t data_len);
    void (*wpa_ap_get_peer_spp_msg)(void *sm, bool *spp_cap, bool *spp_req);
    char *(*wpa_config_parse_string)(const char *value, size_t *len);
    int (*wpa_parse_wpa_ie)(const uint8_t *wpa_ie, size_t wpa_ie_len, struct wifi_wpa_ie *data);
    int (*wpa_config_bss)(uint8_t *bssid);
    int (*wpa_michael_mic_failure)(uint16_t is_unicast);
    uint8_t *(*wpa3_build_sae_msg)(uint8_t *bssid, uint32_t type, size_t *len);
    int (*wpa3_parse_sae_msg)(uint8_t *buf, size_t len, uint32_t type, uint16_t status);
    int (*wpa3_hostap_handle_auth)(uint8_t *buf, size_t len, uint32_t type, uint16_t status, uint8_t *bssid);
    int (*wpa_sta_rx_mgmt)(uint8_t type, uint8_t *frame, size_t len, uint8_t *sender, int8_t rssi, uint8_t channel,
                           uint64_t current_tsf);
    void (*wpa_config_done)(void);
    uint8_t *(*owe_build_dhie)(uint16_t group);
    int (*owe_process_assoc_resp)(const uint8_t *rsn_ie, size_t rsn_len, const uint8_t *dh_ie, size_t dh_len);
    void (*wpa_sta_clear_curr_pmksa)(void);
    void (*wpa_config_reload)(void);
    int (*wpa_parse_wpa_ie_scan_only)(const uint8_t *wpa_ie, size_t wpa_ie_len, struct wifi_wpa_ie *data);
};

int esp_wifi_register_wpa_cb_internal(struct wpa_funcs *cb);

/* The messages of SAE that wpa3_build_sae_msg and wpa3_parse_sae_msg take, as ESP-IDF's fork's sae.h numbers them. */
#define SAE_MSG_COMMIT  1
#define SAE_MSG_CONFIRM 2

/* wifi_cipher_type_t. */
#define WIFI_CIPHER_NONE        0
#define WIFI_CIPHER_WEP40       1
#define WIFI_CIPHER_WEP104      2
#define WIFI_CIPHER_TKIP        3
#define WIFI_CIPHER_CCMP        4
#define WIFI_CIPHER_TKIP_CCMP   5
#define WIFI_CIPHER_AES_CMAC128 6
#define WIFI_CIPHER_SMS4        7
#define WIFI_CIPHER_GCMP        8
#define WIFI_CIPHER_GCMP256     9
#define WIFI_CIPHER_AES_GMAC128 10
#define WIFI_CIPHER_AES_GMAC256 11
#define WIFI_CIPHER_UNKNOWN     12

/* The authentication modes of a station's profile, as esp_wifi_driver.h numbers them for the supplicant. */
#define ESP_AUTH_NONE            0x01
#define ESP_AUTH_WPA2_PSK        0x05
#define ESP_AUTH_WPA2_PSK_SHA256 0x08
#define ESP_AUTH_WPA3_PSK        0x09

/*
 * esp_wifi_driver.h's enum wpa_alg, the keys the libraries take,
 * and its wifi_appie_t's elements: the RSN element, and those added to an association's request.
 */
#define ESP_ALG_NONE 0
#define ESP_ALG_TKIP 2
#define ESP_ALG_CCMP 3
#define ESP_ALG_WEP  6
#define ESP_ALG_GCMP 9
#define WIFI_APPIE_ASSOC_REQ 1
#define WIFI_APPIE_RSN       4

struct wifi_ssid {
    int len;
    uint8_t ssid[32];
};

/* A management group key, as the IGTK KDE lays it out. */
struct wifi_wpa_igtk {
    uint8_t keyid[2];
    uint8_t pn[6];
    uint8_t igtk[32];
};

/* A call esp_wifi_ipc_internal makes on the libraries' Wi-Fi task: fn(arg), arg copied if arg_size says so. */
struct wifi_ipc {
    int (*fn)(void *arg);
    void *arg;
    uint32_t arg_size;
};

/* What the supplicant asks of the libraries, from esp_wifi_driver.h and esp_private/wifi.h. */
uint8_t esp_wifi_sta_get_prof_authmode_internal(void);
struct wifi_ssid *esp_wifi_sta_get_prof_ssid_internal(void);
uint8_t *esp_wifi_sta_get_prof_password_internal(void);
uint8_t esp_wifi_sta_get_pairwise_cipher_internal(void);
uint8_t esp_wifi_sta_get_group_cipher_internal(void);
bool esp_wifi_sta_prof_is_rsn_internal(void);
/* How the station may derive SAE's password element, as hostap's enum sae_pwe numbers the ways. */
uint8_t esp_wifi_get_config_sae_pwe_h2e_internal(uint8_t if_index);
uint16_t esp_wifi_sta_pmf_enabled(void);
int esp_wifi_sta_get_mgmt_group_cipher(void);
uint8_t *esp_wifi_sta_get_ie(uint8_t *bssid, uint8_t elem_id);
int esp_wifi_get_macaddr_internal(uint8_t if_index, uint8_t *macaddr);
int esp_wifi_set_appie_internal(uint8_t type, uint8_t *ie, uint16_t len, uint8_t flag);
int esp_wifi_unset_appie_internal(uint8_t type);
esp_err_t esp_wifi_sta_connect_internal(const uint8_t *bssid);
void esp_wifi_deauthenticate_internal(uint8_t reason_code);
bool esp_wifi_auth_done_internal(void);
int esp_wifi_set_sta_key_internal(int alg, uint8_t *addr, int key_idx, int set_tx, uint8_t *seq, size_t seq_len,
                                  uint8_t *key, size_t key_len, int key_flag);
int esp_wifi_set_igtk_internal(uint8_t if_index, const struct wifi_wpa_igtk *igtk);
int esp_wifi_register_eapol_txdonecb_internal(void (*fn)(uint8_t *eapol, size_t len, bool failed));
esp_err_t esp_wifi_register_mgmt_frame_internal(uint32_t type, uint32_t subtype);
int esp_wifi_internal_tx(int ifx, void *buffer, uint16_t len);
int esp_wifi_ipc_internal(struct wifi_ipc *cfg, bool sync);

/* wifi_mode_t, wifi_interface_t, and the events of WIFI_EVENT the driver waits for. */
#define WIFI_MODE_STA         1
#define WIFI_IF_STA           0
#define WIFI_EVENT_SCAN_DONE        1
#define WIFI_EVENT_STA_START        2
#define WIFI_EVENT_STA_CONNECTED    4
#define WIFI_EVENT_STA_DISCONNECTED 5

/* wifi_auth_mode_t, as far as the driver names it: the weakest it joins, and what a connection reports. */
#define WIFI_AUTH_OPEN          0
#define WIFI_AUTH_WPA2_PSK      3
#define WIFI_AUTH_WPA3_PSK      6
#define WIFI_AUTH_WPA2_WPA3_PSK 7

/*
 * A station's configuration, wifi_sta_config_t, the member of wifi_config_t esp_wifi_set_config reads for one;
 * the union is larger, ESP_CONFIG_MAX bytes at most, and the rest of it is left zero.
 */
struct sta_config {
    uint8_t ssid[32];
    uint8_t password[64];
    int scan_method; /* wifi_scan_method_t */
    bool bssid_set;
    uint8_t bssid[6];
    uint8_t channel;
    uint16_t listen_interval;
    int sort_method; /* wifi_sort_method_t */
    struct {
        int8_t rssi;
        int authmode; /* wifi_auth_mode_t: the weakest the station joins */
        uint8_t rssi_5g_adjustment;
    } threshold;
    struct {
        bool capable, required;
    } pmf_cfg;
    uint32_t rm_enabled : 1, btm_enabled : 1, mbo_enabled : 1, ft_enabled : 1, owe_enabled : 1,
        transition_disable : 1, disable_wpa3_compatible_mode : 1, reserved1 : 25;
    int sae_pwe_h2e; /* wifi_sae_pwe_method_t */
    int sae_pk_mode; /* wifi_sae_pk_mode_t */
    uint8_t failure_retry_cnt;
    uint32_t he_bits; /* the HE and VHT options, all left at zero */
    uint8_t sae_h2e_identifier[32];
};
#define ESP_CONFIG_MAX 256u

/* wifi_event_sta_connected_t and wifi_event_sta_disconnected_t. */
struct sta_connected {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    int authmode;
    uint16_t aid;
};

struct sta_disconnected {
    uint8_t ssid[32];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t reason;
    int8_t rssi;
};

/* The first fields of wifi_ap_record_t, which is larger; a record is read into ESP_AP_RECORD_MAX bytes. */
struct ap_record {
    uint8_t bssid[6];
    uint8_t ssid[33];
    uint8_t primary;
    int32_t second;
    int8_t rssi;
};
#define ESP_AP_RECORD_MAX 256u

esp_err_t esp_wifi_init_internal(const struct init_config *config);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);

/* wifi_protocol_t's bits, of which the station takes those esp_wifi_set_protocol names. */
#define WIFI_PROTOCOL_11B  0x01u
#define WIFI_PROTOCOL_11G  0x02u
#define WIFI_PROTOCOL_11N  0x04u
#define WIFI_PROTOCOL_11AX 0x20u
esp_err_t esp_wifi_set_protocol(int ifx, uint8_t protocols);
esp_err_t esp_wifi_scan_start(const void *config, bool block);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number);
esp_err_t esp_wifi_scan_get_ap_record(void *record);
esp_err_t esp_wifi_get_mac(int ifx, uint8_t mac[6]);
esp_err_t esp_wifi_set_config(int ifx, void *config);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect_internal(void);

/* The libraries' own log: its level, wifi_log_level_t, and its modules, all of them with module 0 and submodule 0. */
#define WIFI_LOG_INFO 3
esp_err_t esp_wifi_internal_set_log_level(int level);
esp_err_t esp_wifi_internal_set_log_mod(int module, uint32_t submodule, bool enable);
/* The libraries' counters, of the radio's receiving and sending with WIFI_STATIS_RXTX, into their log. */
#define WIFI_STATIS_RXTX 0x2u
esp_err_t esp_wifi_statis_dump(uint32_t modules);

/* wifi_ps_type_t: none keeps the radio on between beacons; minimum modem sleeps until each DTIM beacon. */
#define WIFI_PS_NONE      0
#define WIFI_PS_MIN_MODEM 1
esp_err_t esp_wifi_set_ps(int type);

/*
 * What the radio hears, promiscuous, while the station stays joined: each frame after its reception's metadata,
 * the 92 bytes of esp_wifi_rxctrl_t of ESP-IDF's esp_wifi_he_types.h for the C6's MAC, version 2,
 * of which these bytes are read; the filters are wifi_promiscuous_filter_t, a mask alone.
 */
#define RX_CTRL_SIZE      92
#define RX_CTRL_RSSI      0  /* int8_t, dBm */
#define RX_CTRL_RXEND     8  /* the reception's state at its end, 0 if it succeeded */
#define RX_CTRL_TIMESTAMP 12 /* uint32_t, the MAC's microseconds at the reception */
#define RX_CTRL_FORMAT    39 /* bits 0 to 3: wifi_rx_bb_format_t, 0 for 802.11b, 1 for 802.11a/g */
#define RX_CTRL_SIG_LEN   84 /* bits 0 to 13: the MPDU's length, its FCS included */
#define RX_CTRL_STATE     88 /* 0 if the frame was received whole */

/* The MPDU's length, and whether it was received whole. */
static inline uint32_t rx_ctrl_len(const uint8_t *c)
{
    return ((uint32_t)c[RX_CTRL_SIG_LEN] | (uint32_t)c[RX_CTRL_SIG_LEN + 1] << 8) & 0x3fffu;
}

static inline int rx_ctrl_whole(const uint8_t *c)
{
    return c[RX_CTRL_STATE] == 0 && c[RX_CTRL_RXEND] == 0;
}

#define WIFI_PKT_MGMT     0
#define WIFI_PKT_CTRL     1
#define WIFI_PKT_DATA     2
#define WIFI_PROMIS_FILTER_MASK_MGMT     0x01u
#define WIFI_PROMIS_FILTER_MASK_CTRL     0x02u
#define WIFI_PROMIS_FILTER_MASK_DATA     0x04u
#define WIFI_PROMIS_FILTER_MASK_FCSFAIL  0x40u
#define WIFI_PROMIS_CTRL_FILTER_MASK_BA  (1u << 25)
#define WIFI_PROMIS_CTRL_FILTER_MASK_ACK (1u << 29)
esp_err_t esp_wifi_set_promiscuous_rx_cb(void (*cb)(void *buf, int type));
esp_err_t esp_wifi_set_promiscuous_filter(const uint32_t *mask);
esp_err_t esp_wifi_set_promiscuous_ctrl_filter(const uint32_t *mask);
esp_err_t esp_wifi_set_promiscuous(bool on);
/* The channel the radio is on, with wifi_second_chan_t's WIFI_SECOND_CHAN_NONE, 0, for 20 MHz. */
esp_err_t esp_wifi_set_channel(uint8_t primary, int second);

/* The station's received frames, as Ethernet frames, each to be freed by eb once read. */
esp_err_t esp_wifi_internal_reg_rxcb(int ifx, esp_err_t (*fn)(void *buffer, uint16_t len, void *eb));
void esp_wifi_internal_free_rx_buffer(void *eb);

#endif
