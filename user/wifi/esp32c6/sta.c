/*
 * The station's own logic; see sta.h.
 *
 * The station's thread waits for events in a queue of the adapter's: its frames, the supplicant's work,
 * and the root task's ask to leave.
 * A step sends a frame and waits for the answer until a deadline, wait_for.
 * The receiving runs in the interrupt's thread, which must not wait, so it copies a frame to the station
 * from its access point into one of STA_FRAMES buffers, which a second queue hands out and the station gives back,
 * and drops the frame, counted, when all are taken.
 * The events queue has room for every buffer, for STA_RUNS of the supplicant's work, and for the ask to leave.
 *
 * The supplicant, hostap's, see supp.h, runs in the station's thread, over the station's link:
 * the EAPOL frames it sends go out as data frames by mac_tx, in the clear even once the keys are set, see TODO.md,
 * and the keys it derives are kept for the station's CCMP, under which it then serves the link.
 */

#include <stdint.h>

#include "ccmp.h"
#include "lib/libc.h"
#include "mac.h"
#include "mgmt.h"
#include "osi.h"
#include "sta.h"
#include "supp.h"

#define STA_FRAMES    4u    /* the frames that may wait for the station at once */
#define STA_FRAME_MAX 1600u /* the longest the station keeps: a whole data frame with its CCMP header and MIC */
#define STA_RUNS      2u    /* the supplicant's work that may wait at once; see link_run */
#define STA_STACK     4096u /* room for the supplicant's handshakes, whose depth told() says */

enum { EV_FRAME, EV_RUN, EV_LEAVE };

struct event {
    uint32_t kind;
    uint32_t buf;         /* EV_FRAME's buffer */
    int (*fn)(void *arg); /* EV_RUN's work, and its argument */
    void *arg;
};

struct buf {
    uint32_t len;
    uint8_t frame[STA_FRAME_MAX];
};

/* How far the station has come with its access point, which says what leaving it undoes. */
enum { NONE, AUTHENTICATED, ASSOCIATED };

/*
 * A key the handshake installed, with the packet numbers CCMP uses: one for sending, one for receiving,
 * the last received being the replay counter, which a frame's must exceed.
 */
struct sta_key {
    volatile int set; /* written by the station's thread, read by the receiving's once it serves */
    uint8_t tk[CCMP_TK_LEN];
    uint8_t id;
    uint64_t tx_pn, rx_pn;
};

static struct {
    struct queue *events, *free; /* the free queue holds the numbers of the buffers free */
    struct buf *bufs;
    uint8_t ap[6]; /* the access point chosen, which the receiving reads once the station has taken it */
    volatile uint32_t beacon_wanted; /* the receiving keeps what the access point sends every station too */
    volatile uint32_t kept, dropped, runs_lost;
    volatile uint32_t state; /* the station's thread writes it, the first thread reads it to send the link's frames */
    /* The access point's RSN and RSNX elements, whole, from its beacon, and the station's, from the supplicant. */
    uint8_t ap_rsn[2 + 255], ap_rsnx[2 + 255];
    uint32_t ap_rsn_len, ap_rsnx_len;
    uint8_t rsn[80], rsnx[8];
    int has_rsnx;
    int completed; /* the supplicant's handshakes are done */
    uint32_t eapol_heard, eapol_sent;
    uint32_t relayed; /* the station's own group frames, back from the access point */
    /* The keys, and the frames CCMP works between. */
    struct sta_key ptk, gtk;
    uint8_t *eth, *tx_plain, *tx_crypt, *rx_plain;
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

/*
 * A frame to the station from its access point, kept for the station's thread; once the group key is set,
 * a data frame, a deauthentication or a disassociation to the group too; and while the station reads a beacon,
 * one from the access point to every station; in the receiving's thread.
 */
static void heard(const struct mac_frame *m)
{
    static const uint8_t every[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int group = sta.gtk.set && mgmt_to_group(m->frame, m->len, sta.ap);
    if (!mgmt_to_station(m->frame, m->len, drv_self->mac, sta.ap) &&
        !(sta.beacon_wanted && mgmt_to_station(m->frame, m->len, every, sta.ap)) && !group) {
        return;
    }
    struct event e = { EV_FRAME, 0, 0, 0 };
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

/* The access point is the station's no longer; the supplicant is told if it was associated. Whether it was, 1 or 0. */
static int forget(void)
{
    uint32_t was = sta.state;
    sta.state = NONE;
    if (was == ASSOCIATED) {
        supp_disassociated();
    }
    return was != NONE;
}

/*
 * The station leaves the access point it authenticated to, for reason: a deauthentication, sent once,
 * as a station leaves, which ends an association too, so that the access point keeps none.
 */
static void part(uint16_t reason)
{
    static uint8_t frame[MGMT_FRAME_MAX];
    if (forget()) {
        const char *failed = mac_tx(frame, mgmt_deauth(frame, sizeof(frame), drv_self->mac, sta.ap, reason));
        drv_say("sta: deauthenticated, reason %u%s%s\n", (unsigned)reason, failed ? ": " : "", failed ? failed : "");
    }
}

/* What the station counted, into the log, once the receiving is given back. */
static void told(void)
{
    /* Two lines, as drv_say's line holds 160 bytes. */
    drv_say("sta: frames to the station kept %u, dropped %u; EAPOL frames heard %u, sent %u; "
            "the supplicant's work lost %u\n",
            (unsigned)sta.kept, (unsigned)sta.dropped, (unsigned)sta.eapol_heard, (unsigned)sta.eapol_sent,
            (unsigned)sta.runs_lost);
    drv_say("sta: the link's frames in %u, out %u, its own back from the access point %u; "
            "the stack's deepest %u bytes of %u\n",
            (unsigned)drv_self->rx_frames, (unsigned)drv_self->tx_frames, (unsigned)sta.relayed,
            (unsigned)osi_stack_used(osi_self()), (unsigned)STA_STACK);
    drv_mac_told("sta");
}

/* The station fails at step: it leaves the access point, gives the receiving back, and stops as drv_must does. */
static __attribute__((noreturn)) void fail(const char *step, uint32_t detail)
{
    part(MGMT_LEAVING);
    mac_rx_give_back();
    told();
    drv_failed(step, detail);
    drv_radio_off();
    drv_stop(CHILD_FAILED);
}

/* A step of the station's that must succeed. */
static void must(const char *step, int ok)
{
    if (!ok) {
        fail(step, (uint32_t)ESP_FAIL);
    }
    drv_say("sta: %s\n", step);
}

/* The station leaves as the root task asks: the access point, then the receiving, the radio, and DRV_LEFT. */
static __attribute__((noreturn)) void leave(void)
{
    part(MGMT_LEAVING);
    mac_rx_give_back();
    told();
    drv_radio_off();
    drv_say("sta: left, as the root task asked; the radio is off\n");
    drv_stop(DRV_LEFT);
}

/*
 * The events until the deadline, in osi_now_us's microseconds, each frame offered to read, given back after:
 * 1 once read takes one, the frame the step waits for, 0 at the deadline.
 * The supplicant's work runs between them; the ask to leave ends the station whatever the step,
 * and so does the access point letting it, or every station, go, once it has authenticated.
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
        if (e.kind == EV_RUN) {
            e.fn(e.arg);
            continue;
        }
        const struct buf *b = &sta.bufs[e.buf];
        if (drv_self->debug & DRV_DEBUG_FRAMES) {
            drv_say("sta: heard frame control %02x%02x, %u bytes\n", b->frame[0], b->frame[1], (unsigned)b->len);
        }
        uint16_t reason = 0;
        int gone = sta.state != NONE && (mgmt_let_go(b->frame, b->len, drv_self->mac, sta.ap, &reason) ||
                                         mgmt_let_go_group(b->frame, b->len, sta.ap, &reason));
        int taken = !gone && read(b, arg);
        osi_queue_put(sta.free, &e.buf, 0);
        if (gone) {
            forget();
            fail("let go by the access point", reason);
        }
        if (taken) {
            return 1;
        }
    }
}

void sta_leave(void)
{
    static const struct event e = { EV_LEAVE, 0, 0, 0 };
    osi_queue_put(sta.events, &e, 0);
}

/* --- The supplicant's link. --- */

#define EAPOL_MAX 512u /* the longest frame the supplicant sends, a message of the handshakes with its elements */

/* A frame of the supplicant's, of Ethernet type proto, to dest through the access point: 0, or -1 if not sent. */
static int link_send(const uint8_t *dest, uint16_t proto, const uint8_t *buf, size_t len)
{
    static uint8_t frame[MGMT_DATA_FIXED + EAPOL_MAX];
    uint32_t n = mgmt_data(frame, sizeof(frame), drv_self->mac, sta.ap, dest, proto, buf, (uint32_t)len);
    const char *failed = n ? mac_tx(frame, n) : "longer than the station sends";
    if (failed) {
        drv_say("sta: a frame of the supplicant's, %u bytes: %s\n", (unsigned)len, failed);
        return -1;
    }
    sta.eapol_sent++;
    return 0;
}

/* A key the supplicant derived, kept in the station for its CCMP; a key removed, or one of another algorithm, ignored. */
static int link_set_key(const struct supp_key *k)
{
    struct sta_key *key = (k->flag & SUPP_KEY_GROUP) ? &sta.gtk : &sta.ptk;
    const char *what = key == &sta.gtk ? "the group's" : "the pairwise";
    if (k->alg == SUPP_ALG_NONE) {
        key->set = 0;
        drv_say("sta: %s key removed\n", what);
        return 0;
    }
    if (k->alg != SUPP_ALG_CCMP || k->key == 0 || k->key_len != CCMP_TK_LEN) {
        drv_say("sta: a key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
    memcpy(key->tk, k->key, CCMP_TK_LEN);
    key->id = (uint8_t)(k->idx & 3);
    key->tx_pn = 0;
    key->rx_pn = 0;
    /* The receive sequence counter the handshake gives, little-endian as the CCMP header's packet number. */
    for (uint32_t i = 0; k->seq != 0 && i < k->seq_len && i < 6u; i++) {
        key->rx_pn |= (uint64_t)k->seq[i] << (8u * i);
    }
    key->set = 1;
    drv_say("sta: %s key installed, index %d\n", what, key->id);
    return 0;
}

static void link_completed(void)
{
    sta.completed = 1;
}

/* The supplicant ends the association, a handshake having failed: the station leaves for its reason, and fails. */
static void link_deauthenticate(uint16_t reason)
{
    part(reason);
    fail("the supplicant's handshakes", reason);
}

/*
 * The supplicant's work, its timeouts, which hostap.c's alarm asks to run in its thread, the station's.
 * The supplicant arms the alarm in this thread, so a ring or two wait at most;
 * one the queue has no room for is counted, and its timeouts wait for the next.
 */
static void link_run(int (*fn)(void *arg), void *arg)
{
    struct event e = { EV_RUN, 0, fn, arg };
    if (!osi_queue_put(sta.events, &e, 0)) {
        sta.runs_lost++;
    }
}

static const struct supp_link link = { link_send, link_set_key, link_completed, link_deauthenticate, link_run };

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

/* The access point's beacon, its elements kept whole: the supplicant holds message 3/4's against them. */
#define BEACON_WAIT_US 1000000u /* ten beacon intervals of the usual 102.4 ms */

static int beacon_read(const struct buf *b, void *arg)
{
    struct mgmt_beacon m;
    (void)arg;
    if (!mgmt_beacon(b->frame, b->len, 0, &m) || m.probe_response || memcmp(m.bssid, sta.ap, 6) != 0) {
        return 0;
    }
    sta.ap_rsn_len = m.rsn ? 2u + m.rsn[1] : 0;
    sta.ap_rsnx_len = m.rsnx ? 2u + m.rsnx[1] : 0;
    if (m.rsn) {
        memcpy(sta.ap_rsn, m.rsn, sta.ap_rsn_len);
    }
    if (m.rsnx) {
        memcpy(sta.ap_rsnx, m.rsnx, sta.ap_rsnx_len);
    }
    return 1;
}

/*
 * What the station associates with: the access point's elements, read from its beacon, the suites the supplicant
 * takes of them, and the station's own elements, which the supplicant writes, deriving the passphrase's PMK.
 */
static void security(const struct drv *d, const struct drv_net *ap)
{
    sta.beacon_wanted = 1;
    int read = wait_for(osi_now_us() + BEACON_WAIT_US, beacon_read, 0);
    sta.beacon_wanted = 0;
    must("the access point's beacon", read);
    must("the network's RSN element", sta.ap_rsn_len != 0);
    must("a passphrase", d->pass[0] != 0);
    struct supp_network n = {
        .bssid = sta.ap,
        .ssid = (const uint8_t *)ap->ssid,
        .ssid_len = strlen(ap->ssid),
        .pass = d->pass,
        .ap_rsn = sta.ap_rsn,
        .ap_rsnx = sta.ap_rsnx_len ? sta.ap_rsnx : 0,
    };
    must("the suites the station takes", supp_choose(&n) == 0);
    size_t rsn_len = sizeof(sta.rsn), rsnx_len = sizeof(sta.rsnx);
    must("the station's RSN element", supp_connect(&n, sta.rsn, &rsn_len, sta.rsnx, &rsnx_len) == 0);
    sta.has_rsnx = rsnx_len != 0;
}

/*
 * A management frame sent by mac_tx, and the access point's answer read; one lost to the air, or answered too late,
 * is sent again a few times, as a station does. 1 once read takes the answer, 0 if none came.
 */
#define TRIES   3u
#define WAIT_US 200000u

static int exchange(const char *what, const uint8_t *frame, uint32_t n, int (*read)(const struct buf *b, void *arg),
                    void *arg)
{
    for (uint32_t tries = 1; tries <= TRIES; tries++) {
        const char *failed = mac_tx(frame, n);
        if (failed) {
            drv_say("sta: %s, try %u: %s\n", what, (unsigned)tries, failed);
        } else if (wait_for(osi_now_us() + WAIT_US, read, arg)) {
            return 1;
        }
    }
    return 0;
}

static int auth_answer(const struct buf *b, void *arg)
{
    return mgmt_auth_answer(b->frame, b->len, drv_self->mac, sta.ap, arg);
}

/* An open-system authentication; one refused fails with its status, one never answered with ESP_FAIL. */
static void authenticate(void)
{
    static uint8_t frame[MGMT_FRAME_MAX];
    uint32_t n = mgmt_auth_request(frame, sizeof(frame), drv_self->mac, sta.ap);
    uint16_t status = 0;
    int answered = exchange("the authentication", frame, n, auth_answer, &status);
    drv_say("sta: authentication %s, status %u\n", answered ? (status == 0 ? "accepted" : "refused") : "not answered",
            (unsigned)status);
    if (!answered || status != 0) {
        fail("the authentication answered", answered ? status : (uint32_t)ESP_FAIL);
    }
    sta.state = AUTHENTICATED;
}

struct assoc {
    uint16_t status, aid;
};

static int assoc_answer(const struct buf *b, void *arg)
{
    struct assoc *a = arg;
    return mgmt_assoc_answer(b->frame, b->len, drv_self->mac, sta.ap, &a->status, &a->aid);
}

/* The association, with the supplicant's elements; one refused fails with its status, one unanswered with ESP_FAIL. */
static void associate(const struct drv_net *ap)
{
    static uint8_t frame[MGMT_ASSOC_MAX];
    uint32_t n = mgmt_assoc_request(frame, sizeof(frame), drv_self->mac, sta.ap, ap->ssid, sta.rsn,
                                    sta.has_rsnx ? sta.rsnx : 0);
    must("the association request", n != 0);
    struct assoc a = { 0, 0 };
    int answered = exchange("the association", frame, n, assoc_answer, &a);
    drv_say("sta: association %s, status %u, AID %u\n",
            answered ? (a.status == 0 ? "accepted" : "refused") : "not answered", (unsigned)a.status, (unsigned)a.aid);
    if (!answered || a.status != 0) {
        fail("the association answered", answered ? a.status : (uint32_t)ESP_FAIL);
    }
    sta.state = ASSOCIATED;
    supp_associated(sta.ap);
}

/*
 * The access point's EAPOL frames handed to the supplicant, which answers each by the link,
 * until its handshakes are done: the 4-way handshake, whose message 3/4 carries the group's key too.
 * An access point sends a message again about a second later if it hears no answer,
 * and after a few lets the station go.
 */
#define HANDSHAKE_US 8000000u

/* An EAPOL frame from the access point, handed to the supplicant; no other frame is read, nor a protected one. */
static void eapol(const struct buf *b)
{
    struct mgmt_payload p;
    if (mgmt_data_read(b->frame, b->len, drv_self->mac, sta.ap, &p) && p.proto == SUPP_EAPOL) {
        sta.eapol_heard++;
        supp_rx_eapol(p.src, p.body, p.len);
    }
}

static int until_completed(const struct buf *b, void *arg)
{
    (void)arg;
    eapol(b);
    return sta.completed;
}

static void handshake(void)
{
    uint64_t start = osi_now_us();
    int done = wait_for(start + HANDSHAKE_US, until_completed, 0);
    drv_say("sta: the handshakes %s in %u ms\n", done ? "done" : "not done",
            (unsigned)((osi_now_us() - start) / 1000u));
    must("the handshakes done", done);
}

/* --- The data's path, once the handshake has installed the keys. --- */

/* An Ethernet frame from the network process, built into a data frame, encrypted under the pairwise key and sent. */
static void eth_send(struct drv *d, const uint8_t *eth, uint32_t len)
{
    if (len < 14u || len > LINK_SLOT) {
        d->tx_dropped++;
        return;
    }
    if (d->debug & DRV_DEBUG_FRAMES) {
        drv_trace("out", eth, len);
    }
    /*
     * A station's frame is addressed to the access point, so 802.11 sends it under the pairwise key
     * whatever its destination, Address 3; an access point picks the key by Address 1, as mac80211 does,
     * and would drop a broadcast sent under the group key.
     */
    struct sta_key *key = &sta.ptk;
    if (!key->set) {
        d->tx_dropped++;
        return;
    }
    uint16_t proto = (uint16_t)eth[12] << 8 | eth[13];
    uint32_t n = mgmt_data(sta.tx_plain, MGMT_DATA_FIXED + LINK_SLOT, d->mac, sta.ap, eth, proto, eth + 14, len - 14u);
    if (n == 0) {
        d->tx_dropped++;
        return;
    }
    uint32_t cn = ccmp_encrypt(sta.tx_crypt, MGMT_DATA_FIXED + LINK_SLOT + CCMP_HEAD_LEN + CCMP_MIC_LEN, sta.tx_plain, n,
                               key->tk, ++key->tx_pn, key->id);
    if (cn == 0) {
        d->tx_dropped++;
        return;
    }
    const char *failed = mac_tx(sta.tx_crypt, cn);
    if (failed) {
        d->tx_dropped++;
        drv_say("sta: a frame out, %u bytes: %s\n", (unsigned)len, failed);
    } else {
        d->tx_frames++;
    }
}

void sta_link_take(struct drv *d)
{
    uint32_t len;
    const uint8_t *eth;
    while ((eth = chan_get_begin(&d->link, &len)) != 0) {
        if (sta.state != NONE) {
            eth_send(d, eth, len);
        }
        chan_get_end(&d->link);
    }
}

#define FC1_PROTECTED 0x40u     /* the Protected bit, in the Frame Control's second octet */
#define ADDR1         4u        /* where a frame's Address 1, its receiver, lies */
#define SERVE_US      60000000u /* how long each of the serving's waits lasts; any length would do */

/*
 * A frame the station received once it serves the link, wait_for's reader, which never takes one:
 * decrypted once and handed to the supplicant if it is EAPOL, else built into an Ethernet frame for the network process.
 * A data frame left in the clear once the pairwise key is set is not the access point's, and is dropped;
 * so is the station's own group frame, which the access point sends the group again, counted.
 */
static int read_frame(const struct buf *b, void *arg)
{
    struct drv *d = arg;
    const uint8_t *frame = b->frame;
    uint32_t len = b->len;
    struct mgmt_payload p;

    int encrypted = (frame[1] & FC1_PROTECTED) != 0;
    int to_group = (frame[ADDR1] & 0x01) != 0;
    if (encrypted) {
        struct sta_key *key = to_group ? &sta.gtk : &sta.ptk; /* the receiver's address picks the key */
        uint64_t pn;
        uint8_t id;
        if (!key->set || !ccmp_decrypt(sta.rx_plain, STA_FRAME_MAX, frame, len, key->tk, &pn, &id, &len) ||
            id != key->id || pn <= key->rx_pn) {
            d->rx_dropped++;
            return 0;
        }
        key->rx_pn = pn;
        frame = sta.rx_plain;
    }
    if (!mgmt_data_read(frame, len, d->mac, sta.ap, &p)) {
        return 0;
    }
    if (p.proto == SUPP_EAPOL) {
        sta.eapol_heard++;
        supp_rx_eapol(p.src, p.body, p.len);
        return 0;
    }
    if (!encrypted && sta.ptk.set) {
        d->rx_dropped++;
        return 0;
    }
    if (to_group && memcmp(p.src, d->mac, 6) == 0) {
        sta.relayed++;
        return 0;
    }
    if (p.len > LINK_SLOT - 14u) {
        d->rx_dropped++;
        return 0;
    }
    memcpy(sta.eth, frame + ADDR1, 6); /* the destination */
    memcpy(sta.eth + 6, p.src, 6);     /* the source, the frame's Address 3 */
    sta.eth[12] = (uint8_t)(p.proto >> 8);
    sta.eth[13] = (uint8_t)p.proto;
    memcpy(sta.eth + 14, p.body, p.len);
    if (d->debug & DRV_DEBUG_FRAMES) {
        drv_trace("in", sta.eth, 14u + p.len);
    }
    if (chan_send(&d->link, sta.eth, 14u + p.len) == 0) {
        d->rx_frames++;
    } else {
        d->rx_dropped++;
    }
    return 0;
}

/* The station's service of the link, each frame read, until the root task asks it to leave; see wait_for. */
static __attribute__((noreturn)) void serve_station(struct drv *d)
{
    for (;;) {
        wait_for(osi_now_us() + SERVE_US, read_frame, d);
    }
}

/*
 * The station's thread: the scan, the access point's channel, the receiving the station's, the access point's beacon,
 * the authentication, the association, the supplicant's handshakes, and then the link's service,
 * reporting DRV_JOINED so that the root task starts the network process; see TODO.md.
 */
static void sta_main(void *arg)
{
    struct drv *d = arg;
    sta_scan(d);
    const struct drv_net *ap = net_for(d);
    must("the network to join", ap != 0);
    memcpy(sta.ap, ap->bssid, 6);
    drv_say("sta: %s at %02x:%02x:%02x:%02x:%02x:%02x, channel %u\n", ap->ssid, ap->bssid[0], ap->bssid[1],
            ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5], (unsigned)ap->channel);
    /* The channel a beacon named, which mac_channel may refuse, tuned before the MAC is the driver's. */
    drv_must_mac("the access point's channel", mac_channel(ap->channel));
    /* The station's data frames, unicast to it or to the group, which the scan's management filter leaves out. */
    drv_must_mac("every data frame heard", mac_hear(MAC_HEAR_MGMT | MAC_HEAR_DATA));
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(heard));
    security(d, ap);
    authenticate();
    associate(ap);
    handshake();
    d->joined = *ap;
    d->authmode = WIFI_AUTH_WPA2_PSK;
    child_report(&d->c, DRV_JOINED);
    drv_say("sta: joined, and the link is served\n");
    serve_station(d);
}

const char *sta_start(struct drv *d)
{
    sta.bufs = osi_calloc(STA_FRAMES, sizeof(struct buf));
    sta.events = osi_queue_new(STA_FRAMES + STA_RUNS + 1u, sizeof(struct event));
    sta.free = osi_queue_new(STA_FRAMES, sizeof(uint32_t));
    sta.eth = osi_malloc(LINK_SLOT);
    sta.tx_plain = osi_malloc(MGMT_DATA_FIXED + LINK_SLOT);
    sta.tx_crypt = osi_malloc(MGMT_DATA_FIXED + LINK_SLOT + CCMP_HEAD_LEN + CCMP_MIC_LEN);
    sta.rx_plain = osi_malloc(STA_FRAME_MAX);
    if (sta.bufs == 0 || sta.events == 0 || sta.free == 0 || sta.eth == 0 || sta.tx_plain == 0 || sta.tx_crypt == 0 ||
        sta.rx_plain == 0) {
        return "the station's buffers and queues";
    }
    for (uint32_t i = 0; i < STA_FRAMES; i++) {
        osi_queue_put(sta.free, &i, 0);
    }
    if (supp_init(&link, d->mac) != 0) {
        return "the supplicant";
    }
    return osi_thread(sta_main, d, "station", STA_STACK) ? 0 : "the station's thread";
}
