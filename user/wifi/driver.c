/*
 * The driver of the CYW43439, a process of its own; see wifi.h for what it holds.
 *
 * It starts with a0 at the page it shares with the root task, which says where everything else lies,
 * keeps its state at the base of its data frame and its stack at the top,
 * and touches no global, since the globals are the root task's.
 * The firmware blob is lent as frames that together are larger than the regions left,
 * so the driver installs them one at a time in a region of its own, its window, and uploads what each holds.
 */

#include <stddef.h>

#include "cyw43.h"
#include "lib.h"
#include "rvuos.h"
#include "wifi.h"
#include "wlan.h"

#define NO_FRAME 0xffffffffu

struct drv {
    struct cyw43 chip; /* first, so that the chip's sleep finds the driver */
    struct drv_shared *shared;
    struct out out;
    uint32_t pending;  /* bits that came while the driver slept */
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

/* A scan's result goes into the page, the strongest of each name; the last event says the scan is done. */
static void on_event(struct wlan *w, const struct wlan_event *e)
{
    struct drv *d = drv_of(w);
    struct drv_shared *s = d->shared;
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
    if (bss.ssid_len == 0) {
        return;
    }
    for (uint32_t i = 0; i < s->net_count; i++) {
        if (memcmp(s->nets[i].ssid, bss.ssid, bss.ssid_len + 1u) == 0) {
            if (bss.rssi > s->nets[i].rssi) {
                s->nets[i].rssi = bss.rssi;
                s->nets[i].channel = bss.channel;
            }
            return;
        }
    }
    if (s->net_count < DRV_NETS) {
        struct drv_net *n = &s->nets[s->net_count++];
        memcpy(n->ssid, bss.ssid, sizeof(n->ssid));
        n->rssi = bss.rssi;
        n->channel = bss.channel;
    }
}

static void drv_sleep(struct cyw43 *chip, uint32_t us)
{
    struct drv *d = (struct drv *)chip;
    rv_timer_set(DRV_TIMER, DRV_BIT_TIMER, us);
    for (;;) {
        uint32_t bits = 0;
        if (rv_wait(DRV_INBOX, &bits) != KERR_OK) {
            rv_breakpoint();
        }
        d->pending |= bits & ~DRV_BIT_TIMER;
        if (bits & DRV_BIT_TIMER) {
            return;
        }
    }
}

/* A byte of text into the shared page's ring, and a line to the root task. */
static void drv_put(void *to, char c)
{
    struct drv_shared *s = ((struct drv *)to)->shared;
    uint32_t head = s->log_head;
    s->log[head % DRV_LOG_SIZE] = c;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s->log_head = head + 1;
    if (c == '\n') {
        rv_signal(DRV_ROOT, ROOT_BIT_DRIVER);
    }
}

/* The address of the blob's byte at off, with the frame holding it installed, and how many follow in that frame. */
static const uint8_t *blob_at(struct drv *d, uint32_t off, uint32_t *left)
{
    const struct drv_shared *s = d->shared;
    uint32_t start = 0;
    for (uint32_t i = 0; i < s->blob_frames; i++) {
        uint32_t size = s->blob_frame_size[i];
        if (off < start + size) {
            if (d->window != i) {
                if (d->window != NO_FRAME) {
                    rv_invoke(OP_PROCESS_UNINSTALL, DRV_SELF, DRV_REGION_WINDOW, 0, 0);
                }
                if (rv_invoke(OP_PROCESS_INSTALL, DRV_SELF, DRV_REGION_WINDOW, DRV_FIRMWARE + i, RIGHT_R) != KERR_OK) {
                    rv_breakpoint();
                }
                d->window = i;
            }
            *left = start + size - off;
            return (const uint8_t *)(s->blob_base + off);
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

static void report(struct drv *d, uint32_t state)
{
    struct drv_shared *s = d->shared;
    s->step = d->chip.step;
    s->detail = d->chip.detail;
    s->state = state;
    rv_signal(DRV_ROOT, ROOT_BIT_DRIVER);
}

/* Keeps reading what the firmware sends, so that its events do not pile up, for as long as the system runs. */
static __attribute__((noreturn)) void listen(struct drv *d)
{
    for (;;) {
        if (wlan_poll(&d->wlan) == 0) {
            drv_sleep(&d->chip, 1000);
        }
    }
}

static __attribute__((noreturn)) void stop(struct drv *d, uint32_t state)
{
    report(d, state);
    for (;;) {
        uint32_t bits;
        rv_wait(DRV_INBOX, &bits);
    }
}

void driver_main(struct drv_shared *s);
__attribute__((noreturn)) void driver_main(struct drv_shared *s)
{
    struct drv *d = (struct drv *)s->data_base;
    memset(d, 0, sizeof(*d));
    d->shared = s;
    d->window = NO_FRAME;
    d->chip.sleep = drv_sleep;
    d->out = (struct out){ drv_put, d };

    print(&d->out, "drv: up\n");
    gspi_init(&d->chip.bus, s->pio_base, s->pins_base);
    if (cyw43_bus_up(&d->chip) != 0) {
        stop(d, DRV_FAILED);
    }
    print(&d->out, "drv: bus up\n");
    if (cyw43_prepare(&d->chip) != 0) {
        stop(d, DRV_FAILED);
    }
    print(&d->out, "drv: chip 43439, loading firmware\n");

    uint32_t left;
    const struct blob_header *h = (const struct blob_header *)blob_at(d, 0, &left);
    struct blob_header hdr = *h;
    if (hdr.magic != BLOB_MAGIC) {
        d->chip.step = STEP_FW_VERIFY;
        d->chip.detail = hdr.magic;
        stop(d, DRV_FAILED);
    }
    /* The check reads back the first KiB and the last bytes, from a word: the chip aligns the address it is given. */
    uint32_t tail = (hdr.fw_size - 256u) & ~3u;
    if (upload(d, hdr.fw_offset, hdr.fw_size, 0, 0) != 0 ||
        upload(d, hdr.fw_offset, 1024, 0, 1) != 0 ||
        upload(d, hdr.fw_offset + tail, hdr.fw_size - tail, tail, 1) != 0) {
        stop(d, DRV_FAILED);
    }
    print(&d->out, "drv: firmware loaded, ");
    print_dec(&d->out, hdr.fw_size);
    print(&d->out, " bytes\n");

    const uint8_t *nvram = blob_at(d, hdr.nvram_offset, &left);
    if (nvram == 0 || left < ((hdr.nvram_size + 3u) & ~3u) || cyw43_start(&d->chip, nvram, hdr.nvram_size) != 0) {
        stop(d, DRV_FAILED);
    }
    print(&d->out, "drv: firmware runs\n");

    d->wlan.chip = &d->chip;
    d->wlan.on_event = on_event;
    const uint8_t *clm = blob_at(d, hdr.clm_offset, &left);
    if (clm == 0 || left < hdr.clm_size) {
        stop(d, DRV_FAILED);
    }
    int32_t err = wlan_init(&d->wlan, clm, hdr.clm_size, s->mac);
    if (err != 0) {
        d->chip.step = STEP_WLAN_INIT;
        d->chip.detail = (uint32_t)err;
        stop(d, DRV_FAILED);
    }
    print(&d->out, "drv: up, mac ");
    for (uint32_t i = 0; i < 6; i++) {
        static const char hex[] = "0123456789abcdef";
        char b[4] = { hex[s->mac[i] >> 4], hex[s->mac[i] & 15], i < 5 ? ':' : '\n', 0 };
        print(&d->out, b);
    }
    report(d, DRV_UP);

    if (s->mode == MODE_AP) {
        if ((err = wlan_start_ap(&d->wlan, s->ssid, s->pass, s->channel)) != 0) {
            d->chip.step = STEP_AP;
            d->chip.detail = (uint32_t)err;
            stop(d, DRV_FAILED);
        }
        report(d, DRV_AP);
        listen(d);
    }

    if ((err = wlan_scan(&d->wlan)) != 0) {
        d->chip.step = STEP_SCAN;
        d->chip.detail = (uint32_t)err;
        stop(d, DRV_FAILED);
    }
    for (uint32_t ms = 0; !d->scan_done && ms < 8000u; ms++) {
        if (wlan_poll(&d->wlan) == 0) {
            drv_sleep(&d->chip, 1000);
        }
    }
    print(&d->out, d->scan_done ? "drv: scan done, " : "drv: scan timed out, ");
    print_dec(&d->out, d->results);
    print(&d->out, " results\n");
    if (s->mode != MODE_STA) {
        stop(d, DRV_SCANNED);
    }
    report(d, DRV_SCANNED);

    if ((err = wlan_join(&d->wlan, s->ssid, s->pass)) != 0) {
        d->chip.step = STEP_JOIN;
        d->chip.detail = (uint32_t)err;
        stop(d, DRV_FAILED);
    }
    for (uint32_t ms = 0; d->joined == 0 && ms < 10000u; ms++) {
        if (wlan_poll(&d->wlan) == 0) {
            drv_sleep(&d->chip, 1000);
        }
    }
    if (d->joined != 1) {
        d->chip.step = STEP_JOIN;
        d->chip.detail = (uint32_t)d->joined;
        stop(d, DRV_FAILED);
    }
    report(d, DRV_JOINED);
    listen(d);
}
