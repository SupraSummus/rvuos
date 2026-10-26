/*
 * Wi-Fi heard on channels of both bands: the MAC brought up as the station's is, its sniffer on so that it passes
 * every frame it hears, and each channel of channels= listened on in turn for dwell= seconds,
 * 1, 6, 11, 36, 40, 44 and 48 for 3 s each by default; sniff=0 leaves the sniffer off.
 * For each channel it tells the frames heard and the beacons among them, and each access point the first time
 * a beacon of it is heard: its address, the channel it was heard on and the one its DS element names, its RSSI and name.
 * It sends nothing, and may listen on every channel the MAC may be tuned to, those of DFS among them.
 *
 * Run it with make BOARD=esp32c5 radio PROG=listen RADIO_CONFIG=file, see build.mk.
 */

#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"
#include "wifi/conf.h"

DRV_PROGRAM("the listener", DRV_NEED_RNG); /* the MAC's start draws its delays from the generator on this chip */

#define CHANNELS_MAX 16u
#define APS_MAX      48u

struct ap {
    uint8_t bssid[6];
    uint8_t ds;       /* the channel the beacon's DS element names, or 0 */
    uint8_t heard_on; /* the channel it was heard on */
    int8_t rssi;
    char ssid[33];
    uint8_t told;
};

static struct {
    volatile uint32_t frames, beacons;
    struct ap aps[APS_MAX];
    volatile uint32_t ap_count;
} heard_now;

static int up;

void drv_radio_off(void)
{
    if (up) {
        up = 0;
        mac_rx_give_back();
        mac_stop();
    }
}

/* The beacon's DS element's channel and its SSID, from the elements after its 12 fixed bytes. */
static void elements(const uint8_t *body, uint32_t len, struct ap *a)
{
    for (uint32_t i = 12; i + 2 <= len;) {
        uint8_t id = body[i], n = body[i + 1];
        if (i + 2u + n > len) {
            break;
        }
        if (id == 0 && n <= 32) {
            memcpy(a->ssid, body + i + 2, n);
            a->ssid[n] = 0;
        } else if (id == 3 && n == 1) {
            a->ds = body[i + 2];
        }
        i += 2u + n;
    }
}

/* Each frame, received whole, in the interrupt's thread: counted, and a beacon's sender kept the first time. */
static int heard(const struct mac_frame *f)
{
    heard_now.frames++;
    if (f->len < 24u + 12u || f->frame[0] != 0x80) {
        return 0;
    }
    heard_now.beacons++;
    const uint8_t *bssid = f->frame + 16;
    for (uint32_t i = 0; i < heard_now.ap_count; i++) {
        if (memcmp(heard_now.aps[i].bssid, bssid, 6) == 0) {
            return 0;
        }
    }
    if (heard_now.ap_count == APS_MAX) {
        return 0;
    }
    struct ap *a = &heard_now.aps[heard_now.ap_count];
    memset(a, 0, sizeof(*a));
    memcpy(a->bssid, bssid, 6);
    a->rssi = (int8_t)f->rssi;
    a->heard_on = (uint8_t)f->channel;
    elements(f->frame + 24, f->len - 24u, a);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    heard_now.ap_count++;
    return 0;
}

/* The station's start done, which the station posts an event for; this program waits on nothing. */
static void station_started(void)
{
}

/* The numbers of a key's value, as many as there is room for, separated by commas; 0 if the key is not there. */
static uint32_t numbers(const struct drv *d, const char *key, uint32_t *out, uint32_t room)
{
    char text[96];
    config_value(d->conf, d->conf_len, key, text, sizeof(text));
    uint32_t n = 0, v = 0, digits = 0;
    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10u + (uint32_t)(*p - '0');
            digits++;
        } else {
            if (digits != 0 && n < room) {
                out[n++] = v;
            }
            v = digits = 0;
            if (*p == 0) {
                return n;
            }
        }
    }
}

static void tell_aps(void)
{
    uint32_t n = heard_now.ap_count;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) {
        struct ap *a = &heard_now.aps[i];
        if (a->told) {
            continue;
        }
        a->told = 1;
        const uint8_t *m = a->bssid;
        drv_say("listen: ap %02x:%02x:%02x:%02x:%02x:%02x on %u, its DS %u, %d dBm: %s\n", m[0], m[1], m[2], m[3], m[4],
                m[5], a->heard_on, a->ds, a->rssi, a->ssid);
    }
}

void drv_run(struct drv *d, struct self *own)
{
    (void)own;
    uint32_t channels[CHANNELS_MAX] = { 1, 6, 11, 36, 40, 44, 48 };
    uint32_t count = numbers(d, "channels", channels, CHANNELS_MAX);
    if (count == 0) {
        count = 7;
    }
    uint32_t dwell = config_number(d->conf, d->conf_len, "dwell", 3);
    for (uint32_t i = 0; i < count; i++) {
        drv_must("a channel the radio may be tuned to", mac_channel_may_tune(channels[i]) ? ESP_OK : ESP_FAIL);
    }
    mac_up(d->mac);
    up = 1;
    mac_station_start(d->mac, station_started);
    if (config_number(d->conf, d->conf_len, "sniff", 1)) {
        mac_sniffer(1);
    }
    drv_must_mac("the MAC's receiving", mac_rx_take(heard));
    child_report(&d->c, DRV_LISTENING);
    for (uint32_t round = 0; !d->leave; round++) {
        uint32_t ch = channels[round % count];
        drv_must_mac("the channel", mac_channel(ch));
        d->channel = ch;
        uint32_t frames = heard_now.frames, beacons = heard_now.beacons;
        for (uint32_t s = 0; s < dwell && !d->leave; s++) {
            osi_delay_ms(1000);
            child_answer(&d->c);
            tell_aps();
        }
        drv_say("listen: channel %u, %u MHz: %u frames, %u beacons; %u access points in all\n", ch,
                drv_phy_channel_mhz(ch), heard_now.frames - frames, heard_now.beacons - beacons, heard_now.ap_count);
    }
    drv_radio_off();
    drv_say("listen: the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}
