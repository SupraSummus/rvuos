/*
 * The driver's entry and its first thread: Espressif's libraries brought up in station mode, which brings the MAC up,
 * then a channel heard, a scan, or the station's own thread, which joins the network the page names; see drv.h.
 *
 * The root task starts the driver as a child at drv_main, with a0 at its page and its stack in the child's data.
 * The thread fills the image's data, makes its account of what it may build of its own,
 * starts the adapter's threads, and joins them as one of theirs, since it calls the libraries too.
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
#include "rvuos.h"
#include "sta.h"

void drv_main(struct child_page *page);

/* The image's own layout, from its linker script, which esptool writes into the window onto flash. */
extern uint8_t drv_data_load[], drv_data_start[], drv_data_end[], drv_bss_start[], drv_bss_end[];

__attribute__((section(".header"), used)) const struct drv_header drv_image = { DRV_MAGIC, drv_main };

struct drv *drv_self;
static struct thread *first_thread; /* the driver's first, which alone waits on its inbox */
static struct self own;
static struct osi_funcs *funcs;
static void *events; /* an event group of the adapter's, a bit for each Wi-Fi event below 32 */
static struct lock say_lock;
static uint32_t say_word;
static int started; /* whether esp_wifi_start ran, so that the radio is to be turned off */

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
    (void)data;
    drv_say("event: %s %d, %u bytes\n", base, (int)id, (unsigned)size);
    if (!streq(base, "WIFI_EVENT") || (uint32_t)id >= 32 || events == 0) {
        return;
    }
    funcs->event_group_set_bits(events, 1u << id);
}

/* The radio off, which the libraries turn on in esp_wifi_start: the PHY is closed once nothing uses it. */
void drv_radio_off(void)
{
    if (started) {
        started = 0;
        mac_stop();
    }
}

/*
 * The state reported, and the calling thread stopped for good: the first in its inbox, as child_stop stops it,
 * any other in a wait of the adapter's, so that it takes no wake of the first's.
 */
void drv_stop(uint32_t state)
{
    if (osi_self() == first_thread) {
        child_stop(&drv_self->c, state);
    }
    child_report(&drv_self->c, state);
    for (;;) {
        osi_delay_ms(OSI_FOREVER);
    }
}

/* A step of the libraries' that must succeed; one that fails stops the calling thread, and the page says which. */
void drv_must(const char *step, esp_err_t e)
{
    if (e != ESP_OK) {
        drv_failed(step, (uint32_t)e);
        drv_radio_off();
        drv_stop(CHILD_FAILED);
    }
    drv_say("driver: %s\n", step);
}

static void await(const char *what, int32_t id, uint32_t ms)
{
    uint32_t bits = funcs->event_group_wait_bits(events, 1u << id, 1, 1, ms);
    drv_must(what, bits & (1u << id) ? ESP_OK : ESP_FAIL);
}

/*
 * ESP-IDF's WIFI_INIT_CONFIG_DEFAULT, with fewer buffers, as the driver's block of RAM is small, and none of its features,
 * as the libraries' station never joins.
 * The driver receives into a list of its own, see mac.c, so the libraries' static buffers, the MAC's list until then,
 * lie idle: it leaves them two, ESP-IDF's least, so that the heap holds what SAE takes.
 */
static void config(struct init_config *c)
{
    memset(c, 0, sizeof(*c));
    c->osi_funcs = funcs;
    drv_crypto_funcs(&c->crypto_funcs);
    c->static_rx_buf_num = 2;
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

/* What mac.c counted while it received, into the log. */
void drv_mac_told(const char *who)
{
    const struct mac_rx_counts *m = &mac_rx_counts;
    drv_say("%s: the MAC's interrupts %u, causes %x; frames broken %u, chained %u, odd %u; overran %u, restarted %u, "
            "stuck %u; pointed again %u\n",
            who, (unsigned)m->interrupts, (unsigned)m->causes, (unsigned)m->broken, (unsigned)m->chained,
            (unsigned)m->odd, (unsigned)m->overran, (unsigned)m->restarted, (unsigned)m->stuck, (unsigned)m->repointed);
}

/* A step of mac.c's that must succeed: 0, or the step that failed, which stops the driver as drv_must does. */
void drv_must_mac(const char *step, const char *failed)
{
    drv_must(failed ? failed : step, failed ? ESP_FAIL : ESP_OK);
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
 * as it alone waits on the driver's inbox: it answers the root task's checks, hands the station what the network
 * process put into the link, and passes the root task's ask to leave on, once.
 */
static __attribute__((noreturn)) void attend(struct drv *d)
{
    int passed = 0;
    for (;;) {
        uint32_t bits;
        child_answer(&d->c);
        sta_link_take(d);
        if (d->leave && !passed) {
            passed = 1;
            sta_leave();
        }
        rv_wait(CHILD_INBOX, &bits);
    }
}

/*
 * The driver's own esp_wifi_start, taking the libraries' bring-up over a step at a time: the body is the libraries'
 * wifi_start_process written out -- adc2_wifi_acquire, ieee80211_set_hmac_stop, the hardware bring-up's calls,
 * wifi_mode_set, _do_wifi_start, ieee80211_update_phy_country -- so that each of those can be taken over in turn and
 * the window's sequence held to the libraries' by tools/mac-trace.py's diff. Only the station is written; the
 * libraries' start brings an interface up per mode -- reason 0 the station, 1 the soft AP, 3 then both, and none for
 * another -- so another mode stops here rather than guesses. See user/wifi/NOTES.md.
 */
extern int wifi_init_completed(void);
extern int adc2_wifi_acquire(void);
extern void ieee80211_set_hmac_stop(int stop);
extern int wifi_mode_set(int mode);
extern int _do_wifi_start(int mode);
extern void ieee80211_update_phy_country(void);
extern void chm_init(void *chm);

/*
 * The libraries' MAC configuration, their hal_init, group by group: its register writes about HAL_CFG, HAL_HOLD and
 * HAL_MISC are the driver's own, mac.c's, and the groups between them are still the libraries' calls, each marked in
 * drv_mac_config below so that the trace's segments show it and a later step can take it over. See user/wifi/NOTES.md.
 */
extern void mac_txrx_init(void);
extern void hal_mac_rx_set_policy(uint32_t iface, uint32_t a, uint32_t b, uint32_t c);
extern void mac_rxbuf_init(void);
extern void hal_he_init(void);
extern void mac_last_rxbuf_init(void);
extern void hal_mac_rate_autoack_init(void);
extern void hal_mac_disable_low_rate(void);
extern void hal_attenna_init(void);
extern void hal_mac_set_rxbuf_reload_use_hw_beacon_enable(void);
extern void hal_timer_update_by_rtc(uint32_t which, uint32_t hz);
extern void hal_coex_pti_init(void);
extern void hal_set_rx_active_pti(uint32_t pti);
extern void hal_set_rx_ack_pti(uint32_t pti);
extern void hal_set_wifi_default_pti(uint32_t pti);
extern void hal_set_ofdma_sequence_pti(void);

extern void wDev_ProcessFiq(void);   /* pp: their interrupt handler, which they install for the MAC's source */
extern void pm_noise_check_enable(void);
extern void pm_disconnected_start(void);
extern uint8_t g_mac_sleep_en;           /* the libraries' MAC sleep flag, which gates the modem wake */
extern void *g_wdev_last_desc_reset_ptr; /* the ROM cell holding their wdev whose first byte is the flag below */
extern void *g_wifi_nvs;                 /* the libraries' configuration, whose first byte is the mode */
extern char g_ic[];                      /* the libraries' shared control block */

/*
 * The libraries' control block's per-interface masks, a bit each: the interfaces a stop has left, and the ones that
 * are up. The station is their interface 0, its bit the low one.
 */
#define G_IC_STOP_MASK  0x24du
#define G_IC_START_MASK 0x24eu

/*
 * The libraries' wifi_reset_mac, written out: their reset pulse through the adapter, their wdev told its last RX
 * descriptor was reset -- a flag their own receive reads until the driver takes the receive over -- and the MAC's
 * receive off, which their hal_mac_rx_disable does, the one function of theirs this call had left.
 */
static void drv_reset_mac(void)
{
    funcs->wifi_reset_mac();
    *(uint8_t *)g_wdev_last_desc_reset_ptr = 1;
    mac_rx_off();
}

/*
 * The libraries' hal_init, the MAC's configuration, written out: their register writes about HAL_CFG, HAL_HOLD and
 * HAL_MISC and their RX-policy words (mac.c), and the groups between them -- the txrx queues, the receive policy,
 * the RX buffers, the HE tables, the ack rates and low-rate mask, the cipher, the antenna, the timer and the coex
 * PTI -- still their calls, each marked so that the trace's segments show its accesses and a later step is held to
 * it alone. Their hal_init calls the groups in this order, with no argument but the interface's and the timer's.
 */
static void drv_mac_config(void)
{
    osi_trace("mac-config", 0, 0);
    mac_config_start();
    osi_trace("mac-txrx", 0, 0);
    mac_txrx_init();
    osi_trace("mac-policy", 0, 0);
    for (uint32_t i = 0; i < 4u; i++) {
        mac_rx_policy_word(i);
        hal_mac_rx_set_policy(i, 0, 0, 0);
    }
    osi_trace("mac-rxbuf", 0, 0);
    mac_rxbuf_init();
    osi_trace("mac-he", 0, 0);
    hal_he_init();
    mac_last_rxbuf_init();
    osi_trace("mac-rate", 0, 0);
    hal_mac_rate_autoack_init();
    hal_mac_disable_low_rate();
    osi_trace("mac-crypto", 0, 0);
    mac_crypto_init();
    osi_trace("mac-antenna", 0, 0);
    hal_attenna_init();
    osi_trace("mac-post", 0, 0);
    mac_config_finish();
    hal_mac_set_rxbuf_reload_use_hw_beacon_enable();
    osi_trace("mac-pti", 0, 0);
    hal_timer_update_by_rtc(1, funcs->slowclk_cal_get());
    hal_coex_pti_init();
    uint8_t pti_active = 0, pti_default = 1; /* their hal_init's own bytes, which their coex_pti_get fills */
    funcs->coex_pti_get(3, &pti_active);
    funcs->coex_pti_get(0xfu, &pti_default);
    hal_set_rx_active_pti(0);
    hal_set_rx_ack_pti(pti_active);
    hal_set_wifi_default_pti(pti_default);
    hal_set_ofdma_sequence_pti();
}

/*
 * The libraries' wifi_hw_start, the hardware bring-up they run once before their task brings the interface up,
 * written out for the station's fresh start: the stop mask clear, neither guard tripped. The libraries' other
 * branch -- an interface a stop left behind, woken rather than brought up anew -- is not written, since the driver
 * starts the station once from a stopped radio; another state stops here. See user/wifi/NOTES.md.
 */
static int drv_hw_start(void)
{
    uint8_t *ic = (uint8_t *)g_ic;
    if (ic[G_IC_STOP_MASK] != 0 || (ic[G_IC_START_MASK] & 1) != 0) {
        return ESP_FAIL; /* the libraries' wake branch, or the station already up: neither is written */
    }
    ieee80211_set_hmac_stop(0);
    funcs->wifi_pm_sleep_lock_acquire();
    funcs->wifi_clock_enable();
    if (g_mac_sleep_en) {
        funcs->wifi_rtc_disable_iso();
    }
    funcs->phy_enable();
    funcs->coex_enable();
    drv_reset_mac();
    mac_tx_block_clear();              /* the libraries' ic_mac_init: their hal_mac_init's tx-block clear */
    chm_init(g_ic);
    /*
     * The libraries' ic_set_interrupt_handler, written out: their hal_init, the MAC's configuration, which
     * drv_mac_config writes out group by group, then the two interrupt sources the adapter routes and their handler,
     * armed. Their set_intr passes the core the Wi-Fi task runs on and the adapter ignores it; the driver passes
     * none. Their handler is the one the driver's own receive trades for its own once it takes the MAC, see mac.c.
     */
    drv_mac_config();
    funcs->set_intr(0, 2 /* the modem's power */, 1, 1);
    funcs->set_intr(0, 0 /* the MAC */, 1, 1);
    funcs->set_isr(1, (void *)wDev_ProcessFiq, 0);
    funcs->ints_on(1u << 1);
    mac_default_policy(drv_self->mac); /* the libraries' chip_enable: their default receive policy */
    mac_rx_on();                       /* and their ic_enable_rx */
    pm_noise_check_enable();
    funcs->wifi_bb_sleep_retention_attach();
    funcs->wifi_mac_sleep_retention_attach();
    ic[G_IC_STOP_MASK] |= 1; /* the station's interface, the libraries' reason 0 */
    ic[G_IC_START_MASK] |= 1;
    pm_disconnected_start();
    return ESP_OK;
}

static int drv_wifi_start(void)
{
    if (!wifi_init_completed()) {
        return ESP_FAIL;
    }
    int rv = adc2_wifi_acquire();
    if (rv != 0) {
        return rv;
    }
    ieee80211_set_hmac_stop(0);
    int mode = *(const uint8_t *)g_wifi_nvs;
    if (mode != WIFI_MODE_STA) {
        return ESP_FAIL; /* only the station is written; the libraries' other modes do not */
    }
    if ((rv = drv_hw_start()) != 0) {
        return rv;
    }
    if ((rv = wifi_mode_set(mode)) != 0) {
        return rv;
    }
    if ((rv = _do_wifi_start(mode)) != 0) {
        return rv;
    }
    ((uint8_t *)g_ic)[0x1f1] = 2;
    ieee80211_update_phy_country();
    return ESP_OK;
}

/*
 * The libraries' start or the driver's own, so that a traced run of each, of the same image, can be held together by
 * tools/mac-trace.py's diff; the driver's own posts the libraries' bring-up for now, so the two agree. See drv.h.
 */
static int bringup_start(void)
{
    return drv_self->lib_start ? esp_wifi_start() : drv_wifi_start();
}

/*
 * The trace window: only the libraries' bring-up and stop, and nothing else, so that every device access is its own.
 * With trace=1 the driver maps no device, so each access faults to the watcher and the root task carries it out and logs it;
 * with trace=2 the driver maps them as in a plain run, so the same window runs with no fault, the step 2 baseline.
 * It ends with DRV_TRACED once mac_stop returns.
 */
static __attribute__((noreturn)) void trace_run(void)
{
    static struct init_config c;
    config(&c);
    uint64_t began = osi_now_us();
    drv_say("driver: the trace window starts, %u bytes of heap free\n", (unsigned)osi_heap_free());
    osi_trace_init(128);
    osi_trace("window", 0, 0);
    drv_must("esp_wifi_init_internal", esp_wifi_init_internal(&c));
    osi_trace("mode", 0, 0);
    drv_must("station mode", esp_wifi_set_mode(WIFI_MODE_STA));
    osi_trace("start", 0, 0);
    drv_must("esp_wifi_start", bringup_start());
    started = 1;
    /* The trace slows the libraries down, so the wait is given the whole run rather than five seconds. */
    await("the station started", WIFI_EVENT_STA_START, 120000);
    drv_say("driver: init to the station started took %u ms\n", (unsigned)((osi_now_us() - began) / 1000u));
    drv_say("driver: stopping the libraries\n");
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
 * The libraries brought up in station mode, which brings the MAC and the PHY up, though their station never joins:
 * it is given no supplicant, and the driver takes the MAC's receiving from it before anything is joined.
 */
static __attribute__((noreturn)) void run(struct drv *d)
{
    static struct init_config c;
    if (d->trace) {
        trace_run();
    }
    config(&c);
    drv_say("driver: %u bytes of heap free\n", (unsigned)osi_heap_free());
    drv_must("esp_wifi_init_internal", esp_wifi_init_internal(&c));
    /* The libraries' own lines at INFO, as ESP-IDF sets them, or at the level the configuration asks for. */
    uint32_t level = d->lib_log ? d->lib_log : WIFI_LOG_INFO;
    osi_log_level(level);
    drv_must("the libraries' log level", esp_wifi_internal_set_log_level((int)level));
    drv_must("the libraries' log", esp_wifi_internal_set_log_mod(0, 0, true));
    if (d->debug & DRV_DEBUG_WPA) {
        drv_wpa_debug();
    }
    drv_must("station mode", esp_wifi_set_mode(WIFI_MODE_STA));
    drv_must("esp_wifi_start", bringup_start());
    started = 1;
    await("the station started", WIFI_EVENT_STA_START, 5000);
    child_report(&d->c, DRV_UP);
    drv_must_mac("the MAC's sending", mac_tx_init());
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

void drv_main(struct child_page *page)
{
    struct drv *d = (struct drv *)page;
    /*
     * The ROM's ets_delay_us counts the core's cycles in the user-mode performance counter, 0x802,
     * which rvuos leaves stopped in a new process and keeps with it after,
     * at the rate of the clock the ROM found, which the clock's own replaces; see manual/targets.md.
     */
    __asm__ volatile("csrw 0x800, %0\n\tcsrw 0x801, %0" : : "r"(1));
    uint64_t now;
    uint32_t hz, counter;
    rv_clock_read(d->clock, &now, &hz, &counter);
    ets_update_cpu_frequency(hz / 1000000u);
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
    if (funcs == 0 || (first_thread = osi_adopt("driver", (uintptr_t)page, (uintptr_t)page + DRV_DATA_SIZE)) == 0) {
        child_stop(&d->c, CHILD_FAILED);
    }
    events = funcs->event_group_create();
    run(d);
}
