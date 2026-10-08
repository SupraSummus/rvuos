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
 * under WPA3's personal it makes and checks SAE's messages, which the authentication carries, see sae;
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
#define STA_STACK     4096u /* room for the supplicant's handshakes and SAE, whose depth told() says */
#define STA_KEY_ENTRY 4u    /* the MAC's key entry the pairwise key takes, as the libraries' association left it */
#define STA_GRP_ENTRY 0u    /* the MAC's entry the group key of id 1 takes, and the next id 2's, as the libraries' */
#define FC1_PROTECTED 0x40u /* the Protected bit, in the Frame Control's second octet */
#define ADDR1         4u    /* where a frame's Address 1, its receiver, lies */

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
    int pmf;       /* management frames protected, from the supplicant's choice; see part */
    int sae;       /* WPA3's personal, SAE the authentication, from the supplicant's choice */
    uint32_t eapol_heard, eapol_sent, eapol_crypt; /* the last of the supplicant's frames sent under the pairwise key */
    uint32_t relayed; /* the station's own group frames, back from the access point */
    uint32_t let_go_ignored; /* a deauthentication or disassociation ignored under PMF, unforged by the check */
    uint32_t sa_query_sent;  /* SA Query requests the station sent, checking whether the access point still holds it */
    /* The SA Query in progress: its transaction id, what the access point said as it left, and its times. */
    struct {
        int active;
        uint16_t id, reason;
        uint64_t start, last, at;
    } sa_query;
    /* The keys, and the frames CCMP works between. */
    struct keys ptk;      /* the pairwise key in use and the one staged; see keys.h */
    struct key_pn ptk_pn; /* the pairwise key in use's packet numbers */
    /* The group keys, a slot an id (see keys.h), and each slot's counters: the group key's, and the IGTK's for BIP. */
    struct group_keys gtk, igtk;
    uint64_t gtk_rx_pn[KEYS_GROUP][CCMP_REPLAY_COUNT];
    uint64_t igtk_ipn[KEYS_GROUP];
    struct mutex *key_lock; /* held while a key is installed, and by the sending while it uses one */
    uint8_t *eth, *tx_plain, *tx_crypt, *rx_plain;
} sta;

static uint32_t protect(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n);
static uint32_t protect_mgmt(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n);

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
    mac_sniffer(1);
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(scan_heard));
    for (uint32_t ch = MAC_CHANNEL_FIRST; ch <= MAC_CHANNEL_LAST; ch++) {
        mac_channel(ch); /* one of those it tunes to, so never refused */
        osi_delay_ms(SCAN_DWELL_MS);
    }
    mac_sniffer(0);
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
    int group = (sta.gtk.set[0] || sta.gtk.set[1]) && mgmt_to_group(m->frame, m->len, sta.ap);
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
 * Under PMF it goes protected under the pairwise key: an access point ignores an unprotected one, and would
 * keep the station's state, so the next association would be told to come back.
 */
static void part(uint16_t reason)
{
    static uint8_t frame[MGMT_FRAME_MAX];
    static uint8_t crypt[MGMT_FRAME_MAX + CCMP_HEAD_LEN + CCMP_MIC_LEN];
    if (forget()) {
        uint32_t n = mgmt_deauth(frame, sizeof(frame), drv_self->mac, sta.ap, reason);
        uint32_t cn = sta.pmf ? protect_mgmt(crypt, sizeof(crypt), frame, n) : 0;
        const char *failed = mac_tx(cn ? crypt : frame, cn ? cn : n);
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
            "the stack's deepest %u bytes of %u, the heap's fewest free %u\n",
            (unsigned)drv_self->rx_frames, (unsigned)drv_self->tx_frames, (unsigned)sta.relayed,
            (unsigned)osi_stack_used(osi_self()), (unsigned)STA_STACK, (unsigned)osi_heap_least());
    drv_say("sta: the data frames through the cipher: by the MAC %u (of them group %u), in the clear %u; "
            "deauthentications ignored %u, SA Queries sent %u\n",
            (unsigned)sta.rx_mac, (unsigned)sta.rx_mac_grp, (unsigned)sta.rx_clear,
            (unsigned)sta.let_go_ignored, (unsigned)sta.sa_query_sent);
    drv_mac_told("sta");
}

/* The station fails at step: it leaves the access point, gives the receiving back, and stops as drv_must does. */
/* The access point told, the MAC given back to the libraries, and what the station counted, into the log. */
static void hand_back(void)
{
    part(MGMT_LEAVING);
    mac_key_clear(STA_KEY_ENTRY); /* no entry of the own path's is left in the hardware */
    for (uint32_t i = 0; i < KEYS_GROUP; i++) {
        mac_key_clear(STA_GRP_ENTRY + i);
    }
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
 * The plaintext of a protected robust management frame at b into plain, its length into *len, decrypted under
 * the pairwise key in use: 1, or 0 if no key is in use, the MIC or the key id refuses it, or its packet number
 * is not above the counter reserved for management. The station's thread alone takes this path, and the
 * receiving's never touches that counter. The MAC's cipher is left out of it: it takes data frames.
 */
static int open_mgmt(const struct buf *b, uint8_t *plain, uint32_t *len)
{
    uint8_t tk[CCMP_TK_LEN], id;
    osi_mutex_take(sta.key_lock);
    if (!sta.ptk.cur_set) {
        osi_mutex_give(sta.key_lock);
        return 0;
    }
    memcpy(tk, sta.ptk.cur.tk, CCMP_TK_LEN);
    id = sta.ptk.cur.id;
    osi_mutex_give(sta.key_lock);
    uint64_t pn = 0;
    uint8_t kid = 0;
    if (!ccmp_decrypt_mgmt(plain, MGMT_FRAME_MAX, b->frame, b->len, tk, &pn, &kid, len) || kid != id) {
        return 0;
    }
    return ccmp_replay(pn, CCMP_REPLAY_MGMT, sta.ptk_pn.rx_pn) == CCMP_REPLAY_TAKEN;
}

/* The SA Query's answer to the access point, or the station's own request, protected under the pairwise key. */
static void sa_query_send(int response, uint16_t id)
{
    static uint8_t frame[MGMT_FRAME_MAX];
    static uint8_t crypt[MGMT_FRAME_MAX + CCMP_HEAD_LEN + CCMP_MIC_LEN];
    uint32_t n = mgmt_sa_query(frame, sizeof(frame), drv_self->mac, sta.ap, response, id);
    uint32_t cn = protect_mgmt(crypt, sizeof(crypt), frame, n);
    if (cn == 0) {
        return; /* nothing to protect it with: an unprotected SA Query is not sent under PMF */
    }
    const char *failed = mac_tx(crypt, cn);
    if (failed) {
        drv_say("sta: an SA Query %s: %s\n", response ? "answer" : "question", failed);
    } else if (!response) {
        sta.sa_query_sent++;
    }
}

/*
 * The access point said the station is gone, unprotected: ask it whether it still holds the station, one query
 * at a time; mgmt_sa_query_step moves it, and no answer in the whole time leaves the link. The transaction id
 * is random, so that a forged answer to another query is not taken for this one's.
 */
static void sa_query_start(uint16_t reason)
{
    if (sta.sa_query.active) {
        return;
    }
    drv_random((uint8_t *)&sta.sa_query.id, sizeof(sta.sa_query.id));
    sta.sa_query.active = 1;
    sta.sa_query.reason = reason;
    sta.sa_query.start = osi_now_us();
    sta.sa_query.last = sta.sa_query.start - SA_QUERY_RETRY_US; /* the first request goes at once */
    sta.sa_query.at = sta.sa_query.start;
}

/* The SA Query's next move at the time: send again, or, with no answer in the whole time, leave the link. */
static void sa_query_time(void)
{
    if (!sta.sa_query.active) {
        return;
    }
    uint64_t now = osi_now_us();
    int step = mgmt_sa_query_step(now, sta.sa_query.start, sta.sa_query.last);
    if (step == SA_QUERY_SEND) {
        sa_query_send(0, sta.sa_query.id);
        sta.sa_query.last = now;
    } else if (step == SA_QUERY_GIVE_UP) {
        sta.sa_query.active = 0;
        forget();
        fail("the access point's state", sta.sa_query.reason);
    }
    sta.sa_query.at = sta.sa_query.last + SA_QUERY_RETRY_US;
    if (sta.sa_query.start + SA_QUERY_MAX_US < sta.sa_query.at) {
        sta.sa_query.at = sta.sa_query.start + SA_QUERY_MAX_US;
    }
}

/*
 * Whether a deauthentication or disassociation from the access point ends the station, its reason into *reason.
 * Under PMF, in force once a key is in use, a unicast one is read only from the frame the pairwise key protects;
 * an unprotected one is the forgery PMF exists to stop, ignored and counted, and answered with an SA Query,
 * since the access point may instead have lost the station; a group one waits on a BIP check, see TODO.md.
 * Without PMF in force any of them ends the link, as before. See mgmt_let_go_policy.
 */
static int let_go(const struct buf *b, uint16_t *reason)
{
    int group = 0;
    if (!mgmt_let_go_kind(b->frame, b->len, drv_self->mac, sta.ap, &group)) {
        return 0;
    }
    int protected_ = (b->frame[1] & FC1_PROTECTED) != 0;
    int active = sta.pmf && sta.ptk.cur_set;
    static uint8_t plain[MGMT_FRAME_MAX];
    const uint8_t *f = b->frame;
    uint32_t n = b->len;
    int verified = 0;
    if (active && group) {
        /*
         * A group-addressed one is authenticated by BIP rather than encrypted; its reason stands in the clear.
         * Its MIC element's key id names the slot, and mgmt_bip_verify takes no frame under the other.
         */
        for (uint32_t i = 0; i < KEYS_GROUP && !verified; i++) {
            verified = sta.igtk.set[i] && mgmt_bip_verify(f, n, sta.igtk.k[i].tk, sta.igtk.k[i].id, &sta.igtk_ipn[i]);
        }
    } else if (active && protected_) {
        verified = open_mgmt(b, plain, &n);
        if (verified) {
            f = plain;
        }
    }
    if (mgmt_let_go_policy(active, group, protected_, verified) == MGMT_LET_GO_IGNORE) {
        sta.let_go_ignored++;
        if (active && !group && !protected_ && mgmt_let_go(f, n, drv_self->mac, sta.ap, reason)) {
            sa_query_start(*reason);
        }
        return 0;
    }
    return group ? mgmt_let_go_group(f, n, sta.ap, reason) : mgmt_let_go(f, n, drv_self->mac, sta.ap, reason);
}

/*
 * An SA Query from the access point, protected under the pairwise key: a request is answered with a response of
 * the same transaction id, and a response the station asked for ends the query. 1 once it took one. Only an
 * Action frame is opened, so that a protected deauthentication is let_go's alone to read.
 */
static int sa_query_take(const struct buf *b)
{
    if (!sta.pmf || !sta.ptk.cur_set || (b->frame[1] & FC1_PROTECTED) == 0 ||
        !mgmt_is_action(b->frame, b->len)) {
        return 0;
    }
    static uint8_t plain[MGMT_FRAME_MAX];
    uint32_t n = 0;
    if (!open_mgmt(b, plain, &n)) {
        return 0;
    }
    int response = 0;
    uint16_t id = 0;
    if (!mgmt_sa_query_read(plain, n, drv_self->mac, sta.ap, &response, &id)) {
        return 0; /* another action: read_frame drops it */
    }
    if (!response) {
        sa_query_send(1, id);
    } else if (sta.sa_query.active && id == sta.sa_query.id) {
        sta.sa_query.active = 0;
    }
    return 1;
}

/*
 * The events until the deadline, in osi_now_us's microseconds, each frame offered to read, given back after:
 * 1 once read takes one, the frame the step waits for, 0 at the deadline.
 * The supplicant's work runs between them; the ask to leave ends the station whatever the step,
 * and so does the access point letting it, or every station, go, once it has authenticated.
 * An SA Query in progress shortens each wait to its next move, which request again at its retry time or,
 * with no answer before its whole time, leaves the link; see sa_query_time.
 */
static int wait_for(uint64_t deadline, int (*read)(const struct buf *b, void *arg), void *arg)
{
    for (;;) {
        uint64_t now = osi_now_us();
        uint64_t until = sta.sa_query.active && sta.sa_query.at < deadline ? sta.sa_query.at : deadline;
        struct event e;
        if (now >= until || !osi_queue_get(sta.events, &e, (uint32_t)((until - now + 999u) / 1000u))) {
            if (now >= deadline) {
                return 0;
            }
            sa_query_time();
            continue;
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
        int gone = sta.state != NONE && let_go(b, &reason);
        int took = !gone && sa_query_take(b);
        int taken = !gone && !took && read(b, arg);
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

/* The key an install brings, its id and bytes; 0 if its bytes are not a temporal key's. */
static int key_value_of(const struct supp_key *k, struct key_value *v)
{
    if (k->key == 0 || k->key_len != CCMP_TK_LEN) {
        return 0;
    }
    v->id = (uint8_t)k->idx;
    memcpy(v->tk, k->key, CCMP_TK_LEN);
    return 1;
}

/* What a group key's install did, for the log. */
static const char *group_said(enum key_action a)
{
    return a == KEY_INSTALL ? "installed" : a == KEY_AGAIN ? "installed again, its counters kept" : "refused, no slot";
}

/*
 * A group key the supplicant derived, kept in the slot of its id, its counters from the handshake's receive
 * sequence counter; the other slot keeps the old key, which the access point sends under until it switches.
 * Its MAC entry is the slot's, so the MAC's receiving decrypts a protected group frame under either,
 * finding the entry by the frame's key id; see NOTES.md.
 */
static int link_set_group(const struct supp_key *k)
{
    if (k->alg == SUPP_ALG_NONE) {
        int s = keys_group_slot(KEYS_GROUP_ID, (uint32_t)k->idx);
        if (s >= 0) {
            osi_mutex_take(sta.key_lock);
            sta.gtk.set[s] = 0;
            mac_key_clear(STA_GRP_ENTRY + (uint32_t)s);
            osi_mutex_give(sta.key_lock);
            drv_say("sta: the group's key of index %d removed\n", k->idx);
        }
        return 0;
    }
    struct key_value in = { 0 };
    if (k->alg != SUPP_ALG_CCMP || !key_value_of(k, &in)) {
        drv_say("sta: a key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
    int s = keys_group_slot(KEYS_GROUP_ID, in.id);
    osi_mutex_take(sta.key_lock);
    enum key_action a = keys_group_apply(&sta.gtk, KEYS_GROUP_ID, &in);
    if (a == KEY_INSTALL) {
        /* Every priority starts from it: its RSC bounds the frames sent before the join, whatever priority. */
        ccmp_replay_start(sta.gtk_rx_pn[s], key_rsc(k));
        mac_key_set(STA_GRP_ENTRY + (uint32_t)s, sta.ap, in.id, in.tk);
    }
    osi_mutex_give(sta.key_lock);
    drv_say("sta: the group's key of index %d %s\n", in.id, group_said(a));
    return 0;
}

/*
 * The management group key (IGTK) the handshake gives under PMF, kept in the slot of its id for the BIP check of
 * a group-addressed robust management frame; its IPN is the packet number that check reads.
 * The MAC's cipher decrypts data frames alone, so the key waits in software,
 * and no run has received such a frame yet; see TODO.md.
 */
static int link_set_igtk(const struct supp_key *k)
{
    if (k->alg == SUPP_ALG_NONE) {
        int s = keys_group_slot(KEYS_MGMT_GROUP_ID, (uint32_t)k->idx);
        if (s >= 0) {
            osi_mutex_take(sta.key_lock);
            sta.igtk.set[s] = 0;
            osi_mutex_give(sta.key_lock);
            drv_say("sta: the management group key of index %d removed\n", k->idx);
        }
        return 0;
    }
    struct key_value in = { 0 };
    if (k->alg != SUPP_ALG_BIP_CMAC_128 || !key_value_of(k, &in)) {
        drv_say("sta: a management group key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
    int s = keys_group_slot(KEYS_MGMT_GROUP_ID, in.id);
    osi_mutex_take(sta.key_lock);
    enum key_action a = keys_group_apply(&sta.igtk, KEYS_MGMT_GROUP_ID, &in);
    if (a == KEY_INSTALL) {
        sta.igtk_ipn[s] = key_rsc(k);
    }
    osi_mutex_give(sta.key_lock);
    drv_say("sta: the management group key of index %d %s\n", in.id, group_said(a));
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
    struct key_value in = { 0 };
    if (k->alg != SUPP_ALG_CCMP || !key_value_of(k, &in)) {
        drv_say("sta: a key of algorithm %d, %u bytes, ignored\n", k->alg, (unsigned)k->key_len);
        return 0;
    }
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
        /* The management group key's ids are 4 and 5, the group key's 1 to 3; a removal names the id too. */
        if (k->alg == SUPP_ALG_BIP_CMAC_128 || (k->alg == SUPP_ALG_NONE && k->idx > 3)) {
            return link_set_igtk(k);
        }
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

/*
 * The next access point heard for the network from *next on, strongest first, the scan having sorted them:
 * the page's bssid, or one whose ssid is the network's; or 0 once there are no more.
 */
static const struct drv_net *net_next(const struct drv *d, uint32_t *next)
{
    while (*next < d->net_count) {
        const struct drv_net *a = &d->nets[(*next)++];
        if (d->bssid_set ? memcmp(a->bssid, d->bssid, 6) == 0 : streq(a->ssid, d->ssid)) {
            return a;
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
 * 1, or 0 if the beacon was not heard, or offers no suites the station takes, so that another access point
 * of the network may be tried.
 */
static int security(const struct drv *d, const struct drv_net *ap)
{
    sta.beacon_wanted = 1;
    int read = wait_for(osi_now_us() + BEACON_WAIT_US, beacon_read, 0);
    sta.beacon_wanted = 0;
    if (!read || sta.ap_rsn_len == 0) {
        drv_say("sta: %s\n", read ? "no RSN element in the beacon" : "no beacon heard");
        return 0;
    }
    struct supp_network n = {
        .bssid = sta.ap,
        .ssid = (const uint8_t *)ap->ssid,
        .ssid_len = strlen(ap->ssid),
        .pass = d->pass,
        .ap_rsn = sta.ap_rsn,
        .ap_rsnx = sta.ap_rsnx_len ? sta.ap_rsnx : 0,
        .pmf_ok = !d->no_pmf,
        .sae_ok = !d->no_sae,
    };
    if (supp_choose(&n) != 0) {
        return 0;
    }
    drv_say("sta: the suites the station takes\n");
    sta.pmf = n.pmf;
    sta.sae = n.key_mgmt == SUPP_KEY_MGMT_SAE;
    size_t rsn_len = sizeof(sta.rsn), rsnx_len = sizeof(sta.rsnx);
    must("the station's RSN element", supp_connect(&n, sta.rsn, &rsn_len, sta.rsnx, &rsnx_len) == 0);
    sta.has_rsnx = rsnx_len != 0;
    return 1;
}

/*
 * A management frame sent by mac_tx, and the access point's answer read; one lost to the air, or answered too late,
 * is sent again a few times, as a station does. 1 once read takes the answer, 0 if none came.
 */
#define TRIES   3u
#define WAIT_US 200000u

/* One try's sending, said in the log if it failed; 1 once mac_tx sent the frame. */
static int sent(const char *what, uint32_t tries, const uint8_t *frame, uint32_t n)
{
    const char *failed = mac_tx(frame, n);
    if (failed) {
        drv_say("sta: %s, try %u: %s\n", what, (unsigned)tries, failed);
    }
    return failed == 0;
}

static int exchange(const char *what, const uint8_t *frame, uint32_t n, int (*read)(const struct buf *b, void *arg),
                    void *arg)
{
    for (uint32_t tries = 1; tries <= TRIES; tries++) {
        if (sent(what, tries, frame, n) && wait_for(osi_now_us() + WAIT_US, read, arg)) {
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
static void open_system(void)
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
}

/*
 * WPA3's SAE as the authentication (IEEE 802.11 12.4), its messages the supplicant's: the two commits, the access
 * point's maybe asking for a token first, then the two confirms. A message unanswered goes again: the commit as it
 * was, as a new one would start over, and the confirm made anew, its counter the next, which 802.11 has an access
 * point that already accepted answer where it drops a copy.
 */
#define SAE_WAIT_US  1000000u
#define SAE_TOKENS   2u /* the commits sent again with a token the access point asked for */
/* The longest message the station sends: a commit on P-256, its group, scalar and element, and a token in its container. */
#define SAE_BODY_MAX (2u + 32u + 64u + 3u + 255u)

/* An answer of the access point's waited for, the commit or the confirm, and what the supplicant made of it. */
struct sae_wait {
    uint16_t transaction;
    int r;
};

/* The access point's commit or confirm, as w waits for, handed to the supplicant; a refusal's status kept. */
static int sae_answer(const struct buf *b, void *arg)
{
    struct sae_wait *w = arg;
    struct mgmt_sae m;
    if (!mgmt_sae_read(b->frame, b->len, drv_self->mac, sta.ap, &m) || m.transaction != w->transaction) {
        return 0;
    }
    if (m.transaction == MGMT_SAE_COMMIT) {
        w->r = supp_sae_take_commit(m.body, m.len, m.status);
    } else {
        w->r = m.status != 0 ? m.status : supp_sae_take_confirm(m.body, m.len);
    }
    return w->r != SUPP_DISCARD;
}

/*
 * The station's commit or confirm sent, and the access point's own taken, a few times:
 * SUPP_TAKEN, SUPP_AGAIN once the access point asked for a token, SUPP_FAILED, or the status of its refusal.
 */
static int sae_step(uint16_t transaction)
{
    static uint8_t frame[MGMT_AUTH_FIXED + SAE_BODY_MAX];
    int commit = transaction == MGMT_SAE_COMMIT;
    const char *what = commit ? "SAE's commit" : "SAE's confirm";
    struct sae_wait w = { transaction, SUPP_FAILED };
    uint32_t n = 0;
    for (uint32_t tries = 1; tries <= TRIES; tries++) {
        if (tries == 1 || !commit) {
            size_t len = 0;
            const uint8_t *m = commit ? supp_sae_commit(sta.ap, &len) : supp_sae_confirm(&len);
            uint16_t status = commit ? supp_sae_commit_status() : 0;
            n = m ? mgmt_sae(frame, sizeof(frame), drv_self->mac, sta.ap, transaction, status, m, (uint32_t)len) : 0;
            if (n == 0) {
                drv_say("sta: %s not made\n", what);
                return SUPP_FAILED;
            }
        }
        if (sent(what, tries, frame, n) && wait_for(osi_now_us() + SAE_WAIT_US, sae_answer, &w)) {
            return w.r;
        }
    }
    drv_say("sta: %s not answered\n", what);
    return SUPP_FAILED;
}

/* SAE's exchange; one refused fails with the access point's status, one failed or never answered with ESP_FAIL. */
static void sae(void)
{
    uint64_t start = osi_now_us();
    int r = SUPP_AGAIN;
    for (uint32_t commits = 0; r == SUPP_AGAIN && commits <= SAE_TOKENS; commits++) {
        r = sae_step(MGMT_SAE_COMMIT);
    }
    if (r == SUPP_TAKEN) {
        r = sae_step(MGMT_SAE_CONFIRM);
    }
    drv_say("sta: SAE %s in %u ms\n", r == SUPP_TAKEN ? "accepted" : "failed",
            (unsigned)((osi_now_us() - start) / 1000u));
    if (r != SUPP_TAKEN) {
        fail("SAE's authentication", r > 0 ? (uint32_t)r : (uint32_t)ESP_FAIL);
    }
}

/* The authentication, by SAE under WPA3's personal and by open system under WPA2's. */
static void authenticate(void)
{
    if (sta.sae) {
        sae();
    } else {
        open_system();
    }
    sta.state = AUTHENTICATED;
}

struct assoc {
    uint16_t status, aid;
    uint32_t comeback_tu;
};

static int assoc_answer(const struct buf *b, void *arg)
{
    struct assoc *a = arg;
    return mgmt_assoc_answer(b->frame, b->len, drv_self->mac, sta.ap, &a->status, &a->aid, &a->comeback_tu);
}

/*
 * The association, with the supplicant's elements; one refused fails with its status, one unanswered with ESP_FAIL.
 * An access point that holds the station's state from an association whose end it did not hear answers status 30
 * and names in its Timeout Interval element the time to come back: the station waits that long, during which the
 * access point asks the state it holds after the station and, hearing nothing, drops it — its authentication with
 * it — so the station authenticates afresh and asks again, a few times. See TODO.md.
 */
#define ASSOC_TRIES       3u
#define ASSOC_COMEBACK_TU 100u       /* the wait when the access point names no time, in 802.11's time units */
#define ASSOC_WAIT_MAX_US 10000000u  /* the longest a sorry access point may hold the join up */

static void associate(const struct drv_net *ap)
{
    static uint8_t frame[MGMT_ASSOC_MAX];
    uint32_t n = mgmt_assoc_request(frame, sizeof(frame), drv_self->mac, sta.ap, ap->ssid, sta.rsn,
                                    sta.has_rsnx ? sta.rsnx : 0);
    must("the association request", n != 0);
    for (uint32_t tries = 1;; tries++) {
        struct assoc a = { 0, 0, 0 };
        int answered = exchange("the association", frame, n, assoc_answer, &a);
        drv_say("sta: association %s, status %u, AID %u\n",
                answered ? (a.status == 0 ? "accepted" : "refused") : "not answered", (unsigned)a.status,
                (unsigned)a.aid);
        if (answered && a.status == 0) {
            sta.aid = a.aid;
            break;
        }
        if (!answered || a.status != MGMT_TRY_AGAIN || tries >= ASSOC_TRIES) {
            fail("the association answered", answered ? a.status : (uint32_t)ESP_FAIL);
        }
        uint64_t wait_us = (uint64_t)(a.comeback_tu ? a.comeback_tu : ASSOC_COMEBACK_TU) * 1024u;
        if (wait_us > ASSOC_WAIT_MAX_US) {
            wait_us = ASSOC_WAIT_MAX_US;
        }
        drv_say("sta: told to come back in %u ms\n", (unsigned)(wait_us / 1000u));
        osi_delay_ms((uint32_t)((wait_us + 999u) / 1000u));
        authenticate();
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

/*
 * The frame b carries, read once: the plaintext at *frame with its length at *len, and ccmp_frame_kind's verdict
 * on the frame's own Protected bit and the MAC's flag.
 * A protected frame the cipher took has its CCMP header's packet number and key id checked against the key's
 * replay counter, the header then taken away and the bit cleared; the receiver's address picks the key, Address 1,
 * the group's for a group address, whose slot the key id names, so the old key's frames are read until the access
 * point switches.
 * *frame is 0, and the drop counted, if the frame is not the cipher's,
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
    uint32_t hl = ccmp_header_len(f); /* 0 for anything but a Data frame, as a protected management one is */
    uint64_t pn = 0;
    uint8_t id = 0;
    uint64_t *rx_pn = 0; /* the key's replay counters, 0 if the station holds no key of the frame's id */
    if (hl != 0 && *len >= hl + CCMP_HEAD_LEN + 1u && ccmp_head_read(f + hl, &pn, &id)) {
        /* A group frame's key id names its slot, the old key's or the new one's; one to the station, the key in use. */
        int s = to_group ? keys_group_slot(KEYS_GROUP_ID, id) : -1;
        if (s >= 0 && sta.gtk.set[s]) {
            rx_pn = sta.gtk_rx_pn[s];
        } else if (!to_group && sta.ptk.cur_set && id == sta.ptk.cur.id) {
            rx_pn = sta.ptk_pn.rx_pn;
        }
    }
    if (rx_pn == 0) {
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
    int kind = plain_frame(b, drv_self, &frame, &len);
    if (frame != 0 && mgmt_data_read(frame, len, drv_self->mac, sta.ap, &p) && p.proto == SUPP_EAPOL) {
        sta.eapol_heard++;
        supp_rx_eapol(p.src, p.body, p.len, kind == CCMP_FRAME_CCMP ? SUPP_EAPOL_PROTECTED : SUPP_EAPOL_CLEAR);
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
 * The pairwise key in use's bytes and id, and the next packet number under it, under the lock; 0 if no key is
 * in use. The lock rather than a `set` written last, because the compiler may move the key's stores past that
 * flag, and tx_pn is two 32-bit stores on RV32, which a rekey and this reservation would tear.
 */
static int reserve(uint8_t tk[CCMP_TK_LEN], uint8_t *id, uint64_t *pn)
{
    osi_mutex_take(sta.key_lock);
    if (!sta.ptk.cur_set) {
        osi_mutex_give(sta.key_lock);
        return 0;
    }
    memcpy(tk, sta.ptk.cur.tk, CCMP_TK_LEN);
    *id = sta.ptk.cur.id;
    *pn = ++sta.ptk_pn.tx_pn;
    osi_mutex_give(sta.key_lock);
    return 1;
}

/*
 * n bytes at plain protected under the pairwise key in use into crypt: a data frame, and a robust management
 * frame, which shares the key and its packet numbers. 0 if no pairwise key is in use, or the frame does not fit,
 * else the frame's length at crypt.
 */
static uint32_t protect(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n)
{
    uint8_t tk[CCMP_TK_LEN], id;
    uint64_t pn;
    return reserve(tk, &id, &pn) ? ccmp_encrypt(crypt, size, plain, n, tk, pn, id) : 0;
}

static uint32_t protect_mgmt(uint8_t *crypt, uint32_t size, const uint8_t *plain, uint32_t n)
{
    uint8_t tk[CCMP_TK_LEN], id;
    uint64_t pn;
    return reserve(tk, &id, &pn) ? ccmp_encrypt_mgmt(crypt, size, plain, n, tk, pn, id) : 0;
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
        supp_rx_eapol(p.src, p.body, p.len, kind == CCMP_FRAME_CCMP ? SUPP_EAPOL_PROTECTED : SUPP_EAPOL_CLEAR);
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
 * The station's thread: the scan, then of the network's access points, strongest first, the first whose beacon
 * offers suites the station takes, on its channel, with the receiving the station's; the authentication,
 * the association, the supplicant's handshakes, and then the link's service,
 * reporting DRV_JOINED so that the root task starts the network process; see TODO.md.
 */
static void sta_main(void *arg)
{
    struct drv *d = arg;
    sta_scan(d);
    must("a passphrase", d->pass[0] != 0);
    const struct drv_net *ap;
    uint32_t next = 0;
    int taken = 0;
    do {
        ap = net_next(d, &next);
        must("an access point of the network that the station takes", ap != 0);
        memcpy(sta.ap, ap->bssid, 6);
        drv_say("sta: %s at %02x:%02x:%02x:%02x:%02x:%02x, channel %u\n", ap->ssid, ap->bssid[0], ap->bssid[1],
                ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5], (unsigned)ap->channel);
        /* The channel a beacon named, which mac_channel may refuse. */
        drv_must_mac("the access point's channel", mac_channel(ap->channel));
        /*
         * The station's own address and the access point's, set before the authentication, and the station-mode
         * receiving the libraries' association programs, the MAC's list taken once. See mac.c.
         */
        mac_station(drv_self->mac, sta.ap);
        if (!taken) {
            mac_receive(0);
            drv_must_mac("the MAC receives into the driver's list", mac_rx_take(heard));
            taken = 1;
        }
    } while (!security(d, ap));
    authenticate();
    associate(ap);
    /* The association's AID, the receiving's own; see mac.c. */
    mac_receive(sta.aid);
    handshake();
    d->joined = *ap;
    d->authmode = sta.sae ? WIFI_AUTH_WPA3_PSK : WIFI_AUTH_WPA2_PSK;
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
