/*
 * Capability invocation.
 * The ABI is documented in include/rvuos/abi.h.
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "timer.h"

#define POOL_MIN_SIZE 64

/*
 * Not a status code: the thread blocked and is not returning yet.
 * Its registers are left alone until something wakes it.
 */
#define KERR_BLOCKED (-1)

bool debug_trace;

/* The slot itself, as a node of the derivation tree, once cap_lookup has vetted the index. */
static struct cap *slot_node(struct thread *t, uint32_t slot)
{
    return &thread_table(t)->slots[slot];
}

/* True while a node lies in a live pool; a destroy may have taken the table it was in. */
static bool node_live(const struct cap *n)
{
    return pool_overlaps(v2p(n), sizeof(*n));
}

static int op_debug(uint32_t op, const uint32_t *arg)
{
    switch (op) {
    case OP_DEBUG_PUTC:
        kputc((char)arg[1]);
        return KERR_OK;
    case OP_DEBUG_HALT:
        kputs("user halt with code ");
        kput_hex(arg[1]);
        kputc('\n');
        khalt((int)arg[1]);
    case OP_DEBUG_TRACE:
        debug_trace = true;
        return KERR_OK;
    case OP_DEBUG_TICK:
        /* The caller's status is written to its own frame, whoever runs next. */
        sched_tick();
        return KERR_OK;
    case OP_DEBUG_IRQ:
        /* The log's line is not a device's: its level is the log's, and no record fires it. */
        if (arg[1] == LOG_IRQ_LINE || arg[1] >= IRQ_LINES) {
            return KERR_INVALID_ARG;
        }
        return sched_interrupt(arg[1]) ? KERR_OK : KERR_STATE;
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_captable(struct thread *t, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    struct captable *table = (struct captable *)cap_object(cap);

    switch (op) {
    case OP_CAP_COPY:
    case OP_CAP_DERIVE: {
        struct cap src;
        int err = cap_lookup(thread_table(t), arg[2], &src);
        if (err != KERR_OK) {
            return err;
        }
        src.rights &= (uint8_t)arg[3];
        /* A copy stands beside its source in the tree, a derivation below it. */
        if (op == OP_CAP_COPY) {
            return cap_store_beside(table, arg[1], &src, slot_node(t, arg[2]));
        }
        return cap_store(table, arg[1], &src, slot_node(t, arg[2]));
    }
    case OP_CAP_DELETE:
        return cap_clear(table, arg[1]);
    case OP_CAP_REVOKE: {
        struct cap c;
        int err = cap_lookup(table, arg[1], &c);
        if (err != KERR_OK) {
            return err;
        }
        cap_revoke_below(&table->slots[arg[1]]);
        return KERR_OK;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in and the results out. */
static int op_region(struct thread *t, uint32_t slot, const struct cap *cap,
                     uint32_t op, uint32_t *arg)
{
    uint32_t base = cap->a;
    uint32_t size = cap->b;

    switch (op) {
    case OP_REGION_INFO:
        arg[1] = base;
        arg[2] = size;
        arg[3] = cap->rights;
        arg[4] = region_min_size();
        return KERR_OK;
    case OP_REGION_CARVE: {
        uint32_t off = arg[1];
        uint32_t len = arg[2];
        /* A block within the region, so that every region capability is one NAPOT entry. */
        if (off >= size || len > size - off || !napot_block(base + off, len)) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = cap_to_region(base + off, len, cap->rights);
        return cap_store(thread_table(t), arg[3], &sub, slot_node(t, slot));
    }
    case OP_REGION_TO_POOL: {
        if ((cap->rights & (RIGHT_R | RIGHT_W)) != (RIGHT_R | RIGHT_W)) {
            return KERR_NO_RIGHTS;
        }
        /*
         * Kernel objects live in RAM: on a device range the zeroing below would drive registers.
         * The log is RAM the kernel writes on its own, so it cannot hold them either.
         * A region is a block aligned to its size, so one this large lies on OBJ_ALIGN.
         */
        _Static_assert(POOL_MIN_SIZE % OBJ_ALIGN == 0, "a pool-sized block lies on OBJ_ALIGN");
        if (size < POOL_MIN_SIZE || !ram_contains(base, size) ||
            ranges_overlap(base, size, KLOG_BASE, KLOG_REGION_SIZE)) {
            return KERR_INVALID_ARG;
        }
        if (pool_overlaps(base, size) || installed_overlaps(base, size)) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the pool capability. */
        if (arg[1] != slot) {
            int err = cap_slot_free(thread_table(t), arg[1]);
            if (err != KERR_OK) {
                return err;
            }
        }
        /* From here on the memory is the kernel's. */
        memset(p2v(base), 0, size);
        /* The new pool hangs below the caller's own, and dies with it. */
        struct pool *pool = pool_create(base, size, cap->rights, obj_pool(&t->hdr));
        struct cap pc = cap_to_object(&pool->hdr, RIGHT_ALL);
        /* The pool capability takes the region's place in the tree; the region and its derivation go. */
        struct cap *parent = cap_parent(slot_node(t, slot));
        cap_revoke(slot_node(t, slot));
        return cap_store(thread_table(t), arg[1], &pc, parent);
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * The notification a Timer or an Irq is bound to, from a slot in the caller's table.
 * The object signals on the creator's behalf, so the creator must be able to,
 * and the link is not checked on use, so the two must lie in one pool;
 * see DESIGN.md, "Kernel pools and revocation".
 */
static int bound_notification(struct thread *t, uint32_t slot, const struct pool *pool,
                              struct notification **out)
{
    struct cap nc;
    int err = cap_lookup_typed(thread_table(t), slot, CAP_NOTIFICATION, RIGHT_W, &nc);
    if (err != KERR_OK) {
        return err;
    }
    *out = (struct notification *)cap_object(&nc);
    return (*out)->hdr.pool == pool->base ? KERR_OK : KERR_INVALID_ARG;
}

static int op_pool_destroy(struct thread *t, uint32_t slot, struct pool *pool,
                           const uint32_t *arg)
{
    uint32_t base = pool->base;
    uint32_t size = pool->size;
    uint8_t rights = pool->rights;

    /*
     * A thread, its process and its table share one pool,
     * and the destroy takes every pool below this one,
     * so one check on the thread's pool covers everything the caller runs on.
     */
    if (pool_under(obj_pool(&t->hdr), pool)) {
        return KERR_STATE;
    }
    /* The invoked slot holds the Pool capability and the sweep will clear it. */
    if (arg[1] != slot) {
        int err = cap_slot_free(thread_table(t), arg[1]);
        if (err != KERR_OK) {
            return err;
        }
    }

    /*
     * The memory goes back below the region the pool was made of:
     * the nearest ancestor that is not a capability to this pool,
     * since those the sweep is about to clear.
     * That ancestor may lie in a table the destroy takes, and then the memory returns as a root.
     */
    struct cap *parent = cap_parent(slot_node(t, slot));
    while (parent != NULL && parent->type == CAP_POOL && parent->a == base) {
        parent = cap_parent(parent);
    }

    pool_destroy(pool);

    /* Or it was itself derived from a slot the destroy took, and went with it. */
    if (parent != NULL && (!node_live(parent) || parent->type == CAP_NONE)) {
        parent = NULL;
    }
    /* The pools below gave their memory back to nobody; this one gives it to the caller. */
    struct cap rc = cap_to_region(base, size, rights);
    return cap_store(thread_table(t), arg[1], &rc, parent);
}

static int op_pool(struct thread *t, uint32_t slot, const struct cap *cap,
                   uint32_t op, const uint32_t *arg)
{
    struct pool *pool = (struct pool *)cap_object(cap);

    if (op == OP_POOL_DESTROY) {
        return op_pool_destroy(t, slot, pool, arg);
    }
    if (op != OP_POOL_ALLOC) {
        return KERR_WRONG_TYPE;
    }
    uint32_t dst = arg[2];
    int err = cap_slot_free(thread_table(t), dst);
    if (err != KERR_OK) {
        return err;
    }

    struct obj_header *obj;
    switch (arg[1]) {
    /*
     * A structural pointer is not checked on use,
     * so every object is allocated from the pool its parent lies in
     * and dies with it; see DESIGN.md, "Kernel pools and revocation".
     */
    case CAP_PROCESS: {
        struct cap tc;
        int terr = cap_lookup_typed(thread_table(t), arg[3], CAP_CAPTABLE, RIGHT_W, &tc);
        if (terr != KERR_OK) {
            return terr;
        }
        struct captable *table = (struct captable *)cap_object(&tc);
        if (table->hdr.pool != pool->base) {
            return KERR_INVALID_ARG;
        }
        struct process *proc = pool_alloc(pool, CAP_PROCESS, sizeof(*proc));
        if (proc == NULL) {
            return KERR_NO_MEMORY;
        }
        proc->ctable = v2p(table);
        obj = &proc->hdr;
        break;
    }
    case CAP_THREAD: {
        struct cap pc;
        int perr = cap_lookup_typed(thread_table(t), arg[3], CAP_PROCESS, RIGHT_W, &pc);
        if (perr != KERR_OK) {
            return perr;
        }
        struct process *proc = (struct process *)cap_object(&pc);
        if (proc->hdr.pool != pool->base) {
            return KERR_INVALID_ARG;
        }
        struct thread *nt = pool_alloc(pool, CAP_THREAD, sizeof(*nt));
        if (nt == NULL) {
            return KERR_NO_MEMORY;
        }
        nt->proc = v2p(proc);
        nt->state = THREAD_STOPPED;
        /* Until it is configured while tracing is off, the host cannot follow it. */
        nt->flags = THREAD_UNTRACED;
        obj = &nt->hdr;
        break;
    }
    case CAP_NOTIFICATION: {
        struct notification *ntfn = pool_alloc(pool, CAP_NOTIFICATION, sizeof(*ntfn));
        if (ntfn == NULL) {
            return KERR_NO_MEMORY;
        }
        obj = &ntfn->hdr;
        break;
    }
    case CAP_TIMER: {
        struct notification *ntfn;
        int nerr = bound_notification(t, arg[3], pool, &ntfn);
        if (nerr != KERR_OK) {
            return nerr;
        }
        struct timer *timer = pool_alloc(pool, CAP_TIMER, sizeof(*timer));
        if (timer == NULL) {
            return KERR_NO_MEMORY;
        }
        timer->ntfn = v2p(ntfn);
        obj = &timer->hdr;
        break;
    }
    case CAP_CAPTABLE: {
        uint32_t nslots = arg[3];
        if (nslots == 0 || nslots > CAPTABLE_MAX_SLOTS) {
            return KERR_INVALID_ARG;
        }
        struct captable *table = pool_alloc(
            pool, CAP_CAPTABLE, sizeof(*table) + nslots * sizeof(struct cap));
        if (table == NULL) {
            return KERR_NO_MEMORY;
        }
        table->nslots = nslots;
        obj = &table->hdr;
        break;
    }
    default:
        return KERR_INVALID_ARG;
    }

    /* A new object starts a tree of its own. */
    struct cap c = cap_to_object(obj, RIGHT_ALL);
    return cap_store(thread_table(t), dst, &c, NULL);
}

static int op_process(struct thread *t, const struct cap *cap,
                      uint32_t op, const uint32_t *arg)
{
    struct process *proc = (struct process *)cap_object(cap);

    switch (op) {
    case OP_PROCESS_INSTALL: {
        struct cap region;
        int err = cap_lookup_typed(thread_table(t), arg[2], CAP_REGION, 0, &region);
        if (err != KERR_OK) {
            return err;
        }
        uint8_t rights = (uint8_t)arg[3];
        if (rights == 0 || (rights & ~region.rights) != 0) {
            return KERR_NO_RIGHTS;
        }
        /* PMP reserves R=0 W=1; QEMU drops the W and hardware may do anything. */
        if ((rights & RIGHT_W) && !(rights & RIGHT_R)) {
            return KERR_INVALID_ARG;
        }
        return process_install(proc, arg[1], region.a, region.b, rights, slot_node(t, arg[2]));
    }
    case OP_PROCESS_UNINSTALL:
        return process_uninstall(proc, arg[1]);
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_thread(const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    struct thread *target = (struct thread *)cap_object(cap);

    switch (op) {
    case OP_THREAD_CONFIGURE:
        if (target->state != THREAD_STOPPED) {
            return KERR_STATE;
        }
        target->frame.mepc = arg[1];
        target->frame.regs[REG_SP] = arg[2];
        /*
         * The host build follows a thread only if its code was fixed
         * before tracing began; see DESIGN.md, "Verification".
         */
        if (debug_trace) {
            target->flags |= THREAD_UNTRACED;
        } else {
            target->flags &= (uint8_t)~THREAD_UNTRACED;
        }
        return KERR_OK;
    case OP_THREAD_RESUME:
        if (target->state != THREAD_STOPPED) {
            return KERR_STATE;
        }
        target->state = THREAD_READY;
        return KERR_OK;
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_notification(struct thread *t, const struct cap *cap,
                           uint32_t op, uint32_t *arg)
{
    struct notification *ntfn = (struct notification *)cap_object(cap);

    switch (op) {
    case OP_NOTIFY_SIGNAL: {
        if (!(cap->rights & RIGHT_W)) {
            return KERR_NO_RIGHTS;
        }
        /* A wait never returns zero bits, so signalling none would be a lie. */
        if (arg[1] == 0) {
            return KERR_INVALID_ARG;
        }
        sched_signal(ntfn, arg[1]);
        return KERR_OK;
    }
    case OP_NOTIFY_WAIT:
        if (!(cap->rights & RIGHT_R)) {
            return KERR_NO_RIGHTS;
        }
        if (ntfn->bits != 0) {
            arg[1] = ntfn->bits;
            ntfn->bits = 0;
            return KERR_OK;
        }
        sched_wait(t, ntfn);
        return KERR_BLOCKED;
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_timer(const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    struct timer *timer = (struct timer *)cap_object(cap);

    if (op != OP_TIMER_SET) {
        return KERR_WRONG_TYPE;
    }
    uint32_t bits = arg[1];
    uint32_t us = arg[2];
    if (bits == 0) {
        timer->bits = 0;
        return KERR_OK;
    }
    /*
     * The call lands anywhere within the current tick,
     * so the delay rounded up to whole ticks plus one
     * is the first tick that surely lies past it.
     * The count wraps and timer_due compares with a signed difference,
     * which is exact while a delay stays far below half the count's range.
     */
    uint32_t ticks = us / TIMER_US_PER_TICK + (us % TIMER_US_PER_TICK != 0) + 1;
    timer->deadline = sched_ticks + ticks;
    timer->bits = bits;
    return KERR_OK;
}

/* arg[1..] carry the arguments in. */
static int op_irq_line(struct thread *t, uint32_t slot, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    uint32_t first = cap->a;
    uint32_t count = cap->b;

    switch (op) {
    case OP_IRQ_CARVE: {
        uint32_t off = arg[1];
        uint32_t len = arg[2];
        if (len == 0 || off > count || len > count - off) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = cap_to_lines(first + off, len, cap->rights);
        return cap_store(thread_table(t), arg[3], &sub, slot_node(t, slot));
    }
    case OP_IRQ_BIND: {
        if (!(cap->rights & RIGHT_W)) {
            return KERR_NO_RIGHTS;
        }
        /* One Irq per line, so a binding names one line. */
        if (count != 1) {
            return KERR_INVALID_ARG;
        }
        struct cap pc;
        int err = cap_lookup_typed(thread_table(t), arg[1], CAP_POOL, RIGHT_W, &pc);
        if (err != KERR_OK) {
            return err;
        }
        struct pool *pool = (struct pool *)cap_object(&pc);
        struct notification *ntfn;
        err = bound_notification(t, arg[2], pool, &ntfn);
        if (err != KERR_OK) {
            return err;
        }
        if (line_binding(first) != NULL) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the Irq capability. */
        if (arg[3] != slot) {
            err = cap_slot_free(thread_table(t), arg[3]);
            if (err != KERR_OK) {
                return err;
            }
        }
        struct irq *irq = pool_alloc(pool, CAP_IRQ, sizeof(*irq));
        if (irq == NULL) {
            return KERR_NO_MEMORY;
        }
        irq->ntfn = v2p(ntfn);
        irq->line = first;
        line_irq[first] = v2p(irq);
        /* Its bits are zero, so it is disarmed, and its line stays masked as every line does until a set. */
        struct cap ic = cap_to_object(&irq->hdr, RIGHT_ALL);
        /* The Irq capability takes the line's place in the tree; the line and its derivation go. */
        struct cap *parent = cap_parent(slot_node(t, slot));
        cap_revoke(slot_node(t, slot));
        return cap_store(thread_table(t), arg[3], &ic, parent);
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_irq(const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    struct irq *irq = (struct irq *)cap_object(cap);

    if (op != OP_IRQ_SET) {
        return KERR_WRONG_TYPE;
    }
    /* Armed and unmasked are one state, here and in sched_interrupt. */
    irq->bits = arg[1];
    if (irq->line == LOG_IRQ_LINE) {
        klog_set(irq);
    } else {
        irq_enable(irq->line, irq_armed(irq));
    }
    return KERR_OK;
}

/*
 * One line per traced call, in a fixed format
 * so transcripts from the host build and from QEMU can be compared.
 */
static void trace_call(uint32_t op, uint32_t slot, const uint32_t *arg, int status)
{
    kputs("trace: op=");
    kput_hex(op);
    kputs(" slot=");
    kput_hex(slot);
    kputs(" a1=");
    kput_hex(arg[1]);
    kputs(" a2=");
    kput_hex(arg[2]);
    kputs(" a3=");
    kput_hex(arg[3]);
    kputs(" -> ");
    if (status == KERR_BLOCKED) {
        kputs("blocked");
    } else {
        kput_hex((uint32_t)status);
    }
    kputc('\n');
}

static int dispatch(struct thread *t, uint32_t op, uint32_t slot, uint32_t *arg)
{
    struct cap cap;
    int err = cap_lookup(thread_table(t), slot, &cap);
    if (err != KERR_OK) {
        return err;
    }

    /*
     * Operations that change an object need RIGHT_W on it.
     * Regions, interrupt lines, notifications and the debug capability
     * are checked in their handlers,
     * because which right they need depends on the operation.
     */
    switch (cap.type) {
    case CAP_DEBUG:
        err = op_debug(op, arg);
        break;
    case CAP_REGION:
        err = op_region(t, slot, &cap, op, arg);
        break;
    case CAP_CAPTABLE:
        err = (cap.rights & RIGHT_W) ? op_captable(t, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_POOL:
        err = (cap.rights & RIGHT_W) ? op_pool(t, slot, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_PROCESS:
        err = (cap.rights & RIGHT_W) ? op_process(t, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_THREAD:
        err = (cap.rights & RIGHT_W) ? op_thread(&cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_NOTIFICATION:
        err = op_notification(t, &cap, op, arg);
        break;
    case CAP_TIMER:
        err = (cap.rights & RIGHT_W) ? op_timer(&cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_IRQ_LINE:
        err = op_irq_line(t, slot, &cap, op, arg);
        break;
    case CAP_IRQ:
        err = (cap.rights & RIGHT_W) ? op_irq(&cap, op, arg) : KERR_NO_RIGHTS;
        break;
    default:
        err = KERR_WRONG_TYPE;
        break;
    }

    return err;
}

void syscall_dispatch(struct thread *t)
{
    struct trap_frame *f = &t->frame;
    uint32_t op = f->regs[REG_A7];
    /* a0 is the slot, a1..a3 the arguments and, on return, a1..a4 the results. */
    uint32_t in[5], arg[5];
    for (unsigned i = 0; i < 5; i++) {
        in[i] = arg[i] = f->regs[REG_A0 + i];
    }

    /* ecall is a 4-byte instruction; resume after it. */
    f->mepc += 4;

    /* The call that turns tracing on is not itself traced. */
    bool traced = debug_trace;

    /* A wake is traced after the line of the call that caused it; no bits means none. */
    trace_wake_bits = 0;
    int err = dispatch(t, op, arg[0], arg);
    if (err != KERR_BLOCKED) {
        f->regs[REG_A0] = (uint32_t)err;
        for (unsigned i = 1; i < 5; i++) {
            f->regs[REG_A0 + i] = arg[i];
        }
    }

    if (traced) {
        trace_call(op, in[0], in, err);
        if (trace_wake_bits != 0) {
            kputs("trace: wake ");
            kput_hex(trace_wake_bits);
            kputc('\n');
        }
    }
    if (current->state != THREAD_READY) {
        sched_run_next();
    }
    if (debug_trace) {
        selfcheck_run();
    }
}
