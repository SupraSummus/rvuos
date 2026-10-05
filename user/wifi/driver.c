/*
 * The driver of the CYW43439, a process of its own; see wifi.h for what it holds.
 *
 * It starts with a0 at the page it shares with the root task, which says where everything else lies,
 * and keeps its state on its stack, since the globals are the root task's.
 * The firmware blob is lent as frames that together are larger than the regions left,
 * so the driver installs them one at a time in a region of its own, its window, and uploads what each holds.
 * Once the chip runs, the root task takes the blob back and connects the link,
 * and the driver moves frames between the chip and the link for as long as the system runs.
 */

#include <stddef.h>

#include "cyw43.h"
#include "lib/libc.h"
#include "lib/say.h"
#include "wifi.h"
#include "wlan.h"

#define NO_FRAME 0xffffffffu

struct drv {
    struct cyw43 chip;
    struct drv_page *page;
    struct out out;
    uint32_t window;   /* the blob frame installed in the window, or NO_FRAME */
    int scan_done;
    uint32_t results;
    int joined;        /* 1 joined, -1 failed, 0 not yet */
    struct wlan wlan;
};

static struct drv *drv_of(struct wlan *w)
{
    return (struct drv *)((uint8_t *)w - offsetof(struct drv, wlan));
}

/* A scan's result goes into the page, once for each access point; the last event says the scan is done. */
static void on_event(struct wlan *w, const struct wlan_event *e)
{
    struct drv *d = drv_of(w);
    struct drv_page *s = d->page;
    struct wlan_bss bss;
    if (s->mode == MODE_STA && d->joined == 0) {
        d->joined = wlan_join_event(e, s->pass[0] != '\0');
    }
    if (e->type == WLAN_E_ESCAN_RESULT && e->status != WLAN_STATUS_PARTIAL) {
        d->scan_done = 1;
        return;
    }
    if (!wlan_bss_parse(e, &bss)) {
        return;
    }
    d->results++;
    for (uint32_t i = 0; i < s->net_count; i++) {
        if (memcmp(s->nets[i].bssid, bss.bssid, 6) == 0) {
            if (bss.rssi > s->nets[i].rssi) {
                s->nets[i].rssi = bss.rssi;
                s->nets[i].channel = bss.channel;
            }
            return;
        }
    }
    if (s->net_count < DRV_NETS) {
        struct drv_net *n = &s->nets[s->net_count++];
        memcpy(n->bssid, bss.bssid, 6);
        memcpy(n->ssid, bss.ssid, sizeof(n->ssid));
        n->rssi = bss.rssi;
        n->channel = bss.channel;
    }
}

/*
 * For debug=dhcp, a line for each DHCP message heard, a client's or a server's, whoever it is for:
 * where it came from and went, its type, its transaction and the client it names.
 */
static void dhcp_heard(struct drv *d, const uint8_t *f, uint32_t len)
{
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[14 + 9] != 17) {
        return;
    }
    const uint8_t *u = f + 14 + (f[14] & 0x0fu) * 4u;
    uint32_t sport = (uint32_t)u[0] << 8 | u[1], dport = (uint32_t)u[2] << 8 | u[3];
    if (!((sport == 67 && dport == 68) || (sport == 68 && dport == 67)) || u + 8 + 240 > f + len) {
        return;
    }
    const uint8_t *m = u + 8;
    uint32_t type = 0;
    for (const uint8_t *o = m + 240; o + 2 < f + len && o[0] != 255;) {
        if (o[0] == 0) {
            o++;
            continue;
        }
        if (o[0] == 53) {
            type = o[2];
        }
        o += 2 + o[1];
    }
    uint32_t xid = (uint32_t)m[4] << 24 | (uint32_t)m[5] << 16 | (uint32_t)m[6] << 8 | m[7];
    say(&d->out, "dhcp %M > %M type %u xid %x for %M\n", f + 6, f, type, xid, m + 28);
}

/* A frame from the chip to the network process; dropped while the link is down or full. */
static void on_data(struct wlan *w, const uint8_t *frame, uint32_t len)
{
    struct drv_page *p = drv_of(w)->page;
    if (p->dhcp_log) {
        dhcp_heard(drv_of(w), frame, len);
    }
    if (chan_send(&p->link, frame, len) != 0) {
        p->rx_dropped++;
        return;
    }
    p->rx_frames++;
}

/*
 * Waits for the timer; other bits need no keeping, since what they announce lies in the page and the link.
 * Each wait answers the root task's check, since the driver waits only as its loops come round.
 */
static void drv_sleep(struct cyw43 *chip, uint32_t us)
{
    struct drv *d = (struct drv *)((uint8_t *)chip - offsetof(struct drv, chip));
    child_sleep(us);
    child_answer(&d->page->c);
}

/* The address of the blob's byte at off, with the frame holding it installed, and how many follow in that frame. */
static const uint8_t *blob_at(struct drv *d, uint32_t off, uint32_t *left)
{
    const struct drv_page *s = d->page;
    uint32_t start = 0;
    for (uint32_t i = 0; i < s->blob_frames; i++) {
        uint32_t size = s->blob_size[i];
        if (off < start + size) {
            if (d->window != i) {
                if (d->window != NO_FRAME) {
                    rv_process_uninstall(CHILD_SELF, s->window);
                }
                if (rv_process_install(CHILD_SELF, s->window, s->blob_slot[i], RIGHT_R) != KERR_OK) {
                    rv_breakpoint();
                }
                d->window = i;
            }
            *left = start + size - off;
            return (const uint8_t *)(uintptr_t)(s->blob_base + off);
        }
        start += size;
    }
    *left = 0;
    return 0;
}

/* Copies the blob's bytes [off, off + len) to the chip's RAM at ram, a frame at a time; checks them if check. */
static int upload(struct drv *d, uint32_t off, uint32_t len, uint32_t ram, int check)
{
    while (len) {
        uint32_t left;
        const uint8_t *p = blob_at(d, off, &left);
        if (p == 0) {
            return -1;
        }
        uint32_t n = len < left ? len : left;
        if (check) {
            if (cyw43_verify(&d->chip, ram, p, n) != 0) {
                return -1;
            }
        } else {
            cyw43_load(&d->chip, ram, p, n);
        }
        off += n;
        ram += n;
        len -= n;
    }
    return 0;
}

/* A state, with where the chip got to for a failure. */
static void report(struct drv *d, uint32_t state)
{
    d->page->c.step = d->chip.step;
    d->page->c.detail = d->chip.detail;
    child_report(&d->page->c, state);
}

/*
 * For as long as the system runs: what the firmware sends, events and frames, and the frames the network process
 * put into the link, each sent as the firmware's credit allows.
 * The chip is polled every millisecond, and a frame to send wakes the driver at once.
 */
static __attribute__((noreturn)) void serve(struct drv *d)
{
    struct drv_page *p = d->page;
    int linked = 0;
    for (;;) {
        child_answer(&p->c);
        if (!linked && chan_ready(&p->link)) {
            linked = 1;
            say(&d->out, "drv: the link is up\n");
        }
        uint32_t moved = wlan_poll(&d->wlan);
        uint32_t len;
        const uint8_t *frame;
        while ((frame = chan_get_begin(&p->link, &len)) != 0) {
            if (wlan_send(&d->wlan, frame, len) != 0) {
                break;
            }
            p->tx_frames++;
            moved++;
            chan_get_end(&p->link);
        }
        if (moved == 0) {
            uint32_t bits = 0;
            rv_timer_set(CHILD_TIMER, CHILD_BIT_TIMER, 1000);
            rv_wait(CHILD_INBOX, &bits);
        }
    }
}

static __attribute__((noreturn)) void stop(struct drv *d, uint32_t state)
{
    d->page->c.step = d->chip.step;
    d->page->c.detail = d->chip.detail;
    child_stop(&d->page->c, state);
}

static __attribute__((noreturn)) void fail(struct drv *d, uint32_t step, uint32_t detail)
{
    d->chip.step = step;
    d->chip.detail = detail;
    stop(d, CHILD_FAILED);
}

/* Reads what the firmware sends until *done is set, or ms milliseconds have passed. */
static void wait_for(struct drv *d, const int *done, uint32_t ms)
{
    for (uint32_t i = 0; !*done && i < ms; i++) {
        if (wlan_poll(&d->wlan) == 0) {
            drv_sleep(&d->chip, 1000);
        }
    }
}

/* Powers the chip, uploads the firmware and the NVRAM from the blob, and sets the firmware up with the CLM. */
static void boot(struct drv *d)
{
    struct drv_page *s = d->page;
    gspi_init(&d->chip.bus, s->pio_base, s->pins_base);
    if (cyw43_bus_up(&d->chip) != 0 || cyw43_prepare(&d->chip) != 0) {
        stop(d, CHILD_FAILED);
    }
    say(&d->out, "drv: chip 43439, loading firmware\n");

    uint32_t left;
    struct blob_header h = *(const struct blob_header *)blob_at(d, 0, &left);
    if (h.magic != BLOB_MAGIC) {
        fail(d, STEP_FW_VERIFY, h.magic);
    }
    /* The check reads back the first KiB and the last bytes, from a word: the chip aligns the address it is given. */
    uint32_t tail = (h.fw_size - 256u) & ~3u;
    if (upload(d, h.fw_offset, h.fw_size, 0, 0) != 0 || upload(d, h.fw_offset, 1024, 0, 1) != 0 ||
        upload(d, h.fw_offset + tail, h.fw_size - tail, tail, 1) != 0) {
        stop(d, CHILD_FAILED);
    }
    const uint8_t *nvram = blob_at(d, h.nvram_offset, &left);
    if (nvram == 0 || left < ((h.nvram_size + 3u) & ~3u) || cyw43_start(&d->chip, nvram, h.nvram_size) != 0) {
        stop(d, CHILD_FAILED);
    }
    say(&d->out, "drv: firmware runs, %u bytes\n", h.fw_size);

    const uint8_t *clm = blob_at(d, h.clm_offset, &left);
    if (clm == 0 || left < h.clm_size) {
        fail(d, STEP_FW_VERIFY, h.clm_offset);
    }
    int32_t err = wlan_init(&d->wlan, clm, h.clm_size, s->mac);
    if (err != 0) {
        fail(d, STEP_WLAN_INIT, (uint32_t)err);
    }
    say(&d->out, "drv: up, mac %M\n", s->mac);
}

__attribute__((noreturn)) void driver_main(struct child_page *page)
{
    struct drv_page *s = (struct drv_page *)page;
    struct drv drv, *d = &drv;
    memset(d, 0, sizeof(*d));
    d->page = s;
    d->window = NO_FRAME;
    d->chip.sleep = drv_sleep;
    d->out = child_out();
    d->wlan.chip = &d->chip;
    d->wlan.on_event = on_event;
    d->wlan.on_data = on_data;

    boot(d);
    report(d, DRV_UP);
    int32_t err;
    switch (s->mode) {
    case MODE_AP:
        if ((err = wlan_start_ap(&d->wlan, s->ssid, s->pass, s->channel)) != 0) {
            fail(d, STEP_AP, (uint32_t)err);
        }
        report(d, DRV_AP);
        serve(d);
    case MODE_STA:
        /* At once: a scan first would take the run's seconds, and the firmware finds the network itself. */
        if (s->dhcp_log) {
            wlan_power_save_off(&d->wlan);
        }
        if ((err = wlan_join(&d->wlan, s->ssid, s->pass, s->bssid_set ? s->bssid : 0)) != 0) {
            fail(d, STEP_JOIN, (uint32_t)err);
        }
        wait_for(d, &d->joined, 10000);
        if (d->joined != 1) {
            fail(d, STEP_JOIN, (uint32_t)d->joined);
        }
        uint8_t ap[6] = { 0 };
        wlan_bssid(&d->wlan, ap);
        say(&d->out, "drv: joined %M\n", ap);
        report(d, DRV_JOINED);
        serve(d);
    default:
        if ((err = wlan_scan(&d->wlan)) != 0) {
            fail(d, STEP_SCAN, (uint32_t)err);
        }
        wait_for(d, &d->scan_done, 8000);
        say(&d->out, "drv: scan %s, %u results\n", d->scan_done ? "done" : "timed out", d->results);
        stop(d, DRV_SCANNED);
    }
}
