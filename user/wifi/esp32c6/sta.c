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
 * the EAPOL frames it sends go out as data frames by mac_tx, under the pairwise key once the handshake
 * has installed one and in the clear until then, and the keys it derives are kept for the station's CCMP,
 * under which it then serves the link.
 */

#include <stdint.h>

#include "ccmp.h"
#include "keys.h"
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
#define STA_KEY_ENTRY 4u    /* the MAC's key entry the pairwise key takes, as the libraries' association left it */
#define STA_GRP_ENTRY 1u    /* the group key's entry, which the access point's frames to every station use */

enum { EV_FRAME, EV_RUN, EV_LEAVE };

struct event {
    uint32_t kind;
    uint32_t buf;         /* EV_FRAME's buffer */
    int (*fn)(void *arg); /* EV_RUN's work, and its argument */
    void *arg;
};

struct buf {
    uint32_t len;
    uint8_t decrypted; /* the MAC's cipher decrypted the frame; see read_frame */
    uint8_t frame[STA_FRAME_MAX];
};

/* How far the station has come with its access point, which says what leaving it undoes. */
enum { NONE, AUTHENTICATED, ASSOCIATED };

/*
 * A key's packet numbers, the pairwise key's and the group's: one for sending, and a replay counter a priority,
 * the last received of that priority, which a frame's must exceed; see ccmp_replay and ccmp_priority.
 * So a frame of one access category overtaken by a later-numbered one of another is not a replay.
 */
struct key_pn {
    uint64_t tx_pn;
    uint64_t rx_pn[CCMP_REPLAY_COUNT];
};

/* The group key: its bytes and id, and its counters. It is never staged, and the station sends under the pairwise one. */
struct group_key {
    volatile int set; /* written by the station's thread, read by the receiving's once it serves */
    uint8_t id;
    uint8_t tk[CCMP_TK_LEN];
    uint64_t rx_pn[CCMP_REPLAY_COUNT];
};

static struct {
    struct queue *events, *free; /* the free queue holds the numbers of the buffers free */
    struct buf *bufs;
    uint8_t ap[6]; /* the access point chosen, which the receiving reads once the station has taken it */
    uint16_t aid;  /* the association's, which the MAC's station-mode receiving names */
    volatile uint32_t beacon_wanted; /* the receiving keeps what the access point sends every station too */
    volatile uint32_t kept, dropped, runs_lost;
    /* The receiving's drops: copies of a frame already read, and the rest, for the log; see read_frame. */
    volatile uint32_t drop_copy, drop_other;
    /* The receiving's data frames by path: the MAC's cipher, and the clear; see read_frame. */
    volatile uint32_t rx_mac, rx_mac_grp, rx_clear;
    volatile uint32_t state; /* the station's thread writes it, the first thread reads it to send the link's frames */
    /* The access point's RSN and RSNX elements, whole, from its beacon, and the station's, from the supplicant. */
    uint8_t ap_rsn[2 + 255], ap_rsnx[2 + 255];
    uint32_t ap_rsn_len, ap_rsnx_len;
    uint8_t rsn[80], rsnx[8];
    int has_rsnx;
    int completed; /* the supplicant's handshakes are done */
    uint32_t eapol_heard, eapol_sent, eapol_crypt; /* the last of the supplicant's frames sent under the pairwise key */
    uint32_t relayed; /* the station's own group frames, back from the access point */
    /* The keys, and the frames CCMP works between. */
    struct keys ptk;      /* the pairwise key in use and the one staged; see keys.h */
    struct key_pn ptk_pn; /* the pairwise key in use's packet numbers */
    struct group_key gtk; /* the group key and its counters */
    struct mutex *key_lock; /* held while a key is installed, and by the sending while it uses one */
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
    b->decrypted = (uint8_t)(m->decrypted != 0);
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
    /* Two lines, as drv_say's line holds 160 bytes, and a third for which receiving path each frame took. */
    drv_say("sta: frames to the station kept %u, dropped %u; read and dropped: copies %u, other %u; "
            "EAPOL heard %u, sent %u, %u under the key; the supplicant's work lost %u\n",
            (unsigned)sta.kept, (unsigned)sta.dropped, (unsigned)sta.drop_copy, (unsigned)sta.drop_other,
            (unsigned)sta.eapol_heard, (unsigned)sta.eapol_sent, (unsigned)sta.eapol_crypt,
            (unsigned)sta.runs_lost);
    drv_say("sta: the link's frames in %u, out %u, its own back from the access point %u; "
            "the stack's deepest %u bytes of %u\n",
            (unsigned)drv_self->rx_frames, (unsigned)drv_self->tx_frames, (unsigned)sta.relayed,
            (unsigned)osi_stack_used(osi_self()), (unsigned)STA_STACK);
    drv_say("sta: the data frames through the cipher: by the MAC %u (of them group %u), in the clear %u\n",
            (unsigned)sta.rx_mac, (unsigned)sta.rx_mac_grp, (unsigned)sta.rx_clear);
    drv_mac_told("sta");
}

/* The station fails at step: it leaves the access point, gives the receiving back, and stops as drv_must does. */
/* The access point told, the MAC given back to the libraries, and what the station counted, into the log. */
static void hand_back(void)
{
    part(MGMT_LEAVING);
    mac_addr_restore();
    mac_key_clear(STA_KEY_ENTRY); /* the libraries' stop meets no entry of the own path's */
    mac_key_clear(STA_GRP_ENTRY);
    mac_rx_give_back();
    told();
}

static __attribute__((noreturn)) void fail(const char *step, uint32_t detail)
{
    hand_back();
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
    hand_back();
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

/* Defined below; the supplicant's frames take the pairwise key's path to the cipher too. */
static uint32_t protect(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n);

/*
 * A frame of the supplicant's, of Ethernet type proto, to dest through the access point: 0, or -1 if not sent.
 * Under the pairwise key once the handshake installed one, and in the clear until then, as the standard asks;
 * in a buffer of its own, since the station's thread sends it and the first thread sends the link's data.
 */
static int link_send(const uint8_t *dest, uint16_t proto, const uint8_t *buf, size_t len)
{
    static uint8_t plain[MGMT_DATA_FIXED + EAPOL_MAX];
    static uint8_t crypt[MGMT_DATA_FIXED + EAPOL_MAX + CCMP_HEAD_LEN + CCMP_MIC_LEN];
    uint32_t n = mgmt_data(plain, sizeof(plain), drv_self->mac, sta.ap, dest, proto, buf, (uint32_t)len);
    if (n == 0) {
        drv_say("sta: a frame of the supplicant's, %u bytes: longer than the station sends\n", (unsigned)len);
        return -1;
    }
    uint32_t cn = protect(crypt, sizeof(crypt), plain, n);
    const char *failed = mac_tx(cn ? crypt : plain, cn ? cn : n);
    if (failed) {
        drv_say("sta: a frame of the supplicant's, %u bytes: %s\n", (unsigned)len, failed);
        return -1;
    }
    sta.eapol_sent++;
    if (cn != 0) {
        sta.eapol_crypt++;
    }
    return 0;
}

/* The receive sequence counter the handshake gives a key, little-endian as the CCMP header's packet number. */
static uint64_t key_rsc(const struct supp_key *k)
{
    uint64_t rsc = 0;
    for (uint32_t i = 0; k->seq != 0 && i < k->seq_len && i < 6u; i++) {
        rsc |= (uint64_t)k->seq[i] << (8u * i);
    }
    return rsc;
}

/*
 * A group key the supplicant derived, kept as it is, its counters from the handshake's receive sequence counter.
 * The access point sends under the new key from the switch on; keeping the old one until it does is E, see TODO.md.
 */
static int link_set_group(const struct supp_key *k)
{
    if (k->alg == SUPP_ALG_NONE) {
        osi_mutex_take(sta.key_lock);
        sta.gtk.set = 0;
        mac_key_clear(STA_GRP_ENTRY);
        osi_mutex_give(sta.key_lock);
        drv_say("sta: the group's key removed\n");
        return 0;
    }
    if (k->alg != SUPP_ALG_CCMP || k->key == 0 || k->key_len != CCMP_TK_LEN) {
        drv_say("sta: a key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
    osi_mutex_take(sta.key_lock);
    if (sta.gtk.set && sta.gtk.id == (uint8_t)(k->idx & 3) && memcmp(sta.gtk.tk, k->key, CCMP_TK_LEN) == 0) {
        /* A message that comes again with the key already in use: installing it afresh would take the counters back. */
        osi_mutex_give(sta.key_lock);
        drv_say("sta: the group's key installed again, the counters kept\n");
        return 0;
    }
    memcpy(sta.gtk.tk, k->key, CCMP_TK_LEN);
    sta.gtk.id = (uint8_t)(k->idx & 3);
    /* Every priority starts from it: a group key's RSC bounds the frames sent before the join, whatever priority. */
    ccmp_replay_start(sta.gtk.rx_pn, key_rsc(k));
    sta.gtk.set = 1;
    /* The MAC's own entry, so that its receiving takes a protected group frame to the station. */
    mac_key_set(STA_GRP_ENTRY, sta.ap, sta.gtk.id, sta.gtk.tk);
    osi_mutex_give(sta.key_lock);
    drv_say("sta: the group's key installed, index %d\n", sta.gtk.id);
    return 0;
}

/*
 * A pairwise key the supplicant derived. The handshake installs the key it derives for receiving alone before
 * the 4/4 and for sending too after, so the station stages the first and promotes it at the second; the 4/4 goes
 * under the key in use, the old one on a rekey and none on the first join. One lock guards the key and its
 * counters, shared with the sending thread; keys.h and keys-test.c hold the transitions.
 */
static int link_set_pairwise(const struct supp_key *k)
{
    if (k->flag & SUPP_KEY_MODIFY) {
        drv_say("sta: the pairwise key activated for sending, which the station does not do: "
                "it negotiates no extended key ID\n");
        return 0;
    }
    if (k->alg == SUPP_ALG_NONE) {
        osi_mutex_take(sta.key_lock);
        sta.ptk.cur_set = 0;
        sta.ptk.next_set = 0;
        mac_key_clear(STA_KEY_ENTRY);
        osi_mutex_give(sta.key_lock);
        drv_say("sta: the pairwise key removed\n");
        return 0;
    }
    if (k->alg != SUPP_ALG_CCMP || k->key == 0 || k->key_len != CCMP_TK_LEN) {
        drv_say("sta: a key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
    struct key_value in = { 0 };
    in.id = (uint8_t)(k->idx & 3);
    memcpy(in.tk, k->key, CCMP_TK_LEN);
    osi_mutex_take(sta.key_lock);
    enum key_action a = keys_apply(&sta.ptk, &in, (k->flag & SUPP_KEY_TX) != 0, (k->flag & SUPP_KEY_NEXT) != 0);
    if (a == KEY_PROMOTE || a == KEY_INSTALL) {
        sta.ptk_pn.tx_pn = 0;
        ccmp_replay_start(sta.ptk_pn.rx_pn, key_rsc(k));
        /* The MAC's own entry, so that its receiving takes a protected unicast frame to the station. */
        mac_key_set(STA_KEY_ENTRY, sta.ap, sta.ptk.cur.id, sta.ptk.cur.tk);
    }
    osi_mutex_give(sta.key_lock);
    switch (a) {
    case KEY_STAGE:
        drv_say("sta: the pairwise key staged for receiving, index %d\n", sta.ptk.next.id);
        break;
    case KEY_PROMOTE:
        drv_say("sta: the pairwise key now sends and receives, index %d\n", sta.ptk.cur.id);
        break;
    case KEY_INSTALL:
        drv_say("sta: the pairwise key installed, index %d\n", sta.ptk.cur.id);
        break;
    case KEY_AGAIN:
        drv_say("sta: the pairwise key installed again, the counters kept\n");
        break;
    case KEY_REFUSE:
        drv_say("sta: the pairwise key for receiving alone refused: the station negotiates no extended key ID\n");
        break;
    }
    return 0;
}

static int link_set_key(const struct supp_key *k)
{
    if (k->flag & SUPP_KEY_GROUP) {
        return link_set_group(k);
    }
    return link_set_pairwise(k);
}

#define REKEY_AFTER_S 1 /* the debug=rekey run's wait before it asks the access point for a new pairwise key */

static void link_completed(void)
{
    sta.completed = 1;
    if (drv_self->debug & DRV_DEBUG_REKEY) {
        supp_rekey(REKEY_AFTER_S);
    }
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
    sta.aid = a.aid;
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

#define FC1_PROTECTED 0x40u /* the Protected bit, in the Frame Control's second octet */
#define ADDR1         4u    /* where a frame's Address 1, its receiver, lies */

/*
 * The frame b carries, read once: the plaintext at *frame with its length at *len, and ccmp_frame_kind's verdict
 * on the frame's own Protected bit and the MAC's flag.
 * A protected frame the cipher took has its CCMP header's packet number and key id checked against the key's
 * replay counter, the header then taken away and the bit cleared; the receiver's address picks the key, Address 1,
 * the group's for a group address. *frame is 0, and the drop counted, if the frame is not the cipher's,
 * has no key, no CCMP header, another key id, or a packet number not above the counter.
 */
static int plain_frame(const struct buf *b, struct drv *d, const uint8_t **frame, uint32_t *len)
{
    const uint8_t *f = b->frame;
    *frame = 0;
    *len = b->len;
    int kind = ccmp_frame_kind(f, b->decrypted);
    if (kind == CCMP_FRAME_CLEAR) {
        sta.rx_clear++;
        *frame = f;
        return kind;
    }
    if (kind == CCMP_FRAME_DROP) {
        sta.drop_other++;
        d->rx_dropped++;
        return kind;
    }
    /*
     * The MAC's cipher decrypted it and left its CCMP header, whose packet number and key id are checked
     * as the sending's own counter is; the payload stands in the clear, so the cipher is passed over.
     */
    int to_group = (f[ADDR1] & 0x01) != 0;
    int set = to_group ? sta.gtk.set : sta.ptk.cur_set;
    uint8_t key_id = to_group ? sta.gtk.id : sta.ptk.cur.id;
    uint64_t *rx_pn = to_group ? sta.gtk.rx_pn : sta.ptk_pn.rx_pn;
    uint32_t hl = ccmp_header_len(f); /* 0 for anything but a Data frame, as a protected management one is */
    uint64_t pn = 0;
    uint8_t id = 0;
    if (!set || hl == 0 || *len < hl + CCMP_HEAD_LEN + 1u || !ccmp_head_read(f + hl, &pn, &id) ||
        id != key_id) {
        sta.drop_other++;
        d->rx_dropped++;
        return kind;
    }
    int replay = ccmp_replay(pn, ccmp_priority(f), rx_pn);
    if (replay != CCMP_REPLAY_TAKEN) {
        if (replay == CCMP_REPLAY_COPY) {
            sta.drop_copy++; /* the same frame again: the access point sent it once more */
        } else {
            sta.drop_other++;
        }
        d->rx_dropped++;
        return kind;
    }
    sta.rx_mac++;
    if (to_group) {
        sta.rx_mac_grp++;
    }
    memcpy(sta.rx_plain, f, hl); /* the CCMP header taken away, the header and the payload joined */
    memmove(sta.rx_plain + hl, f + hl + CCMP_HEAD_LEN, *len - hl - CCMP_HEAD_LEN);
    sta.rx_plain[1] &= (uint8_t)~FC1_PROTECTED; /* the MAC decrypted it: the frame goes on in the clear */
    *len -= CCMP_HEAD_LEN;
    *frame = sta.rx_plain;
    return kind;
}

/* An EAPOL frame from the access point, handed to the supplicant, through the one reading of a frame above. */
static void eapol(const struct buf *b)
{
    struct mgmt_payload p;
    const uint8_t *frame;
    uint32_t len;
    plain_frame(b, drv_self, &frame, &len);
    if (frame != 0 && mgmt_data_read(frame, len, drv_self->mac, sta.ap, &p) && p.proto == SUPP_EAPOL) {
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

/*
 * n bytes at plain, a data frame, protected under the pairwise key in use: the lock copies the key and reserves
 * the packet number, so that no two frames go with one key and packet number and the encryption runs outside it.
 * The lock rather than a `set` written last, because the compiler may move the key's stores past that flag,
 * and tx_pn is two 32-bit stores on RV32, which a rekey and this reservation would tear.
 * 0 if no pairwise key is in use, or the frame does not fit, else the frame's length at crypt.
 */
static uint32_t protect(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n)
{
    uint8_t tk[CCMP_TK_LEN];
    uint8_t id;
    uint64_t pn;
    osi_mutex_take(sta.key_lock);
    if (!sta.ptk.cur_set) {
        osi_mutex_give(sta.key_lock);
        return 0;
    }
    memcpy(tk, sta.ptk.cur.tk, CCMP_TK_LEN);
    id = sta.ptk.cur.id;
    pn = ++sta.ptk_pn.tx_pn;
    osi_mutex_give(sta.key_lock);
    return ccmp_encrypt(crypt, size, plain, n, tk, pn, id);
}

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
    uint16_t proto = (uint16_t)eth[12] << 8 | eth[13];
    uint32_t n = mgmt_data(sta.tx_plain, MGMT_DATA_FIXED + LINK_SLOT, d->mac, sta.ap, eth, proto, eth + 14, len - 14u);
    uint32_t cn = n ? protect(sta.tx_crypt, MGMT_DATA_FIXED + LINK_SLOT + CCMP_HEAD_LEN + CCMP_MIC_LEN, sta.tx_plain, n)
                    : 0;
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

#define SERVE_US 60000000u /* how long each of the serving's waits lasts; any length would do */

/*
 * A frame the station received once it serves the link, wait_for's reader, which never takes one:
 * read once through plain_frame, and handed to the supplicant if it is EAPOL,
 * else built into an Ethernet frame for the network process. A data frame left in the clear
 * once the pairwise key is set is not the access point's, and is dropped;
 * so is the station's own group frame, which the access point sends the group again, counted.
 */
static int read_frame(const struct buf *b, void *arg)
{
    struct drv *d = arg;
    struct mgmt_payload p;
    const uint8_t *frame;
    uint32_t len;
    int kind = plain_frame(b, d, &frame, &len);
    if (frame == 0) {
        return 0;
    }
    if (!mgmt_data_read(frame, len, d->mac, sta.ap, &p)) {
        return 0;
    }
    if (p.proto == SUPP_EAPOL) {
        sta.eapol_heard++;
        supp_rx_eapol(p.src, p.body, p.len);
        return 0;
    }
    if (kind == CCMP_FRAME_CLEAR && sta.ptk.cur_set) {
        sta.drop_other++;
        d->rx_dropped++;
        return 0;
    }
    if ((frame[ADDR1] & 0x01) != 0 && memcmp(p.src, d->mac, 6) == 0) {
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
    /*
     * The station's own address and the access point's, set before the authentication, and the station-mode
     * receiving the libraries' association programs; the scan's channel sniffer is left and the MAC's list taken.
     * See mac.c.
     */
    mac_station(drv_self->mac, sta.ap);
    mac_receive(0);
    mac_sniffer(0);
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(heard));
    security(d, ap);
    authenticate();
    associate(ap);
    /* The association's AID, the receiving's own; see mac.c. */
    mac_receive(sta.aid);
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
    sta.key_lock = osi_mutex_new();
    if (sta.bufs == 0 || sta.events == 0 || sta.free == 0 || sta.eth == 0 || sta.tx_plain == 0 || sta.tx_crypt == 0 ||
        sta.rx_plain == 0 || sta.key_lock == 0) {
        return "the station's buffers, queues and lock";
    }
    for (uint32_t i = 0; i < STA_FRAMES; i++) {
        osi_queue_put(sta.free, &i, 0);
    }
    if (supp_init(&link, d->mac) != 0) {
        return "the supplicant";
    }
    return osi_thread(sta_main, d, "station", STA_STACK) ? 0 : "the station's thread";
}
