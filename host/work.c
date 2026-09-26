/*
 * The claims of the kernel's loop annotations, counted; see kernel/work.h.
 *
 * Compiled into the harness fuzz-work only, whose kernel objects are built with RVUOS_WORK,
 * so that each loop annotation calls work_step, and with -finstrument-functions,
 * so that every function the kernel enters, inlined or not, opens a frame here.
 * A loop is entered anew when it steps in a frame where it is not open;
 * a step of a loop that is open closes the loops above it, which have ended.
 * Two loops of one frame are open together only when one holds the other,
 * so this is the entry the source means, however the compiler laid the loops out.
 *
 * After every call the harness checks:
 * a bounded loop ran no more than its bound on any one entry,
 * the steps of the loops paid for in each unit are at most twice
 * what the call took away of it, plus what a call may make of it on the way,
 * twice, since a delete below a root makes a root of a node, a link each,
 * and a pool destroy may then clear that node as one its tables hold.
 * What there is of each unit is counted over every object by host_units.
 *
 * Two claims are checked as they are made.
 * No paid loop steps twice without asking intr_pending between, a call's start counting as asked.
 * And the kernel's memset and memcpy, work_memset and work_memcpy here,
 * take no more than the CALL_BOUND before them says.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

#define WORK_DEPTH 64
#define WORK_OPEN 8
/* More than the kernel has paid loops, each stepped at most once since the last question. */
#define WORK_PAID_SITES 16

/*
 * What one call may make of a unit while it takes others away:
 * OP_IRQ_BIND makes an Irq and its capability, one object and one node,
 * as it revokes what was derived from the line.
 */
#define WORK_SLACK 1

static const char *const unit_names[UNITS] = { "node", "link", "object", "waiter" };

struct work_frame {
    const struct work_site *site[WORK_OPEN];
    unsigned count[WORK_OPEN];
    unsigned open;
    /* The CALL_BOUND said for the next memset or memcpy this frame makes; site NULL for none. */
    unsigned long call_bound;
    const char *call_site;
};

static struct work_frame frames[WORK_DEPTH];
static unsigned depth;
static unsigned paid[UNITS];
static unsigned before[UNITS];
/* The paid loops that stepped since intr_pending was last asked. */
static const struct work_site *unasked[WORK_PAID_SITES];
static unsigned unasked_count;

void __cyg_profile_func_enter(void *fn, void *site) __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *fn, void *site) __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)fn;
    (void)site;
    if (depth < WORK_DEPTH) {
        frames[depth].open = 0;
        frames[depth].call_site = NULL;
    }
    depth++;
}

void __cyg_profile_func_exit(void *fn, void *site)
{
    (void)fn;
    (void)site;
    if (depth > 0) {
        depth--;
    }
}

__attribute__((noreturn, format(printf, 1, 2))) static void violated(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "invariant violated: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    host_violated();
}

static unsigned unit_of(const char *name)
{
    for (unsigned u = 0; u < UNITS; u++) {
        if (strcmp(unit_names[u], name) == 0) {
            return u;
        }
    }
    violated("a paid loop counts in %s, a unit the harness does not know", name);
}

void work_step(const struct work_site *s)
{
    if (depth == 0 || depth > WORK_DEPTH) {
        violated("the loop at %s steps outside the frames the harness follows", s->site);
    }
    struct work_frame *f = &frames[depth - 1];
    unsigned i = 0;
    while (i < f->open && f->site[i] != s) {
        i++;
    }
    if (i < f->open) {
        f->open = i + 1;
        f->count[i]++;
    } else {
        if (f->open == WORK_OPEN) {
            violated("the loop at %s is nested deeper than the harness follows", s->site);
        }
        f->site[f->open] = s;
        f->count[f->open] = 1;
        f->open++;
    }
    if (s->kind == 'b' && f->count[i] > s->bound) {
        violated("the loop at %s ran %u times on one entry, beyond its bound of %u", s->site,
                 f->count[i], s->bound);
    }
    if (s->kind == 'p') {
        paid[unit_of(s->unit)]++;
        for (unsigned k = 0; k < unasked_count; k++) {
            if (unasked[k] == s) {
                violated("the paid loop at %s took a second step without asking intr_pending", s->site);
            }
        }
        if (unasked_count == WORK_PAID_SITES) {
            violated("the loop at %s is one paid loop more than the harness follows", s->site);
        }
        unasked[unasked_count++] = s;
    }
}

void work_ask(void)
{
    unasked_count = 0;
}

void work_call(unsigned long bound, const char *site)
{
    if (depth == 0 || depth > WORK_DEPTH) {
        violated("the call at %s is made outside the frames the harness follows", site);
    }
    frames[depth - 1].call_bound = bound;
    frames[depth - 1].call_site = site;
}

/* Host code, and kernel code off every trap's path, calls with no bound said; the link checks the rest. */
static void work_length(size_t n)
{
    if (depth == 0 || depth > WORK_DEPTH) {
        return;
    }
    struct work_frame *f = &frames[depth - 1];
    if (f->call_site != NULL && n > f->call_bound) {
        violated("the call at %s passes %zu, beyond its bound of %lu", f->call_site, n, f->call_bound);
    }
    f->call_site = NULL;
}

void *work_memset(void *dst, int c, size_t n)
{
    work_length(n);
    return memset(dst, c, n);
}

void *work_memcpy(void *dst, const void *src, size_t n)
{
    work_length(n);
    return memcpy(dst, src, n);
}

void work_begin(void)
{
    depth = 0;
    unasked_count = 0;
    memset(paid, 0, sizeof(paid));
    host_units(before);
}

void work_reset(void)
{
    depth = 0;
}

void work_end(void)
{
    unsigned after[UNITS];
    host_units(after);
    for (unsigned u = 0; u < UNITS; u++) {
        unsigned gone = before[u] > after[u] ? before[u] - after[u] : 0;
        if (paid[u] > 2 * (gone + WORK_SLACK)) {
            violated("a call took %u paid steps in %s and took away %u", paid[u], unit_names[u], gone);
        }
    }
}
