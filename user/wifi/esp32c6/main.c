/*
 * The Wi-Fi station's program, its run on the radio's first thread, see radio.c: the MAC and the PHY brought up,
 * then a channel heard, a scan, or the station's own thread, which joins the network the page names; see drv.h.
 *
 * Once the station runs, the thread attends to the root task, and hands the station what the network process
 * puts into the link; the station puts what it receives into the link the other way.
 */

#include <stdarg.h>
#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "mac.h"
#include "mgmt.h"
#include "osi.h"
#include "prof.h"
#include "rvuos.h"
#include "sta.h"

DRV_PROGRAM("the Wi-Fi station", DRV_NEED_RNG | DRV_NEED_NET);

static int started; /* whether esp_wifi_start ran, so that the radio is to be turned off */

/* The radio off, which the libraries turn on in esp_wifi_start: the PHY is closed once nothing uses it. */
void drv_radio_off(void)
{
    if (started) {
        started = 0;
        mac_stop();
    }
}

static void await(const char *what, int32_t id, uint32_t ms)
{
    uint32_t bits = osi_event_wait_bits(&drv_events, 1u << id, 1, 1, ms);
    drv_must(what, bits & (1u << id) ? ESP_OK : ESP_FAIL);
}

/*
 * ESP-IDF's WIFI_INIT_CONFIG_DEFAULT and its crypto table, which the libraries took through esp_wifi_init_internal,
 * are gone with them: the driver's own start needs neither.
 */

/* What mac.c counted while it received, into the log. */
void drv_mac_told(const char *who)
{
    const struct mac_rx_counts *m = &mac_rx_counts;
    drv_say("%s: the MAC's interrupts %u, causes %x; frames broken %u, chained %u, odd %u; overran %u, restarted %u, "
            "stuck %u\n",
            who, (unsigned)m->interrupts, (unsigned)m->causes, (unsigned)m->broken, (unsigned)m->chained,
            (unsigned)m->odd, (unsigned)m->overran, (unsigned)m->restarted, (unsigned)m->stuck);
    /* The sending's, in two lines, as drv_say's line holds 160 bytes: each rate's tries and those acknowledged. */
    const struct mac_tx_counts *t = &mac_tx_counts;
    char line[120];
    uint32_t at = 0;
    for (uint32_t i = 0; i < MAC_TX_RATES && at < sizeof(line); i++) {
        if (t->tries[i] != 0) {
            at += (uint32_t)snprintf(line + at, sizeof(line) - at, " %u:%u/%u", (unsigned)(mac_tx_kbps(i) / 1000u),
                                     (unsigned)t->acked[i], (unsigned)t->tries[i]);
        }
    }
    line[at < sizeof(line) ? at : sizeof(line) - 1u] = 0;
    drv_say("%s: the sending's tries acknowledged by Mb/s%s\n", who, at ? line : " none");
    drv_say("%s: frames not sent %u, ends seen %x, timeouts %u, collisions %u; the rate held %u Mb/s\n", who,
            (unsigned)t->lost, (unsigned)t->ends, (unsigned)t->timeouts, (unsigned)t->collisions,
            (unsigned)(mac_tx_kbps(t->rate) / 1000u));
    static const struct {
        const char *name;
        const struct prof *p;
    } costs[] = {
        { "the sending's lock", &drv_prof.tx_lock },  { "a frame's preparing", &drv_prof.tx_prep },
        { "a try on the air, us", &drv_prof.tx_air }, { "between frames, us", &drv_prof.tx_gap },
        { "a frame's interrupt", &drv_prof.rx_isr },  { "a frame read", &drv_prof.rx_read },
        { "the link's frames sent", &drv_prof.tx_link }, { "the PHY tracked", &drv_prof.phy_track },
        { "the RF off", &drv_prof.rf_off },           { "the RF on", &drv_prof.rf_on },
    };
    for (uint32_t i = 0; i < sizeof(costs) / sizeof(costs[0]); i++) {
        const struct prof *c = costs[i].p;
        if (c->n != 0) {
            drv_say("%s: cost %s: %u times, %u on average, %u at most\n", who, costs[i].name, (unsigned)c->n,
                    (unsigned)(c->sum / c->n), (unsigned)c->most);
        }
    }
}

/*
 * A line about an Ethernet frame for debug=frames: which way it goes, its addresses, its type and length,
 * and what its ARP header asks or tells, or its IPv4 header's addresses and protocol, with UDP's or TCP's ports.
 */
void drv_trace(const char *way, const uint8_t *f, uint32_t len)
{
    char what[96];
    what[0] = 0;
    uint32_t type = len >= 14 ? (uint32_t)f[12] << 8 | f[13] : 0;
    if (type == 0x0806 && len >= 42 && f[21] == 1) {
        snprintf(what, sizeof(what), " who-has %u.%u.%u.%u tell %u.%u.%u.%u", f[38], f[39], f[40], f[41], f[28], f[29],
                 f[30], f[31]);
    } else if (type == 0x0806 && len >= 42) {
        snprintf(what, sizeof(what), " %u.%u.%u.%u is-at %02x:%02x:%02x:%02x:%02x:%02x", f[28], f[29], f[30], f[31],
                 f[22], f[23], f[24], f[25], f[26], f[27]);
    } else if (type == 0x0800 && len >= 34) {
        uint32_t ihl = (f[14] & 0xfu) * 4u, proto = f[23];
        int n = snprintf(what, sizeof(what), " %u.%u.%u.%u > %u.%u.%u.%u proto %u", f[26], f[27], f[28], f[29], f[30],
                         f[31], f[32], f[33], proto);
        if ((proto == 6 || proto == 17) && len >= 14 + ihl + 4 && n > 0) {
            const uint8_t *p = f + 14 + ihl;
            uint32_t sport = (uint32_t)p[0] << 8 | p[1], dport = (uint32_t)p[2] << 8 | p[3];
            int m = snprintf(what + n, sizeof(what) - (uint32_t)n, " port %u > %u", (unsigned)sport, (unsigned)dport);
            if (proto == 17 && (sport == 67 || sport == 68) && (dport == 67 || dport == 68) && len >= 14 + ihl + 16 &&
                m > 0) {
                snprintf(what + n + m, sizeof(what) - (uint32_t)(n + m), " xid 0x%02x%02x%02x%02x", p[12], p[13], p[14],
                         p[15]);
            }
        }
    }
    drv_say("frame %s %02x:%02x:%02x:%02x:%02x:%02x < %02x:%02x:%02x:%02x:%02x:%02x type %04x len %u%s\n", way, f[0],
            f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], (unsigned)type, (unsigned)len, what);
}

/*
 * What listen counts, in the thread that receives: the frames received whole,
 * and of the access point followed, its beacons, their RSSI, and the beacons missed,
 * by the access point's own time in each and its beacon interval;
 * a gap of BEACON_GAP_MAX intervals or more, as the access point's restart makes, is not counted.
 * mac.c hands on no broken frame, but counts them, so the broken are its count.
 * Of the data the access point sends to the group, which it numbers on a count of their own, each frame is counted,
 * and whether the next one heard is the next by number, after a frame with More Data set -- one the access point
 * sends right after, in its run after a DTIM beacon -- and after one without: esp-idf #16096, see TODO.md.
 */
#define BEACON_GAP_MAX 64u

static struct hear {
    volatile uint32_t frames, broken, beacons, missed, rssi;
    volatile uint32_t answers, retried; /* probe responses to the station, and of them those sent again */
    volatile uint32_t group, more;      /* the access point's frames to the group, and of them with More Data */
    volatile uint32_t after_more, gaps_more, after_none, gaps_none; /* the next heard, and of them not the next */
} hear;
static uint8_t hear_ap[6];
static int hear_ap_set, hear_tsf_set, group_set, group_more;
static uint32_t hear_tsf, group_seq;

#define FC_DATA      0x08u /* the frame control's first byte: a data frame, of any subtype */
#define FC_TYPE      0x0cu
#define FC_FROM_DS   0x02u /* its second byte's flags */
#define FC_TO_DS     0x01u
#define FC_MORE_DATA 0x20u

/* A data frame the access point followed sends to the group, its number to the next's. */
static void heard_group(const uint8_t *f, uint32_t len)
{
    if (!hear_ap_set || len < 24u || (f[0] & FC_TYPE) != FC_DATA || (f[1] & (FC_FROM_DS | FC_TO_DS)) != FC_FROM_DS ||
        !(f[4] & 1u) || memcmp(f + 10, hear_ap, 6) != 0) {
        return;
    }
    uint32_t seq = (uint32_t)(f[22] | f[23] << 8) >> 4;
    int more = (f[1] & FC_MORE_DATA) != 0;
    if (group_set && seq != group_seq) {
        int gap = ((seq - group_seq) & 0xfffu) != 1u;
        if (group_more) {
            hear.after_more++;
            hear.gaps_more += (uint32_t)gap;
        } else {
            hear.after_none++;
            hear.gaps_none += (uint32_t)gap;
        }
    }
    hear.group++;
    hear.more += (uint32_t)more;
    group_seq = seq;
    group_more = more;
    group_set = 1;
}

/* The station asks for the page's network PROBES times, a probe request a second, as a station that scans asks. */
#define PROBES 5u

static uint8_t probe_ap[6];

static int heard(const struct mac_frame *m)
{
    struct drv *d = drv_self;
    struct mgmt_beacon b;
    int again;
    hear.frames++;
    heard_group(m->frame, m->len);
    if (d->probe[0] && mgmt_probe_answer(m->frame, m->len, d->mac, d->probe, &b, &again)) {
        memcpy(probe_ap, b.bssid, 6);
        hear.answers++;
        hear.retried += (uint32_t)again;
        return 0;
    }
    if (!mgmt_beacon(m->frame, m->len, 0, &b) || b.probe_response) {
        return 0;
    }
    if (!hear_ap_set) {
        memcpy(hear_ap, b.bssid, 6);
        hear_ap_set = 1;
    }
    if (memcmp(b.bssid, hear_ap, 6) != 0) {
        return 0;
    }
    uint32_t tsf = (uint32_t)b.tsf, interval = (uint32_t)b.interval * 1024u;
    if (hear_tsf_set && interval != 0) {
        uint32_t gap = (tsf - hear_tsf + interval / 2u) / interval;
        if (gap > 1 && gap < BEACON_GAP_MAX) {
            hear.missed += gap - 1;
        }
    }
    hear_tsf = tsf;
    hear_tsf_set = 1;
    hear.beacons++;
    hear.rssi += (uint32_t)-m->rssi;
    return 0;
}

/* What was heard so far, the broken as mac.c counted them. */
static struct hear heard_so_far(void)
{
    struct hear h = hear;
    h.broken = mac_rx_counts.broken;
    return h;
}

/* What was heard since then, into the log, over ms milliseconds: every frame, the broken among them. */
static void hear_tell(const char *what, const struct hear *then, uint32_t ms)
{
    struct hear now = heard_so_far();
    uint32_t beacons = now.beacons - then->beacons, broken = now.broken - then->broken;
    drv_say("listen: %s %u ms: %u frames, %u broken; %02x:%02x:%02x:%02x:%02x:%02x: %u beacons at -%u dBm, %u missed\n",
            what, (unsigned)ms, (unsigned)(now.frames - then->frames + broken), (unsigned)broken, hear_ap[0],
            hear_ap[1], hear_ap[2], hear_ap[3], hear_ap[4], hear_ap[5], (unsigned)beacons,
            (unsigned)(beacons ? (now.rssi - then->rssi) / beacons : 0), (unsigned)(now.missed - then->missed));
}

/*
 * The page's channel heard, every frame the sniffer passes, see drv.h;
 * what was heard told about once a second, as the root task's checks wake the thread, and over the whole run at its end,
 * which fails if it heard nothing, so that a receiver that does not work fails the run.
 */
static __attribute__((noreturn)) void listen(struct drv *d)
{
    if (d->bssid_set) {
        memcpy(hear_ap, d->bssid, 6);
        hear_ap_set = 1;
    }
    mac_sniffer(1);
    drv_must_mac("the channel", mac_channel(d->listen));
    drv_must_mac("the MAC receives into the driver's list", mac_rx_take(heard));
    static uint8_t probe[MGMT_FRAME_MAX];
    uint32_t probe_len = 0, probes = 0, probes_failed = 0;
    if (d->probe[0]) {
        probe_len = mgmt_probe_request(probe, sizeof(probe), d->mac, d->probe, d->listen);
        drv_must("the probe request", probe_len != 0 ? ESP_OK : ESP_FAIL);
        drv_say("driver: probing for %s on channel %u from %02x:%02x:%02x:%02x:%02x:%02x\n", d->probe,
                (unsigned)d->listen, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    }
    d->channel = d->listen;
    child_report(&d->c, DRV_LISTENING);
    struct hear first = heard_so_far(), then = first;
    uint64_t start = osi_now_us(), told = start;
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        if (d->leave) {
            break;
        }
        uint64_t now = osi_now_us();
        if (now - told >= 1000000u) {
            hear_tell("the last", &then, (uint32_t)((now - told) / 1000u));
            then = heard_so_far();
            told = now;
            if (probe_len && probes < PROBES) {
                const char *failed = mac_tx(probe, probe_len);
                if (failed) {
                    drv_say("driver: the probe: %s\n", failed);
                    probes_failed++;
                }
                probes++;
            }
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    mac_sniffer(0);
    mac_rx_give_back();
    drv_mac_told("listen");
    hear_tell("in", &first, (uint32_t)((osi_now_us() - start) / 1000u));
    drv_say("listen: %u frames to the group, %u with More Data; the next not heard after More Data %u of %u, "
            "after none %u of %u\n",
            (unsigned)hear.group, (unsigned)hear.more, (unsigned)hear.gaps_more, (unsigned)hear.after_more,
            (unsigned)hear.gaps_none, (unsigned)hear.after_none);
    if (probe_len) {
        drv_say("probe: %u sent, %u failed; %u answers from %02x:%02x:%02x:%02x:%02x:%02x, %u of them sent again\n",
                (unsigned)probes, (unsigned)probes_failed, (unsigned)hear.answers, probe_ap[0], probe_ap[1], probe_ap[2],
                probe_ap[3], probe_ap[4], probe_ap[5], (unsigned)hear.retried);
    }
    drv_radio_off();
    drv_must("a frame heard", hear.frames != 0 ? ESP_OK : ESP_FAIL);
    drv_must("every probe sent", probes_failed == 0 ? ESP_OK : ESP_FAIL);
    drv_must("a probe answered", probe_len == 0 || hear.answers != 0 ? ESP_OK : ESP_FAIL);
    drv_say("driver: stopped listening; the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}

/*
 * The station runs in a thread of its own, sta.c, and the first thread attends to the root task alone,
 * as it alone waits on the driver's inbox: it answers the root task's checks, hands the station the frames the network
 * process filled the link's buffers with, and passes the root task's ask to leave on, once.
 * It waits only once a turn of the link left nothing,
 * as the network process signals a frame only once the driver has taken those it was last woken for.
 */
static __attribute__((noreturn)) void attend(struct drv *d)
{
    int passed = 0;
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        int more = sta_link_take(d);
        if (d->leave && !passed) {
            passed = 1;
            sta_leave();
        }
        if (!more) {
            rv_wait(CHILD_INBOX, &bits);
        }
    }
}

/*
 * The driver's own esp_wifi_start, the libraries' wifi_start_process written out for the station: the hardware
 * bring-up's calls and the station's start, with none of the libraries' state. Their init, their mode and their
 * state -- their ieee80211_set_hmac_stop, wifi_mode_set, _do_wifi_start's bookkeeping, ieee80211_update_phy_country,
 * their control block's interface masks and started state -- are not made: their station never runs. Their
 * adc2_wifi_acquire, a weak stub in their ieee80211_ioctl.o that returns 0, is left out. See NOTES.md.
 */

/* The STA_START their wifi_station_start posts, through the adapter, which logs it and sets the group's bit. */
static void sta_start_post(void)
{
    osi_event_post("WIFI_EVENT", WIFI_EVENT_STA_START, 0, 0);
}

static int drv_wifi_start(void)
{
    mac_up(drv_self->mac);
    /*
     * The libraries' wifi_mode_set and _do_wifi_start, the driver's own now: the station's start's hardware
     * accesses and the STA_START event, with none of net80211's own state. See macstart.c and NOTES.md.
     */
    mac_station_start(drv_self->mac, sta_start_post);
    /*
     * The libraries' ieee80211_update_phy_country is left out: its guard reads their started state, its
     * ieee80211_regdomain_get_country is a read alone, its phy_update_country_info this driver's stub, and its
     * hal_init_tx_pwr fills the power table their association alone reads and writes the power registers again,
     * with the values mac_init_tx_pwr wrote. Their association never runs.
     */
    return ESP_OK;
}

/*
 * The trace window: only the driver's bring-up and stop, and nothing else, so that every device access is its own.
 * With trace=1 the driver maps no device, so each access faults to the watcher and the root task carries it out and logs it;
 * with trace=2 the driver maps them as in a plain run, so the same window runs with no fault, the step 2 baseline.
 * It ends with DRV_TRACED once mac_stop returns.
 */
static __attribute__((noreturn)) void trace_run(void)
{
    uint64_t began = osi_now_us();
    drv_say("driver: the trace window starts, %u bytes of heap free\n", (unsigned)osi_heap_free());
    osi_trace_init(128);
    osi_trace("window", 0, 0);
    osi_trace("start", 0, 0);
    drv_must("the driver's start", drv_wifi_start());
    started = 1;
    /* The trace slows the run down, so the wait is given the whole run rather than five seconds. */
    await("the station started", WIFI_EVENT_STA_START, 120000);
    drv_say("driver: init to the station started took %u ms\n", (unsigned)((osi_now_us() - began) / 1000u));
    drv_say("driver: stopping\n");
    osi_trace("stop", 0, 0);
    mac_stop();
    started = 0;
    osi_trace("done", 0, 0);
    osi_trace_dump();
    drv_say("driver: the window is done; the tracer served %u faults, %u interrupts\n", osi_faults_served(),
            osi_interrupts());
    drv_stop(DRV_TRACED);
}

/*
 * The station brought up by the driver's own start, which brings the MAC and the PHY up, libphy's calibration among
 * it; the libraries are not initialised, and run nothing.
 */
void drv_run(struct drv *d, struct self *own)
{
    uint32_t note, status;
    if ((status = slot_new(own, &note)) != KERR_OK ||
        (status = rv_pool_alloc(own->pool, CAP_NOTIFICATION, note, 0)) != KERR_OK) {
        drv_failed("the link's lock", status);
        drv_stop(CHILD_FAILED);
    }
    sta_link_init(note);
    if (d->trace) {
        trace_run();
    }
    drv_say("driver: %u bytes of heap free\n", (unsigned)osi_heap_free());
    if (d->debug & DRV_DEBUG_WPA) {
        drv_wpa_debug();
    }
    drv_must("the driver's start", drv_wifi_start());
    started = 1;
    await("the station started", WIFI_EVENT_STA_START, 5000);
    child_report(&d->c, DRV_UP);
    drv_must_mac("the MAC's sending", mac_tx_init(own));
    if (d->ssid[0] == 0 && d->listen) {
        listen(d);
    }
    if (d->ssid[0] == 0) {
        sta_scan(d);
        drv_radio_off();
        drv_say("driver: %u bytes of heap free; the radio is off\n", (unsigned)osi_heap_free());
        child_stop(&d->c, DRV_SCANNED);
    }
    drv_must_mac("the station's thread", sta_start(d));
    attend(d);
}
