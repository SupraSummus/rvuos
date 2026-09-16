/*
 * Kernel invariants, executable form.
 * The list mirrors DESIGN.md, "Properties"; keep the two in step.
 *
 * The checker trusts nothing the kernel computed except pool_list
 * and the boot grant table:
 * it walks pools and objects itself,
 * recomputes what PMP would grant from the region slots,
 * and compares against the image the kernel built
 * and the CSR values read back from the hardware.
 *
 * It runs after every system call in the host fuzzer
 * and, once OP_DEBUG_TRACE has been invoked, on the target as well.
 * It is freestanding so that the same file serves both.
 */

#include "kernel.h"
#include "object.h"
#include "pmp.h"

static __attribute__((noreturn)) void fail(const char *msg, uint32_t a, uint32_t b, uint32_t c)
{
    kputs("invariant violated: ");
    kputs(msg);
    kputc(' ');
    kput_hex(a);
    kputc(' ');
    kput_hex(b);
    kputc(' ');
    kput_hex(c);
    kputc('\n');
    selfcheck_fail();
}

/* Shell sort; the arrays are small and no libc is assumed. */
static void sort_u64(uint64_t *v, size_t n)
{
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            uint64_t x = v[i];
            size_t j = i;
            while (j >= gap && v[j - gap] > x) {
                v[j] = v[j - gap];
                j -= gap;
            }
            v[j] = x;
        }
    }
}

/*
 * Rights the root task received at boot for a range,
 * or zero if the range is not within a single granted range.
 * No operation joins regions,
 * so every region a process can name lies within one of them.
 */
static uint8_t granted_rights(uint32_t base, uint32_t size)
{
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        const struct granted_range *g = &boot_granted[i];
        uint32_t off = base - g->base;
        if (g->size != 0 && base >= g->base && off < g->size && size <= g->size - off) {
            return g->rights;
        }
    }
    return 0;
}

static uint32_t aligned_size(const struct obj_header *o)
{
    return ((uint32_t)obj_size(o) + OBJ_ALIGN - 1) & ~(uint32_t)(OBJ_ALIGN - 1);
}

static void check_pools(void)
{
    /* The boot pool is kernel memory; nothing granted may reach it. */
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        const struct granted_range *g = &boot_granted[i];
        if (g->size && ranges_overlap(g->base, g->size, boot_pool->base, boot_pool->size)) {
            fail("granted memory overlaps the boot pool", g->base, g->size, 0);
        }
    }

    for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
        if (p->hdr.type != CAP_POOL || obj_pool(&p->hdr) != p || v2p(p) != p->base) {
            fail("pool descriptor is malformed", v2p(p), 0, 0);
        }
        if (p->used < sizeof(*p) || p->used > p->size) {
            fail("pool used mark outside the pool", p->base, p->used, p->size);
        }
        if ((p->base | p->size) & (OBJ_ALIGN - 1)) {
            fail("pool is not aligned", p->base, p->size, 0);
        }
        if (p != boot_pool && granted_rights(p->base, p->size) != RIGHT_ALL) {
            fail("pool lies outside memory granted with full rights", p->base, p->size, 0);
        }
        for (struct pool *q = pool_next_pool(p); q != NULL; q = pool_next_pool(q)) {
            if (ranges_overlap(p->base, p->size, q->base, q->size)) {
                fail("pools overlap", p->base, q->base, 0);
            }
        }

        /*
         * Objects tile the pool from the descriptor to the used mark.
         * This is the one walk that does not use object_first/object_next:
         * those cross pools by an object's own header,
         * which is what this loop is here to check.
         */
        paddr_t at = p->base + aligned_size(&p->hdr);
        for (struct obj_header *o = pool_first(p); o != NULL; o = pool_next(p, o)) {
            if (v2p(o) != at) {
                fail("object walk skipped", p->base, at, v2p(o));
            }
            if (obj_pool(o) != p) {
                fail("object claims another pool", at, o->pool, p->base);
            }
            if (o->type != CAP_CAPTABLE && o->type != CAP_PROCESS &&
                o->type != CAP_THREAD && o->type != CAP_NOTIFICATION) {
                fail("object has an unexpected type", at, o->type, 0);
            }
            at += aligned_size(o);
            if (at > p->base + p->used) {
                fail("object runs past the used mark", v2p(o), at, p->base + p->used);
            }
        }
        if (at != p->base + p->used) {
            fail("objects do not end at the used mark", p->base, at, p->base + p->used);
        }
    }
}

/* Rights PMP grants at addr under a TOR image: the lowest matching entry wins. */
static uint8_t image_rights(const uint32_t *addr, const uint8_t *cfg, unsigned count, uint64_t at)
{
    uint64_t lo = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t hi = addr[i];
        if ((cfg[i] & 0x18) == PMP_A_TOR && at >= lo && at < hi) {
            return cfg[i] & (PMP_R | PMP_W | PMP_X);
        }
        lo = hi;
    }
    return 0;
}

static uint8_t slot_rights(const struct process *proc, uint64_t at)
{
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct region_slot *s = &proc->slots[i];
        if (s->rights && at >= s->base && at < (uint64_t)s->base + s->size) {
            return rights_to_pmp(s->rights);
        }
    }
    return 0;
}

/*
 * The PMP image and the region slots describe the same function
 * from address to rights.
 * Both are piecewise constant, so comparing at every breakpoint
 * and between consecutive breakpoints is exact.
 * `csrs` says whether the image was read back from the CSRs,
 * for the report only.
 */
static void check_pmp_image(const struct process *proc, const uint32_t *addr,
                            const uint8_t *cfg, unsigned count, bool csrs)
{
    for (unsigned i = 0; i < count; i++) {
        uint8_t mode = cfg[i] & 0x18;
        if (mode != PMP_A_OFF && mode != PMP_A_TOR) {
            fail("PMP entry uses a mode other than OFF and TOR", i, mode, csrs);
        }
        if ((cfg[i] & PMP_W) && !(cfg[i] & PMP_R)) {
            fail("PMP entry uses the reserved encoding R=0 W=1", i, cfg[i], csrs);
        }
    }

    uint64_t points[2 * PROCESS_REGION_SLOTS + PMP_MAX_ENTRIES + 2];
    size_t n = 0;
    points[n++] = 0;
    points[n++] = 0x100000000ull;
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        if (proc->slots[i].rights) {
            points[n++] = proc->slots[i].base;
            points[n++] = (uint64_t)proc->slots[i].base + proc->slots[i].size;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        points[n++] = addr[i];
    }
    sort_u64(points, n);

    for (size_t i = 0; i + 1 < n; i++) {
        uint64_t a = points[i], b = points[i + 1];
        if (a == b) {
            continue;
        }
        uint64_t probes[2] = { a, a + (b - a) / 2 };
        for (int k = 0; k < 2; k++) {
            uint8_t want = slot_rights(proc, probes[k]);
            uint8_t got = image_rights(addr, cfg, count, probes[k]);
            if (want != got) {
                fail(csrs ? "PMP CSRs grant rights the slots do not"
                          : "PMP image grants rights the slots do not",
                     (uint32_t)probes[k], got, want);
            }
        }
    }
}

static void check_process(const struct process *proc)
{
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct region_slot *s = &proc->slots[i];
        if (s->rights == 0) {
            continue;
        }
        uint8_t granted = granted_rights(s->base, s->size);
        if (s->size == 0 || granted == 0) {
            fail("process maps memory outside what was granted", v2p(proc), s->base, s->size);
        }
        if (s->rights & ~granted) {
            fail("process maps memory with rights beyond the grant", v2p(proc), s->base, s->rights);
        }
        for (struct pool *p = pool_list; p != NULL; p = pool_next_pool(p)) {
            if (ranges_overlap(s->base, s->size, p->base, p->size)) {
                fail("process maps a pool: isolation broken", v2p(proc), s->base, p->base);
            }
        }
        for (unsigned j = i + 1; j < PROCESS_REGION_SLOTS; j++) {
            const struct region_slot *t = &proc->slots[j];
            if (t->rights && ranges_overlap(s->base, s->size, t->base, t->size)) {
                fail("process has overlapping region slots", v2p(proc), i, j);
            }
        }
    }

    if (proc->pmp.count > pmp_entry_count) {
        fail("PMP image has more entries than the hardware", v2p(proc), proc->pmp.count, 0);
    }
    check_pmp_image(proc, proc->pmp.addr, proc->pmp.cfg, proc->pmp.count, false);

    struct obj_header *t = object_find(proc->ctable);
    if (t == NULL || t->type != CAP_CAPTABLE) {
        fail("process has no live capability table", v2p(proc), proc->ctable, 0);
    }
    if (t->pool != proc->hdr.pool) {
        fail("process and its table live in different pools", v2p(proc), 0, 0);
    }
}

static void check_thread(const struct thread *t)
{
    struct obj_header *p = object_find(t->proc);
    if (p == NULL || p->type != CAP_PROCESS) {
        fail("thread has no live process", v2p(t), t->proc, 0);
    }
    if (p->pool != t->hdr.pool) {
        fail("thread and its process live in different pools", v2p(t), 0, 0);
    }
    if (t->flags & ~(uint8_t)THREAD_UNTRACED) {
        fail("thread has unknown flags", v2p(t), t->flags, 0);
    }
    switch (t->state) {
    case THREAD_STOPPED:
    case THREAD_READY:
        if (t->waiting_on != 0) {
            fail("runnable thread waits on something", v2p(t), t->waiting_on, t->state);
        }
        break;
    case THREAD_WAITING: {
        struct obj_header *n = object_find(t->waiting_on);
        if (n == NULL || n->type != CAP_NOTIFICATION) {
            fail("thread waits on something that is not a notification",
                 v2p(t), t->waiting_on, 0);
        }
        break;
    }
    default:
        fail("thread is in an unknown state", v2p(t), t->state, 0);
    }
}

static void check_captable(const struct captable *table)
{
    if (table->nslots == 0 || table->nslots > CAPTABLE_MAX_SLOTS) {
        fail("table has an impossible slot count", v2p(table), table->nslots, 0);
    }
    for (uint32_t i = 0; i < table->nslots; i++) {
        const struct cap *c = &table->slots[i];
        if (c->type == CAP_NONE) {
            continue;
        }
        if (c->rights & ~RIGHT_ALL) {
            fail("capability has unknown rights bits", v2p(table), i, c->rights);
        }
        switch (c->type) {
        case CAP_DEBUG:
            break;
        case CAP_REGION: {
            uint8_t granted = granted_rights(c->a, c->b);
            if (c->b == 0 || granted == 0) {
                fail("region capability outside granted memory", v2p(table), i, c->a);
            }
            if (c->rights & ~granted) {
                fail("region capability with rights beyond the grant", v2p(table), i, c->rights);
            }
            break;
        }
        case CAP_POOL:
        case CAP_CAPTABLE:
        case CAP_PROCESS:
        case CAP_THREAD:
        case CAP_NOTIFICATION: {
            /* Until pool destroy exists every object capability points at a live object. */
            struct obj_header *o = object_find(c->a);
            if (o == NULL || o->type != c->type) {
                fail("dangling object capability", v2p(table), i, c->a);
            }
            break;
        }
        default:
            fail("capability of unknown type", v2p(table), i, c->type);
        }
    }
}

void selfcheck_run(void)
{
    check_pools();

    /* check_pools() ran first, so the flat walk rests on checked headers. */
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        switch (o->type) {
        case CAP_PROCESS:
            check_process((struct process *)o);
            break;
        case CAP_THREAD:
            check_thread((struct thread *)o);
            break;
        case CAP_CAPTABLE:
            check_captable((struct captable *)o);
            break;
        default:
            break;
        }
    }

    if (current != NULL) {
        struct obj_header *o = object_find(v2p(current));
        if (o == NULL || o->type != CAP_THREAD) {
            fail("current thread is not a live object", v2p(current), 0, 0);
        }
        if (current->state != THREAD_READY) {
            fail("the running thread is not runnable", v2p(current), current->state, 0);
        }
        /* The CSRs must carry exactly the current process's image. */
        uint32_t addr[PMP_MAX_ENTRIES];
        uint8_t cfg[PMP_MAX_ENTRIES];
        for (unsigned i = 0; i < pmp_entry_count; i++) {
            pmp_get(i, &addr[i], &cfg[i]);
        }
        check_pmp_image(thread_process(current), addr, cfg, pmp_entry_count, true);
    }
}
