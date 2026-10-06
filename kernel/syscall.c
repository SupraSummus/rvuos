/*
 * Capability invocation, and the fault, the other way a thread's code enters the kernel.
 * The ABI is documented in include/rvuos/abi.h.
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "timer.h"

bool debug_trace;

/* The slot itself, as a node of the derivation tree, once cap_lookup has vetted the index. */
static struct cap *slot_node(struct thread *t, uint32_t slot)
{
    return &thread_table(t)->slots[slot];
}

/*
 * True if [base, base + size), the memory of an Untyped, holds what a revoke below it must leave:
 * the calling thread, its process and the process's table, which it runs on and names capabilities in,
 * and the table the Untyped is named in, since the capability to it that the call is made through
 * would go half way through the destroy of that table's pool.
 * A pool lies within an Untyped or outside it, so its base says which.
 */
static bool holds_caller(struct thread *t, const struct captable *named, uint32_t base, uint32_t size)
{
    return range_contains(base, size, t->hdr.pool) || range_contains(base, size, thread_process(t)->hdr.pool) ||
           range_contains(base, size, thread_table(t)->hdr.pool) || range_contains(base, size, named->hdr.pool);
}

/* The bytes of a1 to a3 into the log, the lowest of a1 first, up to the first zero. */
static void debug_write(const uint32_t *arg)
{
    for (unsigned i = 0; i < DEBUG_WRITE_BYTES; i++) {
        LOOP_BOUND(DEBUG_WRITE_BYTES);
        char c = (char)(arg[1 + i / 4] >> (8 * (i % 4)));
        if (c == '\0') {
            return;
        }
        kputc(c);
    }
}

/* Writing into the log takes RIGHT_W, and the rest, which halts or drives the machine, RIGHT_X. */
static int op_debug(const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    if (!(cap->rights & (op == OP_DEBUG_WRITE ? RIGHT_W : RIGHT_X))) {
        return KERR_NO_RIGHTS;
    }
    switch (op) {
    case OP_DEBUG_WRITE:
        debug_write(arg);
        return KERR_OK;
    case OP_DEBUG_HALT:
        kputs("user halt with code ");
        kput_hex(arg[1]);
        kputc('\n');
        khalt((int)arg[1]);
    case OP_DEBUG_TRACE:
        /*
         * The host boots with full accounts, and the target has run a while before its first traced call.
         * Once tracing is on the call changes nothing, or it would fill an account at will.
         */
        if (!debug_trace) {
            debug_trace = true;
            sched_accounts_fill();
        }
        return KERR_OK;
    case OP_DEBUG_TICK:
        /* The caller's status is written to its own frame, whoever runs next. */
        sched_tick(1);
        return KERR_OK;
    case OP_DEBUG_IRQ:
        /* The log's line is not a device's: its level is the log's, and no record fires it. */
        if (arg[1] == LOG_IRQ_LINE || arg[1] >= IRQ_LINES) {
            return KERR_INVALID_ARG;
        }
        return sched_interrupt(arg[1]) ? KERR_OK : KERR_STATE;
    case OP_DEBUG_PREEMPT:
        preempt_countdown = arg[1];
        return KERR_OK;
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_captable(struct thread *t, uint32_t slot, const struct cap *cap,
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
        struct cap *source = slot_node(t, arg[2]);
        /* A copy stands beside its source in the tree, a derivation below it. */
        if (op == OP_CAP_COPY) {
            /* A copy of an Untyped would be a second allocator over the same memory. */
            if (src.type == CAP_UNTYPED) {
                return KERR_WRONG_TYPE;
            }
            return cap_store_beside(table, arg[1], &src, source);
        }
        /*
         * A derived Untyped is the whole of the memory, which only a free one has to give,
         * and the source makes nothing more while it lives, as for anything else it made.
         */
        if (src.type == CAP_UNTYPED && !untyped_free(source)) {
            return KERR_NO_MEMORY;
        }
        return cap_store(table, arg[1], &src, source);
    }
    case OP_CAP_MOVE: {
        /* The node goes where the source stood, so the same revokes reach it and it reaches the same nodes. */
        struct cap src;
        int err = cap_lookup(thread_table(t), arg[2], &src);
        if (err != KERR_OK) {
            return err;
        }
        err = cap_slot_free(table, arg[1]);
        if (err == KERR_OK) {
            cap_move(slot_node(t, arg[2]), &table->slots[arg[1]]);
        }
        return err;
    }
    case OP_CAP_DELETE:
        return cap_clear(table, arg[1]);
    case OP_CAP_REVOKE: {
        struct cap c;
        int err = cap_lookup(table, arg[1], &c);
        if (err != KERR_OK) {
            return err;
        }
        /* Below an Untyped the pools go too. */
        if (c.type == CAP_UNTYPED && holds_caller(t, table, untyped_base(&c), untyped_size(&c))) {
            return KERR_STATE;
        }
        return cap_revoke_below(&table->slots[arg[1]], slot_node(t, slot), true) ? KERR_OK : KERR_PREEMPTED;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* A delay in whole ticks, rounded up. */
static uint32_t delay_ticks(uint32_t us)
{
    return us / TIMER_US_PER_TICK + (us % TIMER_US_PER_TICK != 0);
}

/*
 * The deadline of a delay of whole ticks, a timer line's or the watchdog's:
 * the call lands anywhere within the current tick, so the delay plus one is the first tick that surely lies past it.
 * The count wraps and deadlines compare with a signed difference,
 * which is exact while a delay stays far below half the count's range.
 */
static uint32_t delay_deadline(uint32_t ticks)
{
    return sched_ticks + ticks + 1;
}

/*
 * Reading the time and the counter's frame takes RIGHT_R, and the watchdog, which halts the machine, RIGHT_W.
 * arg[1..] carry the arguments in and the results out.
 */
static int op_clock(struct thread *t, uint32_t slot, const struct cap *cap, uint32_t op, uint32_t *arg)
{
    if (!(cap->rights & (op == OP_CLOCK_WATCHDOG ? RIGHT_W : RIGHT_R))) {
        return KERR_NO_RIGHTS;
    }
    switch (op) {
    case OP_CLOCK_READ: {
        uint64_t now = timer_now();
        arg[1] = (uint32_t)now;
        arg[2] = (uint32_t)(now >> 32);
        arg[3] = timer_counter_hz();
        arg[4] = COUNTER_ADDR;
        return KERR_OK;
    }
    case OP_CLOCK_FRAME: {
        const struct granted_range *g = &boot_granted[GRANT_COUNTER];
        struct cap r = cap_to_frame(g->base, g->size, g->rights);
        return cap_store(thread_table(t), arg[1], &r, slot_node(t, slot));
    }
    case OP_CLOCK_WATCHDOG: {
        uint32_t us = arg[1];
        if (us == 0 || us > WATCHDOG_US_MAX) {
            return KERR_INVALID_ARG;
        }
        sched_watchdog(delay_deadline(delay_ticks(us)));
        board_watchdog(us);
        return KERR_OK;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in and the results out. */
static int op_frame(struct thread *t, uint32_t slot, const struct cap *cap,
                    uint32_t op, uint32_t *arg)
{
    uint32_t base = cap->a;
    uint32_t size = cap->b;

    switch (op) {
    case OP_FRAME_INFO:
        arg[1] = base;
        arg[2] = size;
        arg[3] = cap->rights;
        arg[4] = region_min_size();
        return KERR_OK;
    case OP_FRAME_CARVE: {
        uint32_t off = arg[1];
        uint32_t len = arg[2];
        /* A block within the frame, so that every frame capability is one NAPOT entry. */
        if (off >= size || len > size - off || !napot_block(base + off, len)) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = cap_to_frame(base + off, len, cap->rights);
        return cap_store(thread_table(t), arg[3], &sub, slot_node(t, slot));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in and the results out. */
static int op_untyped(struct thread *t, uint32_t slot, const struct cap *cap,
                      uint32_t op, uint32_t *arg)
{
    uint32_t base = untyped_base(cap);
    uint32_t size = untyped_size(cap);
    struct cap *u = slot_node(t, slot);
    struct captable *table = thread_table(t);

    switch (op) {
    case OP_UNTYPED_INFO:
        arg[1] = base;
        arg[2] = size;
        arg[3] = cap->rights;
        arg[4] = !untyped_free(u);
        return KERR_OK;
    case OP_UNTYPED_RETYPE: {
        uint32_t type = arg[1];
        if (type != CAP_FRAME && type != CAP_POOL) {
            return KERR_INVALID_ARG;
        }
        /*
         * Kernel objects are written into a pool, so the memory must allow both.
         * A block this large lies on OBJ_ALIGN and holds the descriptor.
         */
        _Static_assert(POOL_MIN_SIZE % OBJ_ALIGN == 0 && POOL_MIN_SIZE >= sizeof(struct pool),
                       "a pool-sized block lies on OBJ_ALIGN and holds its descriptor");
        if (type == CAP_POOL) {
            if ((cap->rights & (RIGHT_R | RIGHT_W)) != (RIGHT_R | RIGHT_W)) {
                return KERR_NO_RIGHTS;
            }
            if (size < POOL_MIN_SIZE) {
                return KERR_INVALID_ARG;
            }
        }
        int err = cap_slot_free(table, arg[2]);
        if (err != KERR_OK) {
            return err;
        }
        /* The whole of the memory, which nothing else lies in while nothing made of it is left. */
        if (!untyped_free(u)) {
            return KERR_NO_MEMORY;
        }
        struct cap c;
        struct cap *parent = u;
        if (type == CAP_POOL) {
            /*
             * The pool's own node hangs below the Untyped and the capability below it,
             * so the capability may go and the memory stays accounted for.
             * It is not zeroed here, which would be work in its size: pool_alloc zeroes each object.
             */
            struct pool *pool = pool_create(base, size, u);
            c = cap_to_object(&pool->hdr, RIGHT_ALL);
            parent = &pool->node;
        } else {
            c = cap_to_frame(base, size, cap->rights);
        }
        arg[1] = base;
        return cap_store(table, arg[2], &c, parent);
    }
    case OP_UNTYPED_SPLIT: {
        /* Each half is a block, so that a frame made of it is one NAPOT entry. */
        uint32_t half = size / 2;
        if (!napot_block(base, half) || arg[1] == arg[2]) {
            return KERR_INVALID_ARG;
        }
        int err = cap_slot_free(table, arg[1]);
        if (err == KERR_OK) {
            err = cap_slot_free(table, arg[2]);
        }
        if (err != KERR_OK) {
            return err;
        }
        if (!untyped_free(u)) {
            return KERR_NO_MEMORY;
        }
        /* Both slots are free, so neither store fails and the split is whole or not at all. */
        struct cap lower = cap_to_untyped(base, half, cap->rights);
        struct cap upper = cap_to_untyped(base + half, half, cap->rights);
        cap_store(table, arg[1], &lower, u);
        return cap_store(table, arg[2], &upper, u);
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A pool only allocates.
 * It is destroyed by a revoke below the Untyped it was made of, whose holder owns the memory;
 * see DESIGN.md, "Kernel pools and revocation".
 */
static int op_pool(struct thread *t, uint32_t slot, const struct cap *cap,
                   uint32_t op, const uint32_t *arg)
{
    struct pool *pool = (struct pool *)cap_object(cap);

    if (op != OP_POOL_ALLOC) {
        return KERR_WRONG_TYPE;
    }
    if (pool->dying) {
        return KERR_STATE;
    }
    uint32_t dst = arg[2];
    int err = cap_slot_free(thread_table(t), dst);
    if (err != KERR_OK) {
        return err;
    }

    struct obj_header *obj;
    switch (arg[1]) {
    /*
     * A process's table and a thread's process may lie in any pool,
     * since the sweep clears the hold on them; see DESIGN.md, "Kernel pools and revocation".
     */
    case CAP_PROCESS: {
        struct cap tc;
        int terr = cap_lookup_typed(thread_table(t), arg[3], CAP_CAPTABLE, RIGHT_W, &tc);
        if (terr != KERR_OK) {
            return terr;
        }
        struct process *proc = pool_alloc(pool, CAP_PROCESS, sizeof(*proc));
        if (proc == NULL) {
            return KERR_NO_MEMORY;
        }
        /* It hangs below the capability it was made with, so a revoke above that takes it. */
        proc->table = tc;
        cap_attach(slot_node(t, arg[3]), &proc->table);
        obj = &proc->hdr;
        break;
    }
    case CAP_THREAD: {
        struct cap pc;
        int perr = cap_lookup_typed(thread_table(t), arg[3], CAP_PROCESS, RIGHT_W, &pc);
        if (perr != KERR_OK) {
            return perr;
        }
        struct thread *nt = pool_alloc(pool, CAP_THREAD, sizeof(*nt));
        if (nt == NULL) {
            return KERR_NO_MEMORY;
        }
        /* It hangs below the capability it was made with, so a revoke above that stops the thread. */
        nt->proc = (struct cap){ .type = CAP_HOSTED, .rights = pc.rights, .a = pc.a };
        cap_attach(slot_node(t, arg[3]), &nt->proc);
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

    /* A new object hangs below the capability it was allocated through, and so below the pool's node. */
    struct cap c = cap_to_object(obj, RIGHT_ALL);
    return cap_store(thread_table(t), dst, &c, slot_node(t, slot));
}

static int op_process(struct thread *t, const struct cap *cap,
                      uint32_t op, const uint32_t *arg)
{
    struct process *proc = (struct process *)cap_object(cap);

    switch (op) {
    case OP_PROCESS_INSTALL: {
        struct cap region;
        int err = cap_lookup_typed(thread_table(t), arg[2], CAP_FRAME, 0, &region);
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
        /* An MPU fetches only what it lets the thread read; see EXECUTE_NEEDS_READ in arch.h. */
        if (EXECUTE_NEEDS_READ && (rights & RIGHT_X) && !(rights & RIGHT_R)) {
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

/* arg[1..] carry the arguments in and the results out. */
static int op_thread(struct thread *t, const struct cap *cap, uint32_t op, uint32_t *arg)
{
    struct thread *target = (struct thread *)cap_object(cap);

    switch (op) {
    case OP_THREAD_CONFIGURE:
        if (target->state != THREAD_STOPPED) {
            return KERR_STATE;
        }
        frame_start(&target->frame, arg[1], arg[2], arg[3]);
        /*
         * The host build follows a thread only if its code was fixed
         * before tracing began; see DESIGN.md, "Verification".
         */
        if (debug_trace) {
            target->flags |= THREAD_UNTRACED;
        } else {
            target->flags &= (uint8_t)~THREAD_UNTRACED;
        }
        /* It starts afresh, so what stopped it before is nothing to tell. */
        target->flags &= (uint8_t)~THREAD_FAULTED;
        return KERR_OK;
    case OP_THREAD_RESUME:
        /* A thread whose process was taken has nothing to run in. */
        if (target->state != THREAD_STOPPED || thread_process(target) == NULL) {
            return KERR_STATE;
        }
        target->flags &= (uint8_t)~THREAD_FAULTED;
        sched_ready(target);
        return KERR_OK;
    case OP_THREAD_FAULT:
        /* The frame keeps what the trap saved, and only a fault leaves the thread stopped with it. */
        if (!(target->flags & THREAD_FAULTED)) {
            return KERR_STATE;
        }
        frame_fault(&target->frame, &arg[1]);
        return KERR_OK;
    case OP_THREAD_READ_REG:
        /* A stopped thread's frame is still until it runs; any other's is the scheduler's or the call's. */
        if (target->state != THREAD_STOPPED) {
            return KERR_STATE;
        }
        return frame_read(&target->frame, arg[1], &arg[1]) ? KERR_OK : KERR_INVALID_ARG;
    case OP_THREAD_WRITE_REG:
        if (target->state != THREAD_STOPPED) {
            return KERR_STATE;
        }
        if (!frame_write(&target->frame, arg[1], arg[2])) {
            return KERR_INVALID_ARG;
        }
        /* The host build sets no register but through a call, so under tracing it follows the thread no more. */
        if (debug_trace) {
            target->flags |= THREAD_UNTRACED;
        }
        return KERR_OK;
    case OP_THREAD_WATCH: {
        /* The watch signals on its setter's behalf, so the setter must be able to, and only the bits it may. */
        uint32_t bits = arg[2];
        struct cap nc;
        if (bits != 0) {
            int err = cap_lookup_typed(thread_table(t), arg[1], CAP_NOTIFICATION, RIGHT_W, &nc);
            if (err != KERR_OK) {
                return err;
            }
            bits &= nc.b;
            if (bits == 0) {
                return KERR_NO_RIGHTS;
            }
        }
        /* The watch is a leaf, so it leaves the tree in a step. */
        if (target->watch.type != CAP_NONE) {
            cap_delete(&target->watch, false);
        }
        if (bits != 0) {
            /* It hangs below the capability it was set with, so a revoke above that clears it. */
            target->watch = (struct cap){ .type = CAP_WATCHED, .rights = nc.rights, .a = nc.a, .b = bits };
            cap_attach(slot_node(t, arg[1]), &target->watch);
        }
        return KERR_OK;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A wait that lends the thread a1 names the caller's time; see OP_NOTIFY_LEND.
 * RIGHT_X lends a thread time and nothing else; see DESIGN.md, "Communication and synchronisation".
 * Out of line, so that the other operations on a notification save no more registers for its lookup.
 */
static __attribute__((noinline)) int op_lend(struct thread *t, struct notification *ntfn, uint32_t *arg)
{
    struct cap bc;
    int err = cap_lookup_typed(thread_table(t), arg[1], CAP_THREAD, RIGHT_X, &bc);
    if (err != KERR_OK) {
        return err;
    }
    struct thread *borrower = (struct thread *)cap_object(&bc);
    if (borrower == t) {
        return KERR_INVALID_ARG;
    }
    if (ntfn->bits != 0) {
        arg[1] = ntfn->bits;
        ntfn->bits = 0;
        return KERR_OK;
    }
    sched_lend(t, ntfn, borrower);
    return KERR_BLOCKED;
}

static int op_notification(struct thread *t, uint32_t slot, const struct cap *cap,
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
        /* The capability says which bits it may signal, so a wait's bits say who signalled. */
        uint32_t bits = arg[1] & cap->b;
        if (bits == 0) {
            return KERR_NO_RIGHTS;
        }
        /* The caller's status is written to its own frame, whoever runs next. */
        if (sched_signal(ntfn, bits)) {
            sched_turn_back();
        }
        return KERR_OK;
    }
    case OP_NOTIFY_CARVE: {
        /* Fewer bits, as a carve of a frame is less memory: a table operation, below the invoked capability. */
        struct cap sub = *cap;
        sub.b &= arg[1];
        if (sub.b == 0) {
            return KERR_INVALID_ARG;
        }
        return cap_store(thread_table(t), arg[2], &sub, slot_node(t, slot));
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
    case OP_NOTIFY_LEND:
        return (cap->rights & RIGHT_R) ? op_lend(t, ntfn, arg) : KERR_NO_RIGHTS;
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A smaller range of the lines or units a capability names, with its rights, below it:
 * a1 = offset from the first, a2 = count, a3 = destination slot.
 */
static int carve_range(struct thread *t, uint32_t slot, const struct cap *cap, const uint32_t *arg)
{
    uint32_t off = arg[1];
    uint32_t len = arg[2];
    if (len == 0 || off > cap->b || len > cap->b - off) {
        return KERR_INVALID_ARG;
    }
    struct cap sub = { .type = cap->type, .rights = cap->rights, .a = cap->a + off, .b = len };
    return cap_store(thread_table(t), arg[3], &sub, slot_node(t, slot));
}

/* arg[1..] carry the arguments in. */
static int op_irq_line(struct thread *t, uint32_t slot, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    uint32_t first = cap->a;
    uint32_t count = cap->b;

    switch (op) {
    case OP_IRQ_CARVE:
        return carve_range(t, slot, cap, arg);
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
        if (pool->dying) {
            return KERR_STATE;
        }
        /* The Irq signals on its creator's behalf, so the creator must be able to. */
        struct cap nc;
        err = cap_lookup_typed(thread_table(t), arg[2], CAP_NOTIFICATION, RIGHT_W, &nc);
        if (err != KERR_OK) {
            return err;
        }
        /* The cores' line is bound already, to the kernel. */
        if (line_binding(first) != NULL || line_is_cores(first)) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the Irq capability. */
        if (arg[3] != slot) {
            err = cap_slot_free(thread_table(t), arg[3]);
            if (err != KERR_OK) {
                return err;
            }
        }
        /* Room is checked before the line's derivation goes, since what a revoke took stays taken. */
        if (!pool_fits(pool, sizeof(struct irq))) {
            return KERR_NO_MEMORY;
        }
        if (!cap_revoke_below(slot_node(t, slot), slot_node(t, slot), true)) {
            return KERR_PREEMPTED;
        }
        struct irq *irq = pool_alloc(pool, CAP_IRQ, sizeof(*irq));
        /* It hangs below the capability it was bound with, so a revoke above that disarms the Irq. */
        irq->ntfn = (struct cap){ .type = CAP_SIGNALLED, .rights = nc.rights, .a = nc.a, .b = nc.b };
        cap_attach(slot_node(t, arg[2]), &irq->ntfn);
        irq->line = first;
        /* A period counts from the last deadline, and a line that never fired from its bind. */
        irq->deadline = sched_ticks;
        line_irq[first] = v2p(irq);
        /* The line goes, a leaf now, and the Irq hangs below the pool capability as any object does. */
        cap_delete(slot_node(t, slot), false);
        /* Its bits are zero, so it is disarmed, and its line stays masked as every line does until a set. */
        struct cap ic = cap_to_object(&irq->hdr, RIGHT_ALL);
        return cap_store(thread_table(t), arg[3], &ic, slot_node(t, arg[1]));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in. */
static int op_time(struct thread *t, uint32_t slot, const struct cap *cap,
                   uint32_t op, const uint32_t *arg)
{
    switch (op) {
    case OP_TIME_CARVE:
        return carve_range(t, slot, cap, arg);
    case OP_TIME_BIND: {
        if (!(cap->rights & RIGHT_W)) {
            return KERR_NO_RIGHTS;
        }
        struct cap tc;
        int err = cap_lookup_typed(thread_table(t), arg[1], CAP_THREAD, RIGHT_W, &tc);
        if (err != KERR_OK) {
            return err;
        }
        /* The first unit lies within the capability even for none, so that the binding lies below it. */
        uint32_t off = arg[2];
        uint32_t count = arg[3];
        if (off >= cap->b || count > cap->b - off) {
            return KERR_INVALID_ARG;
        }
        /* A thread runs on one core, so the units it earns are of one. */
        uint32_t first = cap->a + off;
        if (count != 0 && unit_core(first) != unit_core(first + count - 1)) {
            return KERR_INVALID_ARG;
        }
        /* A unit is earned by one thread at a time; the thread's own it leaves as it binds. */
        struct thread *target = (struct thread *)cap_object(&tc);
        for (uint32_t i = 0; i < count; i++) {
            LOOP_BOUND(TIME_UNITS);
            if (unit_thread[first + i] != 0 && unit_thread[first + i] != v2p(target)) {
                return KERR_OVERLAP;
            }
        }
        /* The binding hangs below the invoked capability, so a revoke above it unbinds the thread. */
        sched_bind(target, first, count, cap->rights, slot_node(t, slot));
        return KERR_OK;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A period on a timer line: the deadline moves to the first tick after the count
 * a whole number of periods from the deadline before; returns the periods skipped.
 * A deadline more than half the count's range old passes for one ahead,
 * and the new one is within a period all the same. See DESIGN.md, "Time".
 */
static uint32_t timer_period(struct irq *irq, uint32_t period)
{
    uint32_t now = sched_ticks;
    if (irq_due(irq, now)) {
        uint32_t behind = now - irq->deadline;
        irq->deadline = now + period - behind % period;
        return behind / period;
    }
    irq->deadline = now + (irq->deadline - now - 1) % period + 1;
    return 0;
}

/* arg[1..] carry the arguments in and the results out. */
static int op_irq(const struct cap *cap, uint32_t op, uint32_t *arg)
{
    struct irq *irq = (struct irq *)cap_object(cap);
    uint32_t bits = arg[1];

    if (op != OP_IRQ_SET) {
        return KERR_WRONG_TYPE;
    }
    /* An Irq whose notification was taken has nothing to signal. */
    if (irq_notification(irq) == NULL) {
        return KERR_STATE;
    }
    /* It signals on its binder's behalf, so only the bits the capability it was bound with may. */
    bits &= irq->ntfn.b;
    if (bits == 0 && arg[1] != 0) {
        return KERR_NO_RIGHTS;
    }
    if (line_is_timer(irq->line) && bits != 0) {
        uint32_t ticks = delay_ticks(arg[2]);
        bool period = arg[3] == IRQ_SET_PERIOD;
        if (arg[3] > IRQ_SET_PERIOD || (period && ticks == 0)) {
            return KERR_INVALID_ARG;
        }
        if (period) {
            arg[1] = timer_period(irq, ticks);
        } else {
            irq->deadline = delay_deadline(ticks);
        }
    }
    /* Armed and unmasked are one state, here and in sched_interrupt. */
    irq_set_bits(irq, bits);
    if (irq->line == LOG_IRQ_LINE) {
        klog_set(irq);
    } else if (line_on_controller(irq->line)) {
        irq_enable(irq->line, irq_armed(irq));
    }
    return KERR_OK;
}

/* The line of a wake, after the line of the call or the fault that caused it; none if nothing woke. */
static void trace_wake(void)
{
    uint32_t bits = core_self()->trace_wake_bits;
    if (bits != 0) {
        kputs("trace: wake ");
        kput_hex(bits);
        kputc('\n');
    }
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
    /*
     * A thread whose process, or its process's table, was taken names nothing.
     * Within a call only a revoke, which then returns, can take either.
     */
    struct captable *table = thread_table(t);
    if (table == NULL) {
        return KERR_INVALID_CAP;
    }
    struct cap cap;
    int err = cap_lookup(table, slot, &cap);
    if (err != KERR_OK) {
        return err;
    }

    /*
     * Operations that change an object need RIGHT_W on it.
     * Frames, Untypeds, interrupt lines, units of time, notifications, the debug capability and the clock
     * are checked in their handlers,
     * because which right they need depends on the operation.
     */
    switch (cap.type) {
    case CAP_DEBUG:
        err = op_debug(&cap, op, arg);
        break;
    case CAP_FRAME:
        err = op_frame(t, slot, &cap, op, arg);
        break;
    case CAP_UNTYPED:
        err = op_untyped(t, slot, &cap, op, arg);
        break;
    case CAP_CLOCK:
        err = op_clock(t, slot, &cap, op, arg);
        break;
    case CAP_CAPTABLE:
        err = (cap.rights & RIGHT_W) ? op_captable(t, slot, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_POOL:
        err = (cap.rights & RIGHT_W) ? op_pool(t, slot, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_PROCESS:
        err = (cap.rights & RIGHT_W) ? op_process(t, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_THREAD:
        err = (cap.rights & RIGHT_W) ? op_thread(t, &cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_NOTIFICATION:
        err = op_notification(t, slot, &cap, op, arg);
        break;
    case CAP_IRQ_LINE:
        err = op_irq_line(t, slot, &cap, op, arg);
        break;
    case CAP_IRQ:
        err = (cap.rights & RIGHT_W) ? op_irq(&cap, op, arg) : KERR_NO_RIGHTS;
        break;
    case CAP_TIME:
        err = op_time(t, slot, &cap, op, arg);
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
        LOOP_BOUND(5);
        in[i] = arg[i] = f->regs[REG_A0 + i];
    }

    /* Resume after the call instruction, unless the call is preempted. */
    f->pc += CALL_SIZE;

    /* The call that turns tracing on is not itself traced. */
    bool traced = debug_trace;

    struct core *core = core_self();
    /* A wake is traced after the line of the call that caused it; no bits means none. */
    core->trace_wake_bits = 0;
    int err = dispatch(t, op, arg[0], arg);
    if (err == KERR_PREEMPTED) {
        /*
         * Back to the call instruction, with the registers as the call found them.
         * The return to user mode takes the pending interrupt at once,
         * and the thread makes the call again when it next runs.
         * The call is traced once, when it finishes.
         */
        f->pc -= CALL_SIZE;
        traced = false;
    } else if (err != KERR_BLOCKED) {
        f->regs[REG_A0] = (uint32_t)err;
        for (unsigned i = 1; i < 5; i++) {
            LOOP_BOUND(4);
            f->regs[REG_A0 + i] = arg[i];
        }
    }

    if (traced) {
        trace_call(op, in[0], in, err);
        trace_wake();
    }
    /*
     * The stop OP_DEBUG_PREEMPT armed is where the tick lands, as OP_DEBUG_TICK does it:
     * the caller stays at its call instruction, ready, and the next thread has its turn.
     * Whatever the call's attempt woke is left untraced, as a stopped attempt's always is.
     */
    if (core->preempt_stopped) {
        core->preempt_stopped = false;
        if (debug_trace) {
            kputs("trace: preempt\n");
        }
        core->trace_wake_bits = 0;
        sched_tick(1);
        if (debug_trace) {
            trace_wake();
        }
    }
    /*
     * A thread that waits gives the processor up, and so does one whose process the call took,
     * unless it handed its turn to the thread it lends its time to, which runs now.
     * One that lost its units finishes its turn, as a preempted call is made again before the tick switches.
     */
    if (core->current->state != THREAD_READY) {
        sched_run_next();
    }
    if (debug_trace) {
        selfcheck_run();
    }
}

/*
 * The thread stops with its pc at the instruction that faulted, so a resume runs it again,
 * and nothing else changes; see DESIGN.md, "Faults".
 * The report goes to the log traced or not, and a wake is traced after it, as after a call's line.
 */
void fault_dispatch(struct thread *t)
{
    kputs("user fault\n");
    report_frame(&t->frame);
    t->state = THREAD_STOPPED;
    t->flags |= THREAD_FAULTED;
    core_self()->trace_wake_bits = 0;
    struct notification *watch = thread_watch(t);
    if (watch != NULL) {
        sched_signal(watch, t->watch.b);
    }
    if (debug_trace) {
        trace_wake();
    }
    sched_run_next();
    if (debug_trace) {
        selfcheck_run();
    }
}
