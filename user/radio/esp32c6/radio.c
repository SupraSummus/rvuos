/*
 * What every program of the radio's is around its own run: its entry and first thread, its text to the kernel's log,
 * its events, and the steps that must succeed; see osi.h.
 *
 * The root task starts a program as a child at drv_main, with a0 at its page and its stack in the child's data.
 * The thread fills the image's data, makes its account of what it may build of its own,
 * starts the adapter, joins its threads as one of theirs, since it calls the radio's code too,
 * and runs the program, drv_run, which never returns.
 */

#include <stdarg.h>
#include <stdint.h>

#include "drv.h"
#include "esp.h"
#include "lib/libc.h"
#include "lib/lock.h"
#include "osi.h"
#include "rvuos.h"

/* The image's own layout, from its linker script, which esptool writes into the window onto flash. */
extern uint8_t drv_data_load[], drv_data_start[], drv_data_end[], drv_bss_start[], drv_bss_end[];

struct drv *drv_self;
struct event_group drv_events;
static struct thread *first_thread; /* the program's first, which alone waits on its inbox */
static struct self own;
static struct lock say_lock;
static uint32_t say_word;

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
    if (!streq(base, "WIFI_EVENT") || (uint32_t)id >= 32) {
        return;
    }
    osi_event_set_bits(&drv_events, 1u << id);
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

/* A step that must succeed; one that fails stops the calling thread, and the page says which. */
void drv_must(const char *step, esp_err_t e)
{
    if (e != ESP_OK) {
        drv_failed(step, (uint32_t)e);
        drv_radio_off();
        drv_stop(CHILD_FAILED);
    }
    drv_say("driver: %s\n", step);
}

/* A step of mac.c's that must succeed: 0, or the step that failed, which stops the program as drv_must does. */
void drv_must_mac(const char *step, const char *failed)
{
    drv_must(failed ? failed : step, failed ? ESP_FAIL : ESP_OK);
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
    /* This thread calls the adapter too, so it is one of its threads, on the stack the root task gave it. */
    if (!osi_init(d, &own) ||
        (first_thread = osi_adopt("driver", (uintptr_t)page, (uintptr_t)page + DRV_DATA_SIZE)) == 0) {
        child_stop(&d->c, CHILD_FAILED);
    }
    drv_run(d, &own);
}
