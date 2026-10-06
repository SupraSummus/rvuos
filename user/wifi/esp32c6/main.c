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

#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "mac.h"
#include "mgmt.h"
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
    drv_say("%s: the MAC's interrupts %u, causes %x; frames broken %u, chained %u, odd %u; overran %u, restarted %u, "
            "stuck %u; pointed again %u\n",
            who, (unsigned)m->interrupts, (unsigned)m->causes, (unsigned)m->broken, (unsigned)m->chained,
            (unsigned)m->odd, (unsigned)m->overran, (unsigned)m->restarted, (unsigned)m->stuck, (unsigned)m->repointed);
}

/* A step of mac.c's that must succeed: 0, or the step that failed, which stops the driver as must does. */
static void must_mac(struct drv *d, const char *step, const char *failed)
{
    must(d, failed ? failed : step, failed ? ESP_FAIL : ESP_OK);
}

/*
 * The driver's own scan: each channel heard through mac.c, each access point kept as heard strongest,
 * and of more than DRV_NETS, the strongest; then sorted, strongest first.
 * It reads beacons alone: it sends no probe, so a probe response it hears answers another station.
 * One that heard none fails, so that a receiver that does not work fails the run.
 */
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

static void scan_own(struct drv *d)
{
    static const uint32_t mgmt = WIFI_PROMIS_FILTER_MASK_MGMT;
    must(d, "the radio's filter", esp_wifi_set_promiscuous_filter(&mgmt));
    must(d, "promiscuous", esp_wifi_set_promiscuous(true));
    must_mac(d, "the MAC receives into the driver's list", mac_rx_take(scan_heard));
    for (uint32_t ch = MAC_CHANNEL_FIRST; ch <= MAC_CHANNEL_LAST; ch++) {
        mac_channel(ch); /* one of those it tunes to, so never refused */
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
 * The station's own authentication, the first step of its own logic: the driver scans for the network by its own code,
 * takes the access point heard strongest, or the page's bssid, retunes to it and sends an open-system
 * Authentication frame by mac_tx, reading the answer through mac.c; the libraries only bring the MAC up.
 * The libraries' station is not asked to connect, so a run with sta=own ends after the authentication, and the
 * association, the keys and the join are to follow; see TODO.md.
 */
#define AUTH_TRIES   3u
#define AUTH_WAIT_US 200000u

static volatile uint32_t auth_done, auth_status;
static uint8_t auth_ap[6];

/* The access point's answer to the Authentication; the receiver's thread alone writes. */
static void auth_heard(const struct mac_frame *m)
{
    uint16_t status;
    if (mgmt_auth_answer(m->frame, m->len, drv_self->mac, auth_ap, &status)) {
        auth_status = status;
        auth_done = 1;
    }
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

static __attribute__((noreturn)) void auth_own(struct drv *d)
{
    scan_own(d);
    const struct drv_net *ap = net_for(d);
    must(d, "the network to authenticate to", ap ? ESP_OK : ESP_FAIL);
    memcpy(auth_ap, ap->bssid, 6);
    drv_say("auth: %s at %02x:%02x:%02x:%02x:%02x:%02x, channel %u\n", ap->ssid, ap->bssid[0], ap->bssid[1],
            ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5], (unsigned)ap->channel);
    /* The channel a beacon named, which mac_channel may refuse, tuned before the MAC is the driver's. */
    must_mac(d, "the access point's channel", mac_channel(ap->channel));
    must_mac(d, "the MAC receives into the driver's list", mac_rx_take(auth_heard));
    static uint8_t frame[MGMT_FRAME_MAX];
    uint32_t n = mgmt_auth_request(frame, sizeof(frame), d->mac, ap->bssid);
    /* A frame lost to the air, or answered too late, is sent again a few times, as a station does. */
    auth_done = 0;
    auth_status = 0;
    for (uint32_t tries = 1; tries <= AUTH_TRIES && !auth_done; tries++) {
        const char *failed = mac_tx(frame, n);
        if (failed) {
            drv_say("auth: the authentication frame, try %u: %s\n", (unsigned)tries, failed);
            continue;
        }
        uint64_t start = osi_now_us();
        while (!auth_done && osi_now_us() - start < AUTH_WAIT_US) {
            osi_delay_ms(2);
        }
    }
    mac_rx_give_back();
    drv_say("auth: %s, status %u\n", auth_done ? (auth_status == 0 ? "accepted" : "refused") : "no answer",
            (unsigned)auth_status);
    must(d, "the authentication answered", auth_done && auth_status == 0 ? ESP_OK : ESP_FAIL);
    d->joined = *ap;
    radio_off();
    drv_say("driver: authenticated; the radio is off\n");
    child_stop(&d->c, DRV_SCANNED);
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
 * What listen counts, in the thread that receives: the frames received whole and those broken,
 * and of the access point followed, its beacons, their RSSI, and the beacons missed,
 * by the access point's own time in each and its beacon interval;
 * a gap of BEACON_GAP_MAX intervals or more, as the access point's restart makes, is not counted.
 * mac.c hands on no broken frame, but counts them, so with own_rx the broken are its count.
 */
#define BEACON_GAP_MAX 64u

static struct hear {
    volatile uint32_t frames, broken, beacons, missed, rssi;
    volatile uint32_t answers, retried; /* probe responses to the station, and of them those sent again */
} hear;
static uint8_t hear_ap[6];
static int hear_ap_set, hear_tsf_set;
static uint32_t hear_tsf;

/* The station asks for the page's network PROBES times, a probe request a second, as a station that scans asks. */
#define PROBES 5u

static uint8_t probe_ap[6];

static void heard(const struct mac_frame *m)
{
    struct drv *d = drv_self;
    struct mgmt_beacon b;
    int again;
    hear.frames++;
    if (d->probe[0] && mgmt_probe_answer(m->frame, m->len, d->mac, d->probe, &b, &again)) {
        memcpy(probe_ap, b.bssid, 6);
        hear.answers++;
        hear.retried += (uint32_t)again;
        return;
    }
    if (!mgmt_beacon(m->frame, m->len, 0, &b) || b.probe_response) {
        return;
    }
    if (!hear_ap_set) {
        memcpy(hear_ap, b.bssid, 6);
        hear_ap_set = 1;
    }
    if (memcmp(b.bssid, hear_ap, 6) != 0) {
        return;
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
}

/* A frame the libraries heard, in the layout mac.c reads, taken to be as long as its header says: theirs to bound. */
static void heard_by_libraries(void *buf, int type)
{
    const uint8_t *c = buf;
    struct mac_frame m;
    (void)type;
    if (!rx_ctrl_whole(c)) {
        hear.broken++;
    } else if (mac_frame_read(c, RX_CTRL_SIZE + rx_ctrl_len(c), &m)) {
        heard(&m);
    }
}

/* What was heard so far, the broken counted where they were dropped. */
static struct hear heard_so_far(void)
{
    struct hear h = hear;
    if (drv_self->own_rx) {
        h.broken = mac_rx_counts.broken;
    }
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
        /* The libraries' sending waits on their interrupt, which mac_rx_take takes, so their probe cannot be finished. */
        must(d, "a probe by the libraries with the driver's interrupt",
             d->probe[0] && !d->own_tx ? ESP_FAIL : ESP_OK);
        must_mac(d, "the MAC receives into the driver's list", mac_rx_take(heard));
    }
    static uint8_t probe[MGMT_FRAME_MAX];
    uint32_t probe_len = 0, probes = 0, probes_failed = 0;
    if (d->probe[0]) {
        probe_len = mgmt_probe_request(probe, sizeof(probe), d->mac, d->probe, d->listen);
        must(d, "the probe request", probe_len != 0 ? ESP_OK : ESP_FAIL);
        drv_say("driver: probing for %s on channel %u from %02x:%02x:%02x:%02x:%02x:%02x, by %s\n", d->probe,
                (unsigned)d->listen, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                d->own_tx ? "the driver's own MAC" : "the libraries");
    }
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
                /* Either gives the frame its sequence number, the libraries as asked to, mac_tx as it does. */
                if (d->own_tx) {
                    const char *failed = mac_tx(probe, probe_len);
                    if (failed) {
                        drv_say("driver: the probe, by the driver's own MAC: %s\n", failed);
                        probes_failed++;
                    }
                } else {
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
    must(d, "every probe sent", probes_failed == 0 ? ESP_OK : ESP_FAIL);
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
    if (d->own_sta) {
        auth_own(d);
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
