/*
 * The station's own logic; see sta.h.
 *
 * The station's thread waits for events in a queue of the adapter's: its frames, and the root task's ask to leave.
 * A step sends a frame and waits for the answer until a deadline, wait_for.
 * The receiving runs in the interrupt's thread, which must not wait, so it copies a frame to the station
 * from its access point into one of STA_FRAMES buffers, which a second queue hands out and the station gives back,
 * and drops the frame, counted, when all are taken.
 * The events queue has room for every buffer and one more, so that the ask to leave always fits.
 */

#include <stdint.h>

#include "lib/libc.h"
#include "mac.h"
#include "mgmt.h"
#include "osi.h"
#include "sta.h"

#define STA_FRAMES    4u    /* the frames that may wait for the station at once */
#define STA_FRAME_MAX 1024u /* the longest the station keeps; a longer one is dropped and counted */
#define STA_STACK     4096u

enum { EV_FRAME, EV_LEAVE };

struct event {
    uint32_t kind;
    uint32_t buf; /* EV_FRAME's buffer */
};

struct buf {
    uint32_t len;
    uint8_t frame[STA_FRAME_MAX];
};

static struct {
    struct queue *events, *free; /* the free queue holds the numbers of the buffers free */
    struct buf *bufs;
    uint8_t ap[6]; /* the access point chosen, which the receiving reads once the station has taken it */
    volatile uint32_t kept, dropped;
} sta;

/* --- The scan. --- */

/* Each access point kept as heard strongest, and of more than DRV_NETS, the strongest; in the receiving's thread. */
static void scan_heard(const struct mac_frame *m)
{
    struct drv *d = drv_self;
    struct mgmt_beacon b;
    int rssi = m->rssi;
    if (!mgmt_beacon(m->frame, m->len, (uint8_t)m->channel, &b) || b.probe_response) {
        return;
    }
    uint32_t n = d->net_count, weakest = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (memcmp(d->nets[i].bssid, b.bssid, 6) == 0) {
            if (rssi > d->nets[i].rssi) {
                d->nets[i].rssi = (int16_t)rssi;
                d->nets[i].channel = b.channel;
            }
            return;
        }
        if (d->nets[i].rssi < d->nets[weakest].rssi) {
            weakest = i;
        }
    }
    struct drv_net *a = n < DRV_NETS ? &d->nets[n] : rssi > d->nets[weakest].rssi ? &d->nets[weakest] : 0;
    if (a == 0) {
        return;
    }
    memcpy(a->bssid, b.bssid, 6);
    memcpy(a->ssid, b.ssid, b.ssid_len);
    a->ssid[b.ssid_len] = 0;
    a->channel = b.channel;
    a->rssi = (int16_t)rssi;
    if (n < DRV_NETS) {
        d->net_count = n + 1;
    }
}

void sta_scan(struct drv *d)
{
    drv_must_mac("every management frame heard", mac_hear(MAC_HEAR_MGMT));
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(scan_heard));
    for (uint32_t ch = MAC_CHANNEL_FIRST; ch <= MAC_CHANNEL_LAST; ch++) {
        mac_channel(ch); /* one of those it tunes to, so never refused */
        osi_delay_ms(SCAN_DWELL_MS);
    }
    mac_rx_give_back();
    drv_mac_told("scan");
    drv_must("an access point heard", d->net_count != 0 ? ESP_OK : ESP_FAIL);
    for (uint32_t i = 1; i < d->net_count; i++) {
        for (uint32_t j = i; j > 0 && d->nets[j].rssi > d->nets[j - 1].rssi; j--) {
            struct drv_net t = d->nets[j];
            d->nets[j] = d->nets[j - 1];
            d->nets[j - 1] = t;
        }
    }
}

/* --- The events. --- */

/* A frame to the station from its access point, kept for the station's thread; in the receiving's thread. */
static void heard(const struct mac_frame *m)
{
    if (!mgmt_to_station(m->frame, m->len, drv_self->mac, sta.ap)) {
        return;
    }
    struct event e = { EV_FRAME, 0 };
    if (m->len > STA_FRAME_MAX || !osi_queue_get(sta.free, &e.buf, 0)) {
        sta.dropped++;
        return;
    }
    struct buf *b = &sta.bufs[e.buf];
    memcpy(b->frame, m->frame, m->len);
    b->len = m->len;
    sta.kept++;
    osi_queue_put(sta.events, &e, 0);
}

/* The station leaves as the root task asks: the receiving given back, the radio off, and DRV_LEFT. */
static __attribute__((noreturn)) void leave(void)
{
    mac_rx_give_back();
    drv_radio_off();
    drv_say("sta: left, as the root task asked; the radio is off\n");
    drv_stop(DRV_LEFT);
}

/*
 * The events until the deadline, in osi_now_us's microseconds, each frame offered to read, given back after:
 * 1 once read takes one, the frame the step waits for, 0 at the deadline;
 * the ask to leave ends the station whatever the step.
 */
static int wait_for(uint64_t deadline, int (*read)(const struct buf *b, void *arg), void *arg)
{
    for (;;) {
        uint64_t now = osi_now_us();
        struct event e;
        if (now >= deadline || !osi_queue_get(sta.events, &e, (uint32_t)((deadline - now + 999u) / 1000u))) {
            return 0;
        }
        if (e.kind == EV_LEAVE) {
            leave();
        }
        int taken = read(&sta.bufs[e.buf], arg);
        osi_queue_put(sta.free, &e.buf, 0);
        if (taken) {
            return 1;
        }
    }
}

void sta_leave(void)
{
    static const struct event e = { EV_LEAVE, 0 };
    osi_queue_put(sta.events, &e, 0);
}

/* --- The station. --- */

static int streq(const char *a, const char *b)
{
    size_t n = strlen(a);
    return n == strlen(b) && memcmp(a, b, n) == 0;
}

/* The access point for the network: the page's bssid, or the strongest whose ssid is the network's. */
static const struct drv_net *net_for(const struct drv *d)
{
    for (uint32_t i = 0; i < d->net_count; i++) {
        if (d->bssid_set ? memcmp(d->nets[i].bssid, d->bssid, 6) == 0 : streq(d->nets[i].ssid, d->ssid)) {
            return &d->nets[i];
        }
    }
    return 0;
}

/*
 * An open-system Authentication sent by mac_tx, and the access point's answer read; one lost to the air,
 * or answered too late, is sent again a few times, as a station does; one refused, or never answered, fails.
 */
#define AUTH_TRIES   3u
#define AUTH_WAIT_US 200000u

static int auth_answer(const struct buf *b, void *arg)
{
    return mgmt_auth_answer(b->frame, b->len, drv_self->mac, sta.ap, arg);
}

static void authenticate(struct drv *d)
{
    static uint8_t frame[MGMT_FRAME_MAX];
    uint32_t n = mgmt_auth_request(frame, sizeof(frame), d->mac, sta.ap);
    uint16_t status = 0;
    int answered = 0;
    for (uint32_t tries = 1; tries <= AUTH_TRIES && !answered; tries++) {
        const char *failed = mac_tx(frame, n);
        if (failed) {
            drv_say("sta: the authentication frame, try %u: %s\n", (unsigned)tries, failed);
            continue;
        }
        answered = wait_for(osi_now_us() + AUTH_WAIT_US, auth_answer, &status);
    }
    drv_say("sta: %s, status %u\n", answered ? (status == 0 ? "accepted" : "refused") : "no answer",
            (unsigned)status);
    drv_must("the authentication answered", answered && status == 0 ? ESP_OK : ESP_FAIL);
}

/*
 * The station's thread: the scan, the access point's channel, the receiving the station's, and the authentication.
 * The libraries' station is not asked to connect, so the run ends after the authentication, DRV_SCANNED,
 * and the association, the keys and the join are to follow; see TODO.md.
 */
static void sta_main(void *arg)
{
    struct drv *d = arg;
    sta_scan(d);
    const struct drv_net *ap = net_for(d);
    drv_must("the network to authenticate to", ap ? ESP_OK : ESP_FAIL);
    memcpy(sta.ap, ap->bssid, 6);
    drv_say("sta: %s at %02x:%02x:%02x:%02x:%02x:%02x, channel %u\n", ap->ssid, ap->bssid[0], ap->bssid[1],
            ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5], (unsigned)ap->channel);
    /* The channel a beacon named, which mac_channel may refuse, tuned before the MAC is the driver's. */
    drv_must_mac("the access point's channel", mac_channel(ap->channel));
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(heard));
    authenticate(d);
    mac_rx_give_back();
    drv_say("sta: frames to the station kept %u, dropped %u\n", (unsigned)sta.kept, (unsigned)sta.dropped);
    d->joined = *ap;
    drv_radio_off();
    drv_say("sta: authenticated; the radio is off\n");
    drv_stop(DRV_SCANNED);
}

const char *sta_start(struct drv *d)
{
    sta.bufs = osi_calloc(STA_FRAMES, sizeof(struct buf));
    sta.events = osi_queue_new(STA_FRAMES + 1u, sizeof(struct event));
    sta.free = osi_queue_new(STA_FRAMES, sizeof(uint32_t));
    if (sta.bufs == 0 || sta.events == 0 || sta.free == 0) {
        return "the station's buffers and queues";
    }
    for (uint32_t i = 0; i < STA_FRAMES; i++) {
        osi_queue_put(sta.free, &i, 0);
    }
    return osi_thread(sta_main, d, "station", STA_STACK) ? 0 : "the station's thread";
}
