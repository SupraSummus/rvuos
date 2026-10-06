/*
 * The driver's entry and its first thread: Espressif's libraries brought up in station mode,
 * then a scan, or the network the page names joined and the link served; see drv.h.
 *
 * The root task starts the driver as a child at drv_main, with a0 at its page and its stack in the child's data.
 * The thread fills the image's data, makes its account of what it may build of its own,
 * starts the adapter's threads, and joins them as one of theirs, since it calls the libraries too.
 * Joined, it reports so, and moves what the network process puts into the link to the libraries,
 * whose receiver puts what the station receives into the link the other way.
 */

#include <stdarg.h>
#include <stdint.h>

#include "beacon.h"
#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "mac.h"
#include "osi.h"
#include "rvuos.h"

void drv_main(struct child_page *page);

/* The image's own layout, from its linker script, which esptool writes into the window onto flash. */
extern uint8_t drv_data_load[], drv_data_start[], drv_data_end[], drv_bss_start[], drv_bss_end[];

__attribute__((section(".header"), used)) const struct drv_header drv_image = { DRV_MAGIC, drv_main };

struct drv *drv_self;
static struct self own;
static struct osi_funcs *funcs;
static void *events; /* an event group of the adapter's, a bit for each Wi-Fi event below 32 */
static struct lock say_lock;
static uint32_t say_word;
static uint32_t let_go; /* the reason the access point gave when it last let the station go */
static int started;     /* whether esp_wifi_start ran, so that the radio is to be turned off */
static uint32_t stats_told;

void drv_vsay(const char *tag, const char *fmt, va_list args)
{
    char line[160];
    uint32_t n = 0;
    if (tag) {
        for (; tag[n] && n < 24; n++) {
            line[n] = tag[n];
        }
        line[n++] = ':';
        line[n++] = ' ';
    }
    vsnprintf(line + n, sizeof(line) - n, fmt, args);
    lock_take(&say_lock);
    rv_puts(CHILD_LOG, line);
    lock_give(&say_lock);
}

void drv_say(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    drv_vsay(0, fmt, args);
    va_end(args);
}

/* The first failure's step into the page, which the root task zeroed, so the name stays terminated. */
static void failed_at(struct drv *d, const char *step, uint32_t detail)
{
    if (d->failed[0] == 0) {
        for (uint32_t i = 0; i + 1 < sizeof(d->failed) && step[i]; i++) {
            d->failed[i] = step[i];
        }
        d->c.detail = detail;
    }
}

void drv_failed(const char *step, uint32_t detail)
{
    failed_at(drv_self, step, detail);
    drv_say("driver: %s failed: %d (0x%x)\n", step, (int)detail, (unsigned)detail);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void drv_event(const char *base, int32_t id, const void *data, size_t size)
{
    struct drv *d = drv_self;
    drv_say("event: %s %d, %u bytes\n", base, (int)id, (unsigned)size);
    if (!streq(base, "WIFI_EVENT") || (uint32_t)id >= 32 || events == 0) {
        return;
    }
    if (id == WIFI_EVENT_STA_CONNECTED && size >= sizeof(struct sta_connected)) {
        const struct sta_connected *c = data;
        memcpy(d->joined.bssid, c->bssid, 6);
        memcpy(d->joined.ssid, c->ssid, 32);
        d->joined.ssid[c->ssid_len < 32 ? c->ssid_len : 32] = 0;
        d->joined.channel = c->channel;
        d->authmode = c->authmode;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED && size >= sizeof(struct sta_disconnected)) {
        let_go = ((const struct sta_disconnected *)data)->reason;
        if (d->c.state == DRV_JOINED && !d->leave) {
            /* Nothing joins again yet: the root task hears the station is gone, and why. */
            drv_failed("let go by the access point", let_go);
            child_report(&d->c, CHILD_FAILED);
        }
    }
    funcs->event_group_set_bits(events, 1u << id);
}

/* The radio off, which the libraries turn on in esp_wifi_start: the PHY is closed once nothing uses it. */
static void radio_off(void)
{
    if (started) {
        started = 0;
        esp_wifi_stop();
    }
}

/* A step of the libraries' that must succeed; one that fails stops the driver, and the page says which. */
static void must(struct drv *d, const char *step, esp_err_t e)
{
    if (e != ESP_OK) {
        drv_failed(step, (uint32_t)e);
        radio_off();
        child_stop(&d->c, CHILD_FAILED);
    }
    drv_say("driver: %s\n", step);
}

static void await(struct drv *d, const char *what, int32_t id, uint32_t ms)
{
    uint32_t bits = funcs->event_group_wait_bits(events, 1u << id, 1, 1, ms);
    must(d, what, bits & (1u << id) ? ESP_OK : ESP_FAIL);
}

/* The first of the events of ids a and b, or -1 if neither comes within ms. */
static int32_t await_either(int32_t a, int32_t b, uint32_t ms)
{
    uint32_t bits = funcs->event_group_wait_bits(events, 1u << a | 1u << b, 1, 0, ms);
    return bits & (1u << a) ? a : bits & (1u << b) ? b : -1;
}

/*
 * ESP-IDF's WIFI_INIT_CONFIG_DEFAULT, with fewer buffers, as the driver's block of RAM is small,
 * and of its features WPA3's SAE alone, unless the page says no_sae.
 */
static void config(struct init_config *c, const struct drv *d)
{
    memset(c, 0, sizeof(*c));
    c->osi_funcs = funcs;
    drv_crypto_funcs(&c->crypto_funcs);
    c->feature_caps = d->no_sae ? 0 : FEATURE_WPA3_SAE;
    c->static_rx_buf_num = 6;
    c->dynamic_rx_buf_num = 16;
    c->tx_buf_type = 1;
    c->dynamic_tx_buf_num = 16;
    c->rx_mgmt_buf_num = 5;
    c->ampdu_rx_enable = 1;
    c->ampdu_tx_enable = 1;
    c->rx_ba_win = 6;
    c->beacon_max_len = 752;
    c->mgmt_sbuf_num = 32;
    c->espnow_max_encrypt_num = 7;
    c->tx_hetb_queue_num = 3;
    c->wifi_task_stack_size = 6656;
    c->magic = INIT_CONFIG_MAGIC;
}

/*
 * A scan hears each channel for SCAN_DWELL_MS, two beacon intervals and a little, and sends nothing:
 * the libraries' passive scan, or with own_rx, the driver's own.
 */
#define SCAN_CHANNELS 13u
#define SCAN_DWELL_MS 250u

static void scan(struct drv *d)
{
    static uint8_t record[ESP_AP_RECORD_MAX];
    static const struct scan_config passive = { .type = WIFI_SCAN_TYPE_PASSIVE, .passive_ms = SCAN_DWELL_MS };
    uint16_t n = 0;
    must(d, "a scan begun", esp_wifi_scan_start(&passive, false));
    await(d, "the scan done", WIFI_EVENT_SCAN_DONE, 15000);
    must(d, "the scan's count", esp_wifi_scan_get_ap_num(&n));
    for (uint32_t i = 0; i < n && i < DRV_NETS; i++) {
        must(d, "a scan's record", esp_wifi_scan_get_ap_record(record));
        const struct ap_record *r = (const struct ap_record *)record;
        struct drv_net *a = &d->nets[i];
        memcpy(a->bssid, r->bssid, 6);
        memcpy(a->ssid, r->ssid, 32);
        a->ssid[32] = 0;
        a->channel = r->primary;
        a->rssi = r->rssi;
        d->net_count = i + 1;
    }
}

/* What mac.c counted while it received, into the log. */
static void mac_told(const char *who)
{
    const struct mac_rx_counts *m = &mac_rx_counts;
    drv_say("%s: the MAC's interrupts %u, causes %x; frames chained %u, odd %u; overran %u, restarted %u, stuck %u; "
            "pointed again %u\n",
            who, (unsigned)m->interrupts, (unsigned)m->causes, (unsigned)m->chained, (unsigned)m->odd,
            (unsigned)m->overran, (unsigned)m->restarted, (unsigned)m->stuck, (unsigned)m->repointed);
}

/*
 * The driver's own scan: each channel heard through mac.c, each access point kept as heard strongest,
 * and of more than DRV_NETS, the strongest; then sorted, strongest first.
 * It reads beacons alone: it sends no probe, so a probe response it hears answers another station.
 * One that heard none fails, so that a receiver that does not work fails the run.
 */
#define BEACON 0x80

static volatile uint8_t scan_on; /* the channel being heard */

static void scan_heard(const uint8_t *c)
{
    struct drv *d = drv_self;
    struct beacon b;
    const uint8_t *f = c + RX_CTRL_SIZE;
    uint32_t len = rx_ctrl_len(c);
    int rssi = (int8_t)c[RX_CTRL_RSSI];
    if (!rx_ctrl_whole(c) || len < 4 || f[0] != BEACON || !beacon_read(f, len - 4, scan_on, &b)) {
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

static void scan_own(struct drv *d)
{
    static const uint32_t mgmt = WIFI_PROMIS_FILTER_MASK_MGMT;
    must(d, "the radio's filter", esp_wifi_set_promiscuous_filter(&mgmt));
    must(d, "promiscuous", esp_wifi_set_promiscuous(true));
    const char *failed = mac_rx_take(scan_heard);
    must(d, failed ? failed : "the MAC receives into the driver's list", failed ? ESP_FAIL : ESP_OK);
    for (uint32_t ch = 1; ch <= SCAN_CHANNELS; ch++) {
        scan_on = (uint8_t)ch;
        mac_channel(ch);
        osi_delay_ms(SCAN_DWELL_MS);
    }
    mac_rx_give_back();
    mac_told("scan");
    must(d, "an access point heard", d->net_count != 0 ? ESP_OK : ESP_FAIL);
    for (uint32_t i = 1; i < d->net_count; i++) {
        for (uint32_t j = i; j > 0 && d->nets[j].rssi > d->nets[j - 1].rssi; j--) {
            struct drv_net t = d->nets[j];
            d->nets[j] = d->nets[j - 1];
            d->nets[j - 1] = t;
        }
    }
}

/*
 * A line about an Ethernet frame for debug=frames: which way it goes, its addresses, its type and length,
 * and what its ARP header asks or tells, or its IPv4 header's addresses and protocol, with UDP's or TCP's ports.
 */
static void trace(const char *way, const uint8_t *f, uint32_t len)
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

/* The libraries' counters of the radio's receiving and sending, into the log. */
static void counters(void)
{
    drv_say("driver: the libraries' counters follow\n");
    esp_wifi_statis_dump(WIFI_STATIS_RXTX);
}

/*
 * What the radio hears, for debug=air, timed by the MAC in microseconds: the access point's frames to the group
 * and to the station, with their sequence numbers, the acknowledgements that end what the station sent,
 * broken frames about as long as a DHCP answer, and of the rest a count, told with a beacon once a second.
 */
static uint8_t air_own[6];
static uint32_t air_beacon_told, air_broken;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void air(void *buf, int type)
{
    const uint8_t *c = buf, *f = c + RX_CTRL_SIZE;
    const uint8_t *bssid = drv_self->joined.bssid;
    uint32_t t = le32(c + RX_CTRL_TIMESTAMP);
    uint32_t len = rx_ctrl_len(c);
    int rssi = (int8_t)c[RX_CTRL_RSSI];
    if (!rx_ctrl_whole(c)) {
        if (len >= 400 && len <= 800) {
            drv_say("air %u broken len %u rssi %d state %u end %u\n", (unsigned)t, (unsigned)len, rssi,
                    (unsigned)c[RX_CTRL_STATE], (unsigned)c[RX_CTRL_RXEND]);
        } else {
            air_broken++;
        }
        return;
    }
    if (type == WIFI_PKT_CTRL) {
        if (len >= 10 && memcmp(f + 4, air_own, 6) == 0) {
            drv_say("air %u %s\n", (unsigned)t, (f[0] & 0xf0) == 0x90 ? "ba" : "ack");
        }
        return;
    }
    if (len < 24 || memcmp(f + 10, bssid, 6) != 0) {
        return;
    }
    if (type == WIFI_PKT_MGMT) {
        if (f[0] == 0x80 && len >= 32 && t - air_beacon_told >= 1000000u) {
            air_beacon_told = t;
            drv_say("air %u beacon tsf %u, %u broken\n", (unsigned)t, (unsigned)le32(f + 24), (unsigned)air_broken);
            air_broken = 0;
        }
        return;
    }
    if (type == WIFI_PKT_DATA) {
        const uint8_t *to = f + 4, *from = f + 16;
        unsigned seq = (unsigned)(f[22] | f[23] << 8) >> 4;
        if (to[0] & 1) {
            drv_say("air %u group from %02x:%02x:%02x:%02x:%02x:%02x len %u seq %u rssi %d format %u rate %u more %u\n",
                    (unsigned)t, from[0], from[1], from[2], from[3], from[4], from[5], (unsigned)len, seq, rssi,
                    (unsigned)(c[RX_CTRL_FORMAT] & 0xfu), (unsigned)(c[1] & 0x1fu), (unsigned)(f[1] >> 5 & 1));
        } else if (memcmp(to, air_own, 6) == 0) {
            drv_say("air %u to us len %u seq %u retry %u rssi %d\n", (unsigned)t, (unsigned)len, seq,
                    (unsigned)(f[1] >> 3 & 1), rssi);
        }
    }
}

/* What the station receives, an Ethernet frame, into the link; dropped while the link is full. */
static esp_err_t receive(void *buffer, uint16_t len, void *eb)
{
    struct drv *d = drv_self;
    if (d->debug & DRV_DEBUG_FRAMES) {
        trace("in", buffer, len);
    }
    if (chan_send(&d->link, buffer, len) == 0) {
        d->rx_frames++;
    } else {
        d->rx_dropped++;
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

#define JOIN_TRIES 3

/*
 * Joins d->ssid: the libraries scan every channel for it, take the access point heard best, or the page's bssid,
 * and authenticate and associate; the supplicant runs the handshake, the PMK of whose passphrase it derives first.
 * They take WPA3's personal where the access point offers it, unless the page says no_sae, and WPA2's otherwise,
 * or with no passphrase an open network, which they refuse yet; see TODO.md.
 * Power save stays off unless the page asks for it: asleep between beacons, the station takes no address; see TODO.md.
 * They hand the station's frames to the receiver once it is joined, as ESP-IDF registers it on the connection.
 */
static void join(struct drv *d)
{
    static uint8_t buf[ESP_CONFIG_MAX] __attribute__((aligned(8)));
    struct sta_config *c = (struct sta_config *)buf;
    memset(buf, 0, sizeof(buf));
    memcpy(c->ssid, d->ssid, strlen(d->ssid));
    memcpy(c->password, d->pass, strlen(d->pass));
    c->scan_method = 1; /* WIFI_ALL_CHANNEL_SCAN */
    c->threshold.authmode = d->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c->pmf_cfg.capable = !d->no_pmf;
    if (d->bssid_set) {
        c->bssid_set = true;
        memcpy(c->bssid, d->bssid, 6);
    }
    if (d->pass[0]) {
        drv_supp_prepare(d->ssid, d->pass);
    }
    if (d->no_ax) {
        uint8_t bgn = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
        must(d, "no 802.11ax", esp_wifi_set_protocol(WIFI_IF_STA, bgn));
    }
    must(d, "power save", esp_wifi_set_ps(d->modem_sleep ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
    must(d, "the station's configuration", esp_wifi_set_config(WIFI_IF_STA, buf));
    memset(buf, 0, sizeof(buf));
    /*
     * A join the access point refuses, or whose answer is lost, is tried again, as ESP-IDF leaves to the program;
     * the last refusal fails it with the reason the access point gave, and no answer in 20 s with ESP_FAIL.
     */
    int32_t e = -1;
    for (uint32_t i = 0; i < JOIN_TRIES; i++) {
        if (i > 0) {
            drv_say("driver: the join refused, reason %u; again\n", (unsigned)let_go);
        }
        must(d, "esp_wifi_connect", esp_wifi_connect());
        e = await_either(WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED, 20000);
        if (e != WIFI_EVENT_STA_DISCONNECTED) {
            break;
        }
    }
    must(d, "the join", e == WIFI_EVENT_STA_CONNECTED ? ESP_OK : e < 0 || let_go == 0 ? ESP_FAIL : (esp_err_t)let_go);
    must(d, "the station's receiver", esp_wifi_internal_reg_rxcb(WIFI_IF_STA, receive));
    if (d->debug & DRV_DEBUG_AIR) {
        static const uint32_t heard = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL |
                                      WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_FCSFAIL;
        static const uint32_t acks = WIFI_PROMIS_CTRL_FILTER_MASK_ACK | WIFI_PROMIS_CTRL_FILTER_MASK_BA;
        must(d, "the station's address", esp_wifi_get_mac(WIFI_IF_STA, air_own));
        must(d, "the radio's filter", esp_wifi_set_promiscuous_filter(&heard));
        must(d, "the radio's control filter", esp_wifi_set_promiscuous_ctrl_filter(&acks));
        must(d, "what the radio hears", esp_wifi_set_promiscuous_rx_cb(air));
        must(d, "promiscuous", esp_wifi_set_promiscuous(true));
    }
}

/*
 * The station leaves the network once the run is over, as the root task asks,
 * so that the access point keeps no association of it into the next run:
 * one that guards management frames would refuse the next run's first join while it holds it.
 */
static __attribute__((noreturn)) void leave(struct drv *d)
{
    if (d->debug & DRV_DEBUG_STATS) {
        counters();
    }
    esp_err_t e = esp_wifi_disconnect_internal();
    if (e == ESP_OK) {
        funcs->event_group_wait_bits(events, 1u << WIFI_EVENT_STA_DISCONNECTED, 1, 1, 1000);
    }
    radio_off();
    drv_say("driver: left the network: %d; the radio is off\n", (int)e);
    child_stop(&d->c, DRV_LEFT);
}

/*
 * For as long as the system runs: what the network process put into the link, to the libraries,
 * which copy each frame; one they refuse, with no buffer free, is dropped, as a network may drop it.
 * The network process is trusted as the libraries are, so the link is emptied at each wake.
 * Each wake answers the root task's check, which a wait on the link sees too,
 * and sees whether to leave or to tell the libraries' counters.
 */
static __attribute__((noreturn)) void serve(struct drv *d)
{
    for (;;) {
        uint32_t len, bits;
        const uint8_t *frame;
        child_answer(&d->c);
        if (d->leave) {
            leave(d);
        }
        if (d->stats != stats_told) {
            stats_told = d->stats;
            counters();
        }
        while ((frame = chan_get_begin(&d->link, &len)) != 0) {
            if (d->debug & DRV_DEBUG_FRAMES) {
                trace("out", frame, len);
            }
            if (esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, (uint16_t)len) == ESP_OK) {
                d->tx_frames++;
            } else {
                d->tx_dropped++;
            }
            chan_get_end(&d->link);
        }
        rv_wait(CHILD_INBOX, &bits);
    }
}

/*
 * What listen counts, in the thread that receives: every frame and those broken,
 * and of the access point followed, its beacons, their RSSI, and the beacons missed,
 * by the access point's own time in each and its beacon interval;
 * a gap of BEACON_GAP_MAX intervals or more, as the access point's restart makes, is not counted.
 */
#define BEACON_MIN     40 /* a beacon's header, timestamp, interval and capabilities, and FCS */
#define BEACON_GAP_MAX 64u

static struct hear {
    volatile uint32_t frames, broken, beacons, missed, rssi;
    volatile uint32_t answers, retried; /* probe responses to the station, and of them those sent again */
} hear;
static uint8_t hear_ap[6];
static int hear_ap_set, hear_tsf_set;
static uint32_t hear_tsf;

/*
 * A probe request, as a station that scans sends one: to every station, from the station's own address,
 * asking for one network, with the rates the station takes and the channel it asks on; the MAC adds the FCS.
 */
#define PROBE_REQUEST  0x40
#define PROBE_RESPONSE 0x50
#define PROBE_SSID     36 /* where a probe response's elements start, after its timestamp, interval and capabilities */
#define PROBES         5u
#define PROBE_MAX      (24u + 2u + 32u + 10u + 6u + 3u)

static uint8_t probe_from[6];
static uint8_t probe_ap[6];

static uint32_t probe_request(uint8_t *f, const char *ssid, uint32_t channel)
{
    static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    static const uint8_t more_rates[] = { 50, 4, 0x30, 0x48, 0x60, 0x6c };
    uint32_t n = 0, len = strlen(ssid);
    f[n++] = PROBE_REQUEST;
    f[n++] = 0;
    f[n++] = 0;
    f[n++] = 0;
    memset(f + n, 0xff, 6);
    memcpy(f + n + 6, probe_from, 6);
    memset(f + n + 12, 0xff, 6);
    n += 18;
    f[n++] = 0;
    f[n++] = 0;
    f[n++] = 0;
    f[n++] = (uint8_t)len;
    memcpy(f + n, ssid, len);
    n += len;
    memcpy(f + n, rates, sizeof(rates));
    n += sizeof(rates);
    memcpy(f + n, more_rates, sizeof(more_rates));
    n += sizeof(more_rates);
    f[n++] = 3;
    f[n++] = 1;
    f[n++] = (uint8_t)channel;
    return n;
}

/* A probe response to the station, for the network it asks for. */
static void heard_answer(const uint8_t *f, uint32_t len)
{
    const char *ssid = drv_self->probe;
    uint32_t n = strlen(ssid);
    if (len < PROBE_SSID + 2u + n + 4u || memcmp(f + 4, probe_from, 6) != 0 || f[PROBE_SSID] != 0 ||
        f[PROBE_SSID + 1] != n || memcmp(f + PROBE_SSID + 2, ssid, n) != 0) {
        return;
    }
    memcpy(probe_ap, f + 16, 6);
    hear.answers++;
    if (f[1] & 0x08u) {
        hear.retried++;
    }
}

static void heard(const uint8_t *c)
{
    const uint8_t *f = c + RX_CTRL_SIZE;
    hear.frames++;
    if (!rx_ctrl_whole(c)) {
        hear.broken++;
        return;
    }
    if (f[0] == PROBE_RESPONSE && drv_self->probe[0]) {
        heard_answer(f, rx_ctrl_len(c));
        return;
    }
    if (f[0] != BEACON || rx_ctrl_len(c) < BEACON_MIN) {
        return;
    }
    if (!hear_ap_set) {
        memcpy(hear_ap, f + 10, 6);
        hear_ap_set = 1;
    }
    if (memcmp(f + 10, hear_ap, 6) != 0) {
        return;
    }
    uint32_t tsf = le32(f + 24), interval = ((uint32_t)f[32] | (uint32_t)f[33] << 8) * 1024u;
    if (hear_tsf_set && interval != 0) {
        uint32_t gap = (tsf - hear_tsf + interval / 2u) / interval;
        if (gap > 1 && gap < BEACON_GAP_MAX) {
            hear.missed += gap - 1;
        }
    }
    hear_tsf = tsf;
    hear_tsf_set = 1;
    hear.beacons++;
    hear.rssi += (uint32_t)-(int8_t)c[RX_CTRL_RSSI];
}

static void heard_by_libraries(void *buf, int type)
{
    (void)type;
    heard(buf);
}

/* What the libraries wrote to the MAC to send the first probe, for debug=tx, into the log; see mac.h. */
static void words_told(const char *what, const uint32_t *w, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 8) {
        char line[96];
        uint32_t k = 0;
        for (uint32_t j = i; j < n && j < i + 8; j++) {
            k += (uint32_t)snprintf(line + k, sizeof(line) - k, " %08x", (unsigned)w[j]);
        }
        drv_say("tx: %s +%02x:%s\n", what, (unsigned)(4u * i), line);
    }
}

static void tx_told(void)
{
    const struct mac_tx_seen *s = &mac_tx_seen;
    uint32_t now[MAC_TX_PPDU_WORDS];
    mac_tx_ppdu(s->slot, now);
    drv_say("tx: slot %u\n", (unsigned)s->slot);
    words_told("queue", s->queue, 4);
    words_told("state", s->state, 4);
    words_told("ppdu", s->ppdu, MAC_TX_PPDU_WORDS);
    words_told("ppdu after", now, MAC_TX_PPDU_WORDS);
    drv_say("tx: descriptor at %08x\n", (unsigned)s->desc_at);
    words_told("descriptor", s->desc, s->desc_at ? MAC_TX_DESC_WORDS : 0);
    char line[2 * MAC_TX_FRAME_SIZE + 1];
    for (uint32_t i = 0; i < MAC_TX_FRAME_SIZE; i++) {
        snprintf(line + 2 * i, 3, "%02x", s->frame[i]);
    }
    drv_say("tx: frame at %08x: %s\n", (unsigned)s->frame_at, s->frame_at ? line : "");
}

/* What was heard since then, into the log, over ms milliseconds. */
static void hear_tell(const char *what, const struct hear *then, uint32_t ms)
{
    uint32_t beacons = hear.beacons - then->beacons;
    drv_say("listen: %s %u ms: %u frames, %u broken; %02x:%02x:%02x:%02x:%02x:%02x: %u beacons at -%u dBm, %u missed\n",
            what, (unsigned)ms, (unsigned)(hear.frames - then->frames), (unsigned)(hear.broken - then->broken),
            hear_ap[0], hear_ap[1], hear_ap[2], hear_ap[3], hear_ap[4], hear_ap[5], (unsigned)beacons,
            (unsigned)(beacons ? (hear.rssi - then->rssi) / beacons : 0), (unsigned)(hear.missed - then->missed));
}

/*
 * The page's channel heard, promiscuous, every frame, the broken too, by the libraries or by mac.c, see drv.h;
 * what was heard told about once a second, as the root task's checks wake the thread, and over the whole run at its end,
 * which fails if it heard nothing, so that a receiver that does not work fails the run.
 */
static __attribute__((noreturn)) void listen(struct drv *d)
{
    static const uint32_t all = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL |
                                WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_FCSFAIL;
    if (d->bssid_set) {
        memcpy(hear_ap, d->bssid, 6);
        hear_ap_set = 1;
    }
    must(d, "the radio's filter", esp_wifi_set_promiscuous_filter(&all));
    if (!d->own_rx) {
        must(d, "what the radio hears", esp_wifi_set_promiscuous_rx_cb(heard_by_libraries));
    }
    must(d, "promiscuous", esp_wifi_set_promiscuous(true));
    must(d, "the channel", esp_wifi_set_channel(d->listen, 0));
    if (d->own_rx) {
        must(d, "a probe, with rx=own", d->probe[0] ? ESP_FAIL : ESP_OK);
        const char *failed = mac_rx_take(heard);
        must(d, failed ? failed : "the MAC receives into the driver's list", failed ? ESP_FAIL : ESP_OK);
    }
    static uint8_t probe[PROBE_MAX];
    uint16_t probe_seq = 0;
    uint32_t probe_len = 0, probes = 0, probes_failed = 0, tx_shown = 0;
    if (d->probe[0]) {
        must(d, "the station's address", esp_wifi_get_mac(WIFI_IF_STA, probe_from));
        probe_len = probe_request(probe, d->probe, d->listen);
        drv_say("driver: probing for %s on channel %u from %02x:%02x:%02x:%02x:%02x:%02x, by %s\n", d->probe,
                (unsigned)d->listen, probe_from[0], probe_from[1], probe_from[2], probe_from[3], probe_from[4],
                probe_from[5], d->own_tx ? "the driver's own MAC" : "the libraries");
    }
    child_report(&d->c, DRV_LISTENING);
    struct hear first = { 0 }, then = { 0 };
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
            then = hear;
            told = now;
            if (mac_tx_seen.full && !tx_shown) {
                tx_told();
                tx_shown = 1;
            }
            if (probe_len && probes < PROBES) {
                if (d->own_tx) {
                    /* The libraries set the frame's sequence number themselves; the driver's own frame carries it. */
                    probe[22] = (uint8_t)(probe_seq << 4);
                    probe[23] = (uint8_t)(probe_seq >> 4);
                    probe_seq = (uint16_t)((probe_seq + 1u) & 0xfffu);
                    const char *failed = mac_tx(probe, probe_len);
                    if (failed) {
                        drv_say("driver: the probe, by the driver's own MAC: %s\n", failed);
                        probes_failed++;
                    }
                } else {
                    mac_tx_seen.armed = (d->debug & DRV_DEBUG_TX) != 0;
                    if (esp_wifi_80211_tx(WIFI_IF_STA, probe, (int)probe_len, true) != ESP_OK) {
                        probes_failed++;
                    }
                }
                probes++;
            }
        }
        rv_wait(CHILD_INBOX, &bits);
    }
    if (d->own_rx) {
        mac_rx_give_back();
        mac_told("listen");
    }
    hear_tell(d->own_rx ? "by mac.c, in" : "by the libraries, in", &first,
              (uint32_t)((osi_now_us() - start) / 1000u));
    if (probe_len) {
        drv_say("probe: %u sent, %u failed; %u answers from %02x:%02x:%02x:%02x:%02x:%02x, %u of them sent again\n",
                (unsigned)probes, (unsigned)probes_failed, (unsigned)hear.answers, probe_ap[0], probe_ap[1], probe_ap[2],
                probe_ap[3], probe_ap[4], probe_ap[5], (unsigned)hear.retried);
    }
    radio_off();
    must(d, "a frame heard", hear.frames != 0 ? ESP_OK : ESP_FAIL);
    must(d, "a probe answered", probe_len == 0 || hear.answers != 0 ? ESP_OK : ESP_FAIL);
    drv_say("driver: stopped listening; the radio is off\n");
    child_stop(&d->c, DRV_LEFT);
}

static __attribute__((noreturn)) void run(struct drv *d)
{
    static struct init_config c;
    config(&c, d);
    drv_say("driver: %u bytes of heap free\n", (unsigned)osi_heap_free());
    must(d, "esp_wifi_init_internal", esp_wifi_init_internal(&c));
    /* The libraries' own lines at INFO, as ESP-IDF sets them, or at the level the configuration asks for. */
    uint32_t level = d->lib_log ? d->lib_log : WIFI_LOG_INFO;
    osi_log_level(level);
    must(d, "the libraries' log level", esp_wifi_internal_set_log_level((int)level));
    must(d, "the libraries' log", esp_wifi_internal_set_log_mod(0, 0, true));
    if (d->debug & DRV_DEBUG_WPA) {
        drv_wpa_debug();
    }
    must(d, "the supplicant's table", drv_wpa_register());
    must(d, "station mode", esp_wifi_set_mode(WIFI_MODE_STA));
    must(d, "esp_wifi_start", esp_wifi_start());
    started = 1;
    await(d, "the station started", WIFI_EVENT_STA_START, 5000);
    child_report(&d->c, DRV_UP);
    if (d->ssid[0] == 0 && d->listen) {
        listen(d);
    }
    if (d->ssid[0] == 0) {
        if (d->own_rx) {
            scan_own(d);
        } else {
            scan(d);
        }
        if (d->debug & DRV_DEBUG_STATS) {
            counters();
        }
        radio_off();
        drv_say("driver: %u bytes of heap free; the radio is off\n", (unsigned)osi_heap_free());
        child_stop(&d->c, DRV_SCANNED);
    }
    join(d);
    drv_say("driver: joined, %u bytes of heap free\n", (unsigned)osi_heap_free());
    child_report(&d->c, DRV_JOINED);
    serve(d);
}

void drv_main(struct child_page *page)
{
    struct drv *d = (struct drv *)page;
    /*
     * The ROM's ets_delay_us counts the core's cycles in the user-mode performance counter, 0x802,
     * which rvuos leaves stopped in a new process and keeps with it after; see manual/targets.md.
     */
    __asm__ volatile("csrw 0x800, %0\n\tcsrw 0x801, %0" : : "r"(1));
    memcpy(drv_data_start, drv_data_load, (size_t)(drv_data_end - drv_data_start));
    memset(drv_bss_start, 0, (size_t)(drv_bss_end - drv_bss_start));
    drv_self = d;
    child_self(&own, &d->own);
    uint32_t note, status;
    if ((status = slot_new(&own, &note)) != KERR_OK ||
        (status = rv_pool_alloc(own.pool, CAP_NOTIFICATION, note, 0)) != KERR_OK) {
        failed_at(d, "the log's lock", status);
        child_stop(&d->c, CHILD_FAILED);
    }
    say_lock = (struct lock){ (uint32_t)(uintptr_t)&say_word, note };
    lock_init(&say_lock);
    funcs = osi_init(d, &own);
    /* This thread calls the libraries too, so it is one of the adapter's, on the stack the root task gave it. */
    if (funcs == 0 || osi_adopt("driver", (uintptr_t)page, (uintptr_t)page + DRV_DATA_SIZE) == 0) {
        child_stop(&d->c, CHILD_FAILED);
    }
    events = funcs->event_group_create();
    run(d);
}
