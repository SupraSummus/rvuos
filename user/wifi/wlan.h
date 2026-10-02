#ifndef RVUOS_WIFI_WLAN_H
#define RVUOS_WIFI_WLAN_H

/*
 * Talking to the CYW43439's firmware once it runs, over the bus's function 2.
 * Every packet starts with an SDPCM header, whose channel says what follows:
 * a control answer (a CDC header and an ioctl's result), an event (BDC, then a Broadcom event in an Ethernet frame),
 * or data (BDC, then an Ethernet frame).
 * The firmware lends credit, a window of sequence numbers the host may send in, with every packet it sends.
 * The layout is Infineon's WHD, as embassy's cyw43 driver has it.
 */

#include <stdint.h>

#include "cyw43.h"

#define WLAN_MTU 1514u
#define WLAN_PACKET_MAX 2048u

/* Events, as the firmware numbers them; see wlan.c for the mask it is given. */
#define WLAN_E_SET_SSID     0u
#define WLAN_E_JOIN         1u
#define WLAN_E_AUTH         3u
#define WLAN_E_DEAUTH       5u
#define WLAN_E_DEAUTH_IND   6u
#define WLAN_E_DISASSOC_IND 12u
#define WLAN_E_LINK         16u
#define WLAN_E_PSK_SUP      46u
#define WLAN_E_ESCAN_RESULT 69u

#define WLAN_STATUS_SUCCESS     0u
#define WLAN_STATUS_PARTIAL     8u
#define WLAN_STATUS_UNSOLICITED 6u

struct wlan_event {
    uint32_t type, status, reason, auth_type, flags;
    const uint8_t *data;
    uint32_t len;
};

struct wlan;
typedef void (*wlan_event_fn)(struct wlan *w, const struct wlan_event *e);
typedef void (*wlan_data_fn)(struct wlan *w, const uint8_t *frame, uint32_t len);

struct wlan {
    struct cyw43 *chip;
    wlan_event_fn on_event;
    wlan_data_fn on_data;
    uint8_t seq, seq_max;
    uint16_t ioctl_id;
    /* The answer to the ioctl in flight, copied here when it comes. */
    uint8_t *answer;
    uint32_t answer_room, answer_len;
    int32_t answer_status;
    int answered;
    /* One packet either way; the first word is the bus's command. */
    uint32_t buf[1 + WLAN_PACKET_MAX / 4];
};

/* Reads every packet the firmware has waiting and hands each on; how many there were. */
uint32_t wlan_poll(struct wlan *w);

/* An ioctl of the firmware's: kind 0 gets, 2 sets. 0, or the firmware's error, or -1 for no answer. */
int32_t wlan_ioctl(struct wlan *w, uint32_t kind, uint32_t cmd, const void *data, uint32_t len, void *out, uint32_t room);
int32_t wlan_set_u32(struct wlan *w, uint32_t cmd, uint32_t value);
int32_t wlan_set_var(struct wlan *w, const char *name, const void *value, uint32_t len);
int32_t wlan_set_var_u32(struct wlan *w, const char *name, uint32_t value);
int32_t wlan_get_var(struct wlan *w, const char *name, void *out, uint32_t len);

/* The firmware's setup after boot: the CLM, the country, the events, up. mac gets the chip's address. */
int32_t wlan_init(struct wlan *w, const uint8_t *clm, uint32_t clm_len, uint8_t mac[6]);
/* Starts an active scan of every channel; results come as WLAN_E_ESCAN_RESULT events. */
int32_t wlan_scan(struct wlan *w);

/*
 * Joins a network as a station: WPA2 with AES when a passphrase is given, open when it is empty.
 * The outcome comes as events, which wlan_join_event reads.
 */
int32_t wlan_join(struct wlan *w, const char *ssid, const char *passphrase);
/* What an event says of a join: 1 joined, -1 failed, 0 nothing yet. */
int wlan_join_event(const struct wlan_event *e, int secure);
/* Starts an access point with WPA2 and AES, on a channel. */
int32_t wlan_start_ap(struct wlan *w, const char *ssid, const char *passphrase, uint32_t channel);

/* A scan result's fields, from an ESCAN_RESULT event of status PARTIAL; 0 if it holds none. */
struct wlan_bss {
    uint8_t bssid[6];
    uint8_t ssid_len;
    char ssid[33];
    int16_t rssi;
    uint8_t channel;
};
int wlan_bss_parse(const struct wlan_event *e, struct wlan_bss *bss);

#endif
