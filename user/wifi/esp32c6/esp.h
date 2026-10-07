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

/* wifi_mode_t, and the event of WIFI_EVENT the driver waits for. */
#define WIFI_MODE_STA        1
#define WIFI_EVENT_STA_START 2

/* wifi_auth_mode_t, as far as the driver names it: what the station joined by. */
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_WPA3_PSK 6

/* The ROM's delay, which counts the core's cycles, as many a microsecond as the ROM was last told. */
void ets_delay_us(uint32_t us);
void ets_update_cpu_frequency(uint32_t ticks_per_us);

esp_err_t esp_wifi_init_internal(const struct init_config *config);
esp_err_t esp_wifi_set_mode(int mode);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);

/* The libraries' own log: its level, wifi_log_level_t, and its modules, all of them with module 0 and submodule 0. */
#define WIFI_LOG_INFO 3
esp_err_t esp_wifi_internal_set_log_level(int level);
esp_err_t esp_wifi_internal_set_log_mod(int module, uint32_t submodule, bool enable);

/*
 * What the MAC writes before each frame it receives, the 92 bytes of esp_wifi_rxctrl_t
 * of ESP-IDF's esp_wifi_he_types.h for the C6's MAC, version 2, of which these bytes are read.
 */
#define RX_CTRL_SIZE      92
#define RX_CTRL_RSSI      0  /* int8_t, dBm */
#define RX_CTRL_RXEND     8  /* the reception's state at its end, 0 if it succeeded */
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

/* The libraries' promiscuous mode, whose filter of types, wifi_promiscuous_filter_t, a mask alone, mac.c sets. */
#define WIFI_PROMIS_FILTER_MASK_MGMT    0x01u
#define WIFI_PROMIS_FILTER_MASK_CTRL    0x02u
#define WIFI_PROMIS_FILTER_MASK_DATA    0x04u
#define WIFI_PROMIS_FILTER_MASK_FCSFAIL 0x40u
esp_err_t esp_wifi_set_promiscuous_filter(const uint32_t *mask);
esp_err_t esp_wifi_set_promiscuous(bool on);

#endif
