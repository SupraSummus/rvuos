/*
 * Capability invocation, and the fault, the other way a thread's code enters the kernel.
 * The ABI is documented in include/rvuos/abi.h.
 */

#include "dma.h"
#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "route.h"
#include "sched.h"
#include "timer.h"

#if SELFCHECK
bool debug_trace;
#endif

/* The slot itself, as a node of the derivation tree, once slot_lookup has vetted the number. */
static struct cap *slot_node(struct thread *t, uint32_t slot)
{
    return &slot_table(t, slot)->slots[slot_index(slot)];
}

/*
 * True if [base, base + size), the memory of an Untyped, holds what a revoke below it must leave:
 * the calling thread, its process and the tables the process mounts, which it runs on and names capabilities in,
 * and the table the Untyped is named in, since the capability to it that the call is made through
 * would go half way through the destroy of that table's pool.
 * A pool lies within an Untyped or outside it, so its base says which.
 */
static bool holds_caller(struct thread *t, const struct captable *named, uint32_t base, uint32_t size)
{
    const struct process *proc = thread_process(t);
    if (range_contains(base, size, t->hdr.pool) || range_contains(base, size, proc->hdr.pool) ||
        range_contains(base, size, named->hdr.pool)) {
        return true;
    }
    for (uint32_t i = 0; i < PROCESS_TABLES; i++) {
        LOOP_BOUND(PROCESS_TABLES);
        const struct captable *table = process_table(proc, i);
        if (table != NULL && range_contains(base, size, table->hdr.pool)) {
            return true;
        }
    }
    return false;
}

/* Into the log: the bytes of a1 to a3, the lowest of a1 first, up to the first zero. */
static int op_print(const uint32_t *arg)
{
    for (unsigned i = 0; i < PRINT_BYTES; i++) {
        LOOP_BOUND(PRINT_BYTES);
        char c = (char)(arg[1 + i / 4] >> (8 * (i % 4)));
        if (c == '\0') {
            break;
        }
        kputc(c);
    }
    return KERR_OK;
}

static int op_halt(const uint32_t *arg)
{
    kputs("user halt with code ");
    kput_hex(arg[1]);
    kputc('\n');
    khalt((int)arg[1]);
}

/* Driving the machine as a test does: tracing, moving the clock, interrupting and preempting. */
static int op_test(uint32_t op, const uint32_t *arg)
{
    /*
     * Untraced, only time and the devices drive the machine:
     * a step of the clock no time brought would leave it ahead of the counter for good,
     * and every deadline set before would fire early, the watchdog's among them.
     */
    if (!debug_trace && op != OP_TEST_TRACE) {
        return KERR_STATE;
    }
    switch (op) {
#if SELFCHECK
    case OP_TEST_TRACE:
        /*
         * The host boots with every virtual time at the count, and the target has run a while before its first traced call,
         * so the clock goes on from the counter, by record now, and every virtual time is brought to it.
         * Once tracing is on the call changes nothing, or it would forgive a thread at will.
         */
        if (!debug_trace) {
            sched_trace_clock = counter_read();
            debug_trace = true;
            sched_vtimes_reset();
        }
        return KERR_OK;
#endif
    case OP_TEST_TICK:
        /* The caller's status is written to its own frame, whoever runs next. */
        sched_tick();
        return KERR_OK;
    case OP_TEST_IRQ:
        /* The log's line is not a device's: its level is the log's, and no record fires it. */
        if (arg[1] == LOG_IRQ_LINE || arg[1] >= IRQ_LINES) {
            return KERR_INVALID_ARG;
        }
        return sched_interrupt(arg[1]) ? KERR_OK : KERR_STATE;
    case OP_TEST_PREEMPT:
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
        int err = slot_lookup(t, arg[2], &src);
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
        int err = slot_lookup(t, arg[2], &src);
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
        if (c.type == CAP_UNTYPED && holds_caller(t, table, block_base(&c), block_size(&c))) {
            return KERR_STATE;
        }
        return cap_revoke_below(&table->slots[arg[1]], slot, true) ? KERR_OK : KERR_PREEMPTED;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * The deadline of a delay in microseconds, a timer line's or the watchdog's:
 * the first count past the clock by at least that much, so even a delay of none lies ahead of it.
 */
static uint64_t delay_deadline(uint32_t us)
{
    return timer_now() + timer_us_counts(us) + 1;
}

/*
 * Reading the time takes RIGHT_R, and the watchdog, which halts the machine, RIGHT_W; see op_shapes.
 * arg[1..] carry the arguments in and the results out.
 */
static int op_clock(uint32_t op, uint32_t *arg)
{
    switch (op) {
    case OP_CLOCK_READ: {
        uint64_t now = timer_now();
        arg[1] = (uint32_t)now;
        arg[2] = (uint32_t)(now >> 32);
        arg[3] = timer_counter_hz();
        return KERR_OK;
    }
    case OP_CLOCK_WATCHDOG: {
        uint32_t us = arg[1];
        if (us == 0 || us > WATCHDOG_US_MAX) {
            return KERR_INVALID_ARG;
        }
        sched_watchdog(delay_deadline(us));
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
    uint32_t base = block_base(cap);
    uint32_t size = block_size(cap);

    switch (op) {
    case OP_FRAME_CARVE: {
        uint32_t off = arg[1];
        uint32_t len = arg[2];
        /* A block within the frame, so that every frame capability is one NAPOT entry. */
        if (off >= size || len > size - off || !napot_block(base + off, len)) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = cap_to_frame(base + off, len, cap->rights);
        return slot_store(t, arg[3], &sub, slot_node(t, slot));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in and the results out. */
static int op_untyped(struct thread *t, uint32_t slot, const struct cap *cap,
                      uint32_t op, uint32_t *arg)
{
    uint32_t base = block_base(cap);
    uint32_t size = block_size(cap);
    struct cap *u = slot_node(t, slot);

    switch (op) {
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
        int err = slot_vacant(t, arg[2]);
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
        return slot_store(t, arg[2], &c, parent);
    }
    case OP_UNTYPED_SPLIT: {
        /* Each half is a block, so that a frame made of it is one NAPOT entry. */
        uint32_t half = size / 2;
        if (!napot_block(base, half)) {
            return KERR_INVALID_ARG;
        }
        int err = slot_vacant(t, arg[1]);
        if (err == KERR_OK) {
            err = slot_vacant(t, arg[2]);
        }
        if (err != KERR_OK) {
            return err;
        }
        /* Two numbers name one slot when they are one, or point into one table mounted twice. */
        if (slot_node(t, arg[1]) == slot_node(t, arg[2])) {
            return KERR_INVALID_ARG;
        }
        if (!untyped_free(u)) {
            return KERR_NO_MEMORY;
        }
        /* Both slots are free and distinct, so neither store fails and the split is whole or not at all. */
        struct cap lower = cap_to_untyped(base, half, cap->rights);
        struct cap upper = cap_to_untyped(base + half, half, cap->rights);
        slot_store(t, arg[1], &lower, u);
        return slot_store(t, arg[2], &upper, u);
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * What an object of a type would take of a pool, and the room, see OP_POOL_INFO:
 * the types OP_POOL_ALLOC makes, the Irq OP_IRQ_BIND, the Route OP_ROUTE_BIND and the Dma OP_DMA_BIND make of a pool,
 * and the descriptor.
 */
static int pool_info(const struct pool *pool, uint32_t *arg)
{
    uint32_t type = arg[1];
    uint32_t slots = arg[3];
    if (type != CAP_CAPTABLE && type != CAP_PROCESS && type != CAP_THREAD && type != CAP_NOTIFICATION &&
        type != CAP_IRQ && type != CAP_ROUTE && type != CAP_DMA && type != CAP_POOL) {
        return KERR_INVALID_ARG;
    }
    uint32_t most = type == CAP_CAPTABLE ? CAPTABLE_MAX_SLOTS : type == CAP_PROCESS ? pmp_entry_count
                  : type == CAP_THREAD ? CORES : 0;
    if (most != 0 ? slots == 0 || slots > most : slots != 0) {
        return KERR_INVALID_ARG;
    }
    arg[1] = obj_align((uint32_t)obj_type_size((uint8_t)type, slots));
    arg[2] = pool_room(pool);
    return KERR_OK;
}

/*
 * A pool allocates, and tells what an allocation would take of it.
 * It is destroyed by a revoke below the Untyped it was made of, whose holder owns the memory;
 * see DESIGN.md, "Kernel pools and revocation".
 * arg[1..] carry the arguments in and the results out.
 */
static int op_pool(struct thread *t, uint32_t slot, const struct cap *cap,
                   uint32_t op, uint32_t *arg)
{
    struct pool *pool = (struct pool *)cap_object(cap);

    if (op == OP_POOL_INFO) {
        return pool_info(pool, arg);
    }
    if (op != OP_POOL_ALLOC) {
        return KERR_WRONG_TYPE;
    }
    if (pool->dying) {
        return KERR_STATE;
    }
    uint32_t dst = arg[2];
    int err = slot_vacant(t, dst);
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
        int terr = slot_lookup_typed(t, arg[3], CAP_CAPTABLE, RIGHT_W, &tc);
        if (terr != KERR_OK) {
            return terr;
        }
        /* A region slot for each region it may have installed at once, one at least and an entry each at most. */
        uint32_t regions = arg[4];
        if (regions == 0 || regions > pmp_entry_count) {
            return KERR_INVALID_ARG;
        }
        struct process *proc = pool_alloc(pool, CAP_PROCESS, obj_type_size(CAP_PROCESS, regions));
        if (proc == NULL) {
            return KERR_NO_MEMORY;
        }
        proc->regions = (uint8_t)regions;
        /* Its first table hangs below the capability it was made with, so a revoke above that takes it. */
        proc->tables[0] = tc;
        cap_attach(slot_node(t, arg[3]), &proc->tables[0]);
        obj = &proc->hdr;
        break;
    }
    case CAP_THREAD: {
        struct cap pc;
        int perr = slot_lookup_typed(t, arg[3], CAP_PROCESS, RIGHT_W, &pc);
        if (perr != KERR_OK) {
            return perr;
        }
        /* A place for each core it may hold the time of at once, one at least and one of each core at most. */
        uint32_t places = arg[4];
        if (places == 0 || places > CORES) {
            return KERR_INVALID_ARG;
        }
        struct thread *nt = pool_alloc(pool, CAP_THREAD, obj_type_size(CAP_THREAD, places));
        if (nt == NULL) {
            return KERR_NO_MEMORY;
        }
        thread_make_places(nt, places);
        /* It hangs below the capability it was made with, so a revoke above that stops the thread. */
        nt->proc = (struct cap){ .type = CAP_PROCESS, .rights = pc.rights, .a = cap_addr(&pc) };
        nt->proc.b = held_word(v2p(nt), 0);
        cap_attach(slot_node(t, arg[3]), &nt->proc);
        nt->state = THREAD_STOPPED;
        /* Until it is configured while tracing is off, the host cannot follow it. */
        nt->flags = THREAD_UNTRACED;
        obj = &nt->hdr;
        break;
    }
    case CAP_NOTIFICATION: {
        struct notification *ntfn = pool_alloc(pool, CAP_NOTIFICATION, obj_type_size(CAP_NOTIFICATION, 0));
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
        struct captable *table = pool_alloc(pool, CAP_CAPTABLE, obj_type_size(CAP_CAPTABLE, nslots));
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
    return slot_store(t, dst, &c, slot_node(t, slot));
}

static int op_process(struct thread *t, const struct cap *cap,
                      uint32_t op, const uint32_t *arg)
{
    struct process *proc = (struct process *)cap_object(cap);

    switch (op) {
    case OP_PROCESS_INSTALL: {
        struct cap region;
        int err = slot_lookup_typed(t, arg[2], CAP_FRAME, 0, &region);
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
        return process_install(proc, arg[1], block_base(&region), block_size(&region), rights, slot_node(t, arg[2]));
    }
    case OP_PROCESS_UNINSTALL:
        return process_uninstall(proc, arg[1]);
    case OP_PROCESS_MOUNT: {
        struct cap tc;
        int err = slot_lookup_typed(t, arg[2], CAP_CAPTABLE, RIGHT_W, &tc);
        if (err != KERR_OK) {
            return err;
        }
        return process_mount(proc, arg[1], &tc, slot_node(t, arg[2]));
    }
    case OP_PROCESS_UNMOUNT:
        return process_unmount(proc, arg[1]);
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
    case OP_THREAD_STOP:
        arg[1] = sched_stop(target);
        return KERR_OK;
    case OP_THREAD_FAULT:
        /* The frame keeps what the trap saved, and only a fault leaves the thread stopped with it. */
        if (!(target->flags & THREAD_FAULTED)) {
            return KERR_STATE;
        }
        frame_fault(&target->frame, &arg[1]);
        return KERR_OK;
    case OP_THREAD_OVERRUNS:
        arg[1] = 0;
        for (uint32_t i = 0; i < thread_places(target); i++) {
            LOOP_BOUND(CORES);
            arg[1] += thread_place(target, i)->overruns;
        }
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
        uint32_t code = 0;
        struct cap nc;
        if (bits != 0) {
            int err = slot_lookup_typed(t, arg[1], CAP_NOTIFICATION, RIGHT_W, &nc);
            if (err != KERR_OK) {
                return err;
            }
            bits &= notify_bits(&nc);
            if (bits == 0) {
                return KERR_NO_RIGHTS;
            }
            /* The watch is a capability, whose bits are a block. */
            if (!notify_block(bits, &code)) {
                return KERR_INVALID_ARG;
            }
        }
        /* The watch is a leaf, so it leaves the tree in a step. */
        if (target->watch.type != CAP_NONE) {
            cap_delete(&target->watch, false);
        }
        if (bits != 0) {
            /* It hangs below the capability it was set with, so a revoke above that clears it. */
            target->watch = (struct cap){ .type = CAP_NOTIFICATION, .rights = nc.rights };
            notify_set(&target->watch, cap_addr(&nc), code);
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
    int err = slot_lookup_typed(t, arg[1], CAP_THREAD, RIGHT_X, &bc);
    if (err != KERR_OK) {
        return err;
    }
    struct thread *borrower = (struct thread *)cap_object(&bc);
    if (borrower == t) {
        return KERR_INVALID_ARG;
    }
    if (arg[2] != 0) {
        if (!span_valid(arg[2])) {
            return KERR_INVALID_ARG;
        }
        sched_span(t, arg[2]);
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
        /* A wait never returns zero bits, so signalling none would be a lie. */
        if (arg[1] == 0) {
            return KERR_INVALID_ARG;
        }
        /* The capability says which bits it may signal, so a wait's bits say who signalled. */
        uint32_t bits = arg[1] & notify_bits(cap);
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
        /*
         * Fewer bits, as a carve of a frame is less memory: a table operation, below the invoked capability.
         * They are a block, as every Notification capability's are, so that six bits of the slot say which.
         */
        uint32_t code;
        if (!notify_block(arg[1] & notify_bits(cap), &code)) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = *cap;
        notify_set(&sub, cap_addr(cap), code);
        return slot_store(t, arg[2], &sub, slot_node(t, slot));
    }
    case OP_NOTIFY_WAIT:
        /* The span is the waiter's own, which costs nobody else; see sched.c, "Requests". */
        if (arg[1] != 0) {
            if (!span_valid(arg[1])) {
                return KERR_INVALID_ARG;
            }
            sched_span(t, arg[1]);
        }
        if (ntfn->bits != 0) {
            arg[1] = ntfn->bits;
            ntfn->bits = 0;
            return KERR_OK;
        }
        sched_wait(t, ntfn);
        return KERR_BLOCKED;
    case OP_NOTIFY_LEND:
        return op_lend(t, ntfn, arg);
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A smaller range of the lines, units or channels a capability names, with its rights, below it:
 * a1 = offset from the first, a2 = count, a3 = destination slot.
 */
static int carve_range(struct thread *t, uint32_t slot, const struct cap *cap, const uint32_t *arg)
{
    uint32_t off = arg[1];
    uint32_t len = arg[2];
    if (len == 0 || off > range_count(cap) || len > range_count(cap) - off) {
        return KERR_INVALID_ARG;
    }
    struct cap sub = { .type = cap->type, .rights = cap->rights, .a = range_word(range_first(cap) + off, len) };
    return slot_store(t, arg[3], &sub, slot_node(t, slot));
}

/* arg[1..] carry the arguments in. */
static int op_irq_line(struct thread *t, uint32_t slot, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    uint32_t first = range_first(cap);
    uint32_t count = range_count(cap);

    switch (op) {
    case OP_IRQ_CARVE:
        return carve_range(t, slot, cap, arg);
    case OP_IRQ_BIND: {
        /* One Irq per line, so a binding names one line. */
        if (count != 1) {
            return KERR_INVALID_ARG;
        }
        struct cap pc;
        int err = slot_lookup_typed(t, arg[1], CAP_POOL, RIGHT_W, &pc);
        if (err != KERR_OK) {
            return err;
        }
        struct pool *pool = (struct pool *)cap_object(&pc);
        if (pool->dying) {
            return KERR_STATE;
        }
        /* The Irq signals on its creator's behalf, so the creator must be able to. */
        struct cap nc;
        err = slot_lookup_typed(t, arg[2], CAP_NOTIFICATION, RIGHT_W, &nc);
        if (err != KERR_OK) {
            return err;
        }
        /* The cores' line is bound already, to the kernel. */
        if (line_binding(first) != NULL || line_is_cores(first)) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the Irq capability, under whichever number names it. */
        if (slot_at(t, arg[3]) != slot_node(t, slot)) {
            err = slot_vacant(t, arg[3]);
            if (err != KERR_OK) {
                return err;
            }
        }
        /* Room is checked before the line's derivation goes, since what a revoke took stays taken. */
        if (!pool_fits(pool, obj_type_size(CAP_IRQ, 0))) {
            return KERR_NO_MEMORY;
        }
        if (!cap_revoke_below(slot_node(t, slot), slot, true)) {
            return KERR_PREEMPTED;
        }
        struct irq *irq = pool_alloc(pool, CAP_IRQ, obj_type_size(CAP_IRQ, 0));
        /* It hangs below the capability it was bound with, so a revoke above that disarms the Irq. */
        irq->ntfn = nc;
        irq->ntfn.b = held_word(v2p(irq), nc.b);
        cap_attach(slot_node(t, arg[2]), &irq->ntfn);
        irq->line = first;
        /* A period counts from the last deadline, and a line that never fired from its bind. */
        irq->deadline = timer_now();
        line_irq[first] = v2p(irq);
        /* The line goes, a leaf now, and the Irq hangs below the pool capability as any object does. */
        cap_delete(slot_node(t, slot), false);
        /* Its bits are zero, so it is disarmed, and its line stays masked as every line does until a set. */
        struct cap ic = cap_to_object(&irq->hdr, RIGHT_ALL);
        return slot_store(t, arg[3], &ic, slot_node(t, arg[1]));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in; see DESIGN.md, "Routes". */
static int op_route_line(struct thread *t, uint32_t slot, const struct cap *cap,
                         uint32_t op, const uint32_t *arg)
{
    uint32_t channel = range_first(cap);

    switch (op) {
    case OP_ROUTE_CARVE:
        return carve_range(t, slot, cap, arg);
    case OP_ROUTE_BIND: {
        /* One Route per channel, so a binding names one channel. */
        if (range_count(cap) != 1) {
            return KERR_INVALID_ARG;
        }
        struct cap pc;
        int err = slot_lookup_typed(t, arg[1], CAP_POOL, RIGHT_W, &pc);
        if (err != KERR_OK) {
            return err;
        }
        struct pool *pool = (struct pool *)cap_object(&pc);
        if (pool->dying) {
            return KERR_STATE;
        }
        if (channel_binding(channel) != NULL) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the Route capability, under whichever number names it. */
        if (slot_at(t, arg[2]) != slot_node(t, slot)) {
            err = slot_vacant(t, arg[2]);
            if (err != KERR_OK) {
                return err;
            }
        }
        /* Room is checked before the channel's derivation goes, since what a revoke took stays taken. */
        if (!pool_fits(pool, obj_type_size(CAP_ROUTE, 0))) {
            return KERR_NO_MEMORY;
        }
        if (!cap_revoke_below(slot_node(t, slot), slot, true)) {
            return KERR_PREEMPTED;
        }
        struct route *r = pool_alloc(pool, CAP_ROUTE, obj_type_size(CAP_ROUTE, 0));
        r->channel = (uint16_t)channel;
        channel_route[channel - ROUTE_FIRST] = v2p(r);
        /* The channel goes, a leaf now, and the Route hangs below the pool capability as any object does. */
        cap_delete(slot_node(t, slot), false);
        struct cap rc = cap_to_object(&r->hdr, RIGHT_ALL);
        return slot_store(t, arg[2], &rc, slot_node(t, arg[1]));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in. */
static int op_route(struct thread *t, const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    struct route *r = (struct route *)cap_object(cap);

    switch (op) {
    case OP_ROUTE_END: {
        uint32_t end = arg[1];
        if (end != ROUTE_EVENT && end != ROUTE_TASK) {
            return KERR_INVALID_ARG;
        }
        /* An event is heard, and a task done: the frame lets its holder read the one and write the other. */
        struct cap fc;
        int err = slot_lookup_typed(t, arg[2], CAP_FRAME, end == ROUTE_EVENT ? RIGHT_R : RIGHT_W, &fc);
        if (err != KERR_OK) {
            return err;
        }
        uint32_t word;
        if (!router_end(end, block_base(&fc), block_size(&fc), arg[3], &word)) {
            return KERR_INVALID_ARG;
        }
        /*
         * An end connected before is replaced; nothing is derived from it, so it goes in a step,
         * and its going disarms the Route, so the router never wires the new end to the other's old partner.
         */
        if (r->ends[end].type != CAP_NONE) {
            cap_delete(&r->ends[end], false);
        }
        /* It hangs below the capability it was connected through, so a revoke above that disarms the Route. */
        r->ends[end] = (struct cap){ .type = CAP_FRAME, .rights = fc.rights, .a = fc.a };
        r->ends[end].b = held_word(v2p(r), 0);
        cap_attach(slot_at(t, arg[2]), &r->ends[end]);
        r->names[end] = arg[3];
        return KERR_OK;
    }
    case OP_ROUTE_SET:
        if (arg[1] > 1) {
            return KERR_INVALID_ARG;
        }
        if (arg[1] == 1 && (r->ends[ROUTE_EVENT].type == CAP_NONE || r->ends[ROUTE_TASK].type == CAP_NONE)) {
            return KERR_STATE;
        }
        route_arm(r, arg[1] == 1);
        return KERR_OK;
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in; see DESIGN.md, "DMA". */
static int op_dma_line(struct thread *t, uint32_t slot, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    uint32_t channel = range_first(cap);

    switch (op) {
    case OP_DMA_CARVE:
        return carve_range(t, slot, cap, arg);
    case OP_DMA_BIND: {
        /* One Dma per channel, so a binding names one channel. */
        if (range_count(cap) != 1) {
            return KERR_INVALID_ARG;
        }
        struct cap pc;
        int err = slot_lookup_typed(t, arg[1], CAP_POOL, RIGHT_W, &pc);
        if (err != KERR_OK) {
            return err;
        }
        struct pool *pool = (struct pool *)cap_object(&pc);
        if (pool->dying) {
            return KERR_STATE;
        }
        if (dma_binding(channel) != NULL) {
            return KERR_OVERLAP;
        }
        /* The invoked slot is cleared below, so it may receive the Dma capability, under whichever number names it. */
        if (slot_at(t, arg[2]) != slot_node(t, slot)) {
            err = slot_vacant(t, arg[2]);
            if (err != KERR_OK) {
                return err;
            }
        }
        /* Room is checked before the channel's derivation goes, since what a revoke took stays taken. */
        if (!pool_fits(pool, obj_type_size(CAP_DMA, 0))) {
            return KERR_NO_MEMORY;
        }
        if (!cap_revoke_below(slot_node(t, slot), slot, true)) {
            return KERR_PREEMPTED;
        }
        struct dma *d = pool_alloc(pool, CAP_DMA, obj_type_size(CAP_DMA, 0));
        d->channel = (uint16_t)channel;
        channel_dma[channel - DMA_FIRST] = v2p(d);
        /* The channel goes, a leaf now, and the Dma hangs below the pool capability as any object does. */
        cap_delete(slot_node(t, slot), false);
        struct cap dc = cap_to_object(&d->hdr, RIGHT_ALL);
        return slot_store(t, arg[2], &dc, slot_node(t, arg[1]));
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/* arg[1..] carry the arguments in. */
static int op_dma(struct thread *t, const struct cap *cap, uint32_t op, const uint32_t *arg)
{
    struct dma *d = (struct dma *)cap_object(cap);

    switch (op) {
    case OP_DMA_END: {
        uint32_t end = arg[1];
        if (end != DMA_SOURCE && end != DMA_SINK) {
            return KERR_INVALID_ARG;
        }
        /* The source is read and the sink written: the frame lets its holder do the one or the other. */
        struct cap fc;
        int err = slot_lookup_typed(t, arg[2], CAP_FRAME, end == DMA_SOURCE ? RIGHT_R : RIGHT_W, &fc);
        if (err != KERR_OK) {
            return err;
        }
        uint32_t word;
        if (!dmac_end(d->channel, end, block_base(&fc), block_size(&fc), arg[3], &word)) {
            return KERR_INVALID_ARG;
        }
        /*
         * An end connected before is replaced; nothing is derived from it, so it goes in a step,
         * and its going halts the channel, so no transfer goes on with an end the Dma no longer holds.
         */
        if (d->ends[end].type != CAP_NONE) {
            cap_delete(&d->ends[end], false);
        }
        /* It hangs below the capability it was connected through, so a revoke above that halts the channel. */
        d->ends[end] = (struct cap){ .type = CAP_FRAME, .rights = fc.rights, .a = fc.a };
        d->ends[end].b = held_word(v2p(d), 0);
        cap_attach(slot_at(t, arg[2]), &d->ends[end]);
        d->names[end] = arg[3];
        return KERR_OK;
    }
    case OP_DMA_START: {
        uint32_t offset = arg[1], length = arg[2];
        if (offset % DMA_GRAIN != 0 || length % DMA_GRAIN != 0 || length == 0 || length > DMA_MOST) {
            return KERR_INVALID_ARG;
        }
        if (d->ends[DMA_SOURCE].type == CAP_NONE || d->ends[DMA_SINK].type == CAP_NONE || dma_busy(d)) {
            return KERR_STATE;
        }
        /* The range lies in the memory end's frame, which holds it as long as the transfer may reach it. */
        const struct cap *memory = &d->ends[dmac_memory(d->channel)];
        if (offset > block_size(memory) || length > block_size(memory) - offset) {
            return KERR_INVALID_ARG;
        }
        uint32_t device = dmac_memory(d->channel) == DMA_SOURCE ? DMA_SINK : DMA_SOURCE;
        dmac_start(d->channel, dma_word(d, device), block_base(memory) + offset, length);
        return KERR_OK;
    }
    case OP_DMA_STOP:
        dmac_halt(d->channel);
        return KERR_OK;
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
        struct cap tc;
        int err = slot_lookup_typed(t, arg[1], CAP_THREAD, RIGHT_W, &tc);
        if (err != KERR_OK) {
            return err;
        }
        /* The first unit lies within the capability even for none, so that the binding lies below it. */
        uint32_t off = arg[2];
        uint32_t count = arg[3];
        uint32_t span = arg[4];
        if (!span_valid(span)) {
            return KERR_INVALID_ARG;
        }
        if (off >= range_count(cap) || count > range_count(cap) - off) {
            return KERR_INVALID_ARG;
        }
        /* A place holds the time of one core, so the units it earns are of one. */
        uint32_t first = range_first(cap) + off;
        if (count != 0 && unit_core(first) != unit_core(first + count - 1)) {
            return KERR_INVALID_ARG;
        }
        /* A unit is earned by one place at a time; the thread's own on the core its place leaves as it binds. */
        struct thread *target = (struct thread *)cap_object(&tc);
        for (uint32_t i = 0; i < count; i++) {
            LOOP_BOUND(TIME_UNITS);
            paddr_t at = unit_place[first + i];
            if (at != 0 && place_thread(p2v(at)) != target) {
                return KERR_OVERLAP;
            }
        }
        /* The place of the core, or an empty one: none is a place too few. */
        struct place *p = sched_place_for(target, unit_core(first));
        if (p == NULL) {
            return KERR_LIMIT;
        }
        /* The binding hangs below the invoked capability, so a revoke above it empties the place. */
        sched_bind(p, first, count, cap->rights, slot_node(t, slot), span);
        return KERR_OK;
    }
    default:
        return KERR_WRONG_TYPE;
    }
}

/*
 * A period on a timer line, in counts: the deadline moves to the first count after now
 * a whole number of periods from the deadline before; returns the periods skipped, as many as a word holds.
 * A deadline ahead stays, unless it lies more than a period ahead, when it comes within one. See DESIGN.md, "Time".
 */
static uint32_t timer_period(struct irq *irq, uint64_t period)
{
    uint64_t now = timer_now(), rest;
    if (irq_due(irq, now)) {
        uint64_t skipped = timer_periods(now - irq->deadline, period, &rest);
        irq->deadline = now + period - rest;
        return skipped > UINT32_MAX ? UINT32_MAX : (uint32_t)skipped;
    }
    timer_periods(irq->deadline - now - 1, period, &rest);
    irq->deadline = now + rest + 1;
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
    bits &= notify_bits(&irq->ntfn);
    if (bits == 0 && arg[1] != 0) {
        return KERR_NO_RIGHTS;
    }
    if (line_is_timer(irq->line) && bits != 0) {
        bool period = arg[3] == IRQ_SET_PERIOD;
        if (arg[3] > IRQ_SET_PERIOD || (period && arg[2] == 0)) {
            return KERR_INVALID_ARG;
        }
        if (period) {
            arg[1] = timer_period(irq, timer_us_counts(arg[2]));
        } else {
            irq->deadline = delay_deadline(arg[2]);
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

/*
 * The results of a describing call, after its line, so that the replay compares its answers as well:
 * OP_CAP_INFO's four words, OP_POOL_INFO's two, as a3 and a4 are what its caller left there,
 * and OP_THREAD_OVERRUNS's one, beside the zero its a2 must be.
 * Out of line, and only under tracing, so that the call's own path saves no more registers for it.
 */
static __attribute__((noinline)) void trace_info(uint32_t op, const uint32_t *arg)
{
    kputs("trace: info a1=");
    kput_hex(arg[1]);
    kputs(" a2=");
    kput_hex(arg[2]);
    if (op == OP_CAP_INFO) {
        kputs(" a3=");
        kput_hex(arg[3]);
        kputs(" a4=");
        kput_hex(arg[4]);
    }
    kputc('\n');
}

/*
 * What a capability is and what it names, see OP_CAP_INFO: its type and rights in a1, and three words by type.
 * It looks at most at the slot's copy and at one thing more, the object or the node first below an Untyped.
 * Out of line, so that dispatch saves no more registers for it, on the stack's deepest path.
 */
static __attribute__((noinline)) int cap_info(const struct cap *cap, uint32_t *arg)
{
    /* a1 is the page, and only page 0 is told; see DESIGN.md, open decision 33. */
    if (arg[1] != 0 || arg[2] != 0 || arg[3] != 0) {
        return KERR_INVALID_ARG;
    }
    arg[4] = 0;
    switch (cap->type) {
    case CAP_FRAME:
        arg[2] = block_base(cap);
        arg[3] = block_size(cap);
        arg[4] = region_min_size();
        break;
    case CAP_UNTYPED:
        arg[2] = block_base(cap);
        arg[3] = block_size(cap);
        arg[4] = untyped_made(cap);
        break;
    case CAP_POOL: {
        const struct pool *pool = (const struct pool *)cap_object(cap);
        arg[2] = pool->size;
        arg[3] = pool_room(pool);
        arg[4] = pool->dying;
        break;
    }
    case CAP_CAPTABLE:
        arg[2] = ((const struct captable *)cap_object(cap))->nslots;
        break;
    case CAP_PROCESS:
        /* Its region slots, and the most a process may have here, an entry each: what OP_POOL_ALLOC holds a4 to. */
        arg[2] = ((const struct process *)cap_object(cap))->regions;
        arg[3] = pmp_entry_count;
        break;
    case CAP_NOTIFICATION:
        /* The bits it may signal, never those that are set, which only a wait takes, with RIGHT_R. */
        arg[2] = notify_bits(cap);
        break;
    case CAP_IRQ_LINE:
        /* Carving narrows within one grant, so a range is all timer lines or none. */
        arg[2] = range_first(cap);
        arg[3] = range_count(cap);
        arg[4] = line_is_timer(range_first(cap));
        break;
    case CAP_IRQ: {
        /* What the bind's capability may signal, zero once the notification was taken, and never what is armed. */
        const struct irq *irq = (const struct irq *)cap_object(cap);
        arg[2] = irq->line;
        arg[3] = irq_notification(irq) != NULL ? notify_bits(&irq->ntfn) : 0;
        arg[4] = line_is_timer(irq->line);
        break;
    }
    case CAP_TIME:
        arg[2] = range_first(cap);
        arg[3] = range_count(cap);
        break;
    case CAP_ROUTE_LINE:
        arg[2] = range_first(cap);
        arg[3] = range_count(cap);
        break;
    case CAP_ROUTE: {
        /* Which ends are connected and whether it is armed, never what the ends are. */
        const struct route *r = (const struct route *)cap_object(cap);
        arg[2] = r->channel;
        arg[3] = (r->ends[ROUTE_EVENT].type != CAP_NONE ? 1u << ROUTE_EVENT : 0) |
                 (r->ends[ROUTE_TASK].type != CAP_NONE ? 1u << ROUTE_TASK : 0);
        arg[4] = r->armed;
        break;
    }
    case CAP_DMA_LINE:
        arg[2] = range_first(cap);
        arg[3] = range_count(cap);
        break;
    case CAP_DMA: {
        /* Which ends are connected and whether a transfer may still reach the memory, never what the ends are. */
        const struct dma *d = (const struct dma *)cap_object(cap);
        arg[2] = d->channel;
        arg[3] = (d->ends[DMA_SOURCE].type != CAP_NONE ? 1u << DMA_SOURCE : 0) |
                 (d->ends[DMA_SINK].type != CAP_NONE ? 1u << DMA_SINK : 0);
        arg[4] = dma_busy(d);
        break;
    }
    case CAP_CLOCK:
        /* The rate and the default span, not the time, so no RIGHT_R. */
        arg[2] = timer_counter_hz();
        arg[3] = timer_default_span();
        break;
    case CAP_THREAD: {
        /* How many places it has, and the cores whose time they hold, a bit each. */
        const struct thread *th = (const struct thread *)cap_object(cap);
        arg[2] = th->places;
        for (uint32_t i = 0; i < th->places; i++) {
            LOOP_BOUND(CORES);
            if (th->time[i].type == CAP_TIME) {
                arg[3] |= 1u << unit_core(range_first(&th->time[i]));
            }
        }
        break;
    }
    default:
        /* Print, Halt and Test tell nothing more. */
        break;
    }
    arg[1] = cap->type | (uint32_t)cap->rights << 8;
    return KERR_OK;
}

/*
 * What dispatch checks of an operation before its handler, in the manual's order:
 * the type it is invoked on, the right it needs, and zero in what of a1 to a3 it does not read.
 * A code with no entry has type CAP_NONE, which no capability a slot resolves to has.
 * A right of 0 is none, or one the handler decides by what the call asks: a retype into a pool takes RIGHT_R and RIGHT_W.
 */
#define SHAPE_A1 0x1
#define SHAPE_A2 0x2
#define SHAPE_A3 0x4
#define SHAPE_A123 (SHAPE_A1 | SHAPE_A2 | SHAPE_A3)
#define SHAPE_RIGHTS_A3 0x8 /* a3 is a rights mask, zero past RIGHT_ALL */
#define SHAPE_VARIES 0x10   /* its own values decide what it reads; see shape_reads */
struct op_shape {
    uint8_t type;
    uint8_t right;
    uint8_t reads;
};
static const struct op_shape op_shapes[OP_COUNT] = {
    [OP_PRINT] = { CAP_PRINT, RIGHT_W, SHAPE_A123 },
    [OP_HALT] = { CAP_HALT, RIGHT_W, SHAPE_A1 },
    [OP_TEST_TRACE] = { CAP_TEST, RIGHT_W, 0 },
    [OP_TEST_TICK] = { CAP_TEST, RIGHT_W, 0 },
    [OP_TEST_IRQ] = { CAP_TEST, RIGHT_W, SHAPE_A1 },
    [OP_TEST_PREEMPT] = { CAP_TEST, RIGHT_W, SHAPE_A1 },
    [OP_CAP_COPY] = { CAP_CAPTABLE, RIGHT_W, SHAPE_A123 | SHAPE_RIGHTS_A3 },
    [OP_CAP_DELETE] = { CAP_CAPTABLE, RIGHT_W, SHAPE_A1 },
    [OP_CAP_REVOKE] = { CAP_CAPTABLE, RIGHT_W, SHAPE_A1 },
    [OP_CAP_DERIVE] = { CAP_CAPTABLE, RIGHT_W, SHAPE_A123 | SHAPE_RIGHTS_A3 },
    [OP_CAP_MOVE] = { CAP_CAPTABLE, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_FRAME_CARVE] = { CAP_FRAME, 0, SHAPE_A123 },
    [OP_UNTYPED_RETYPE] = { CAP_UNTYPED, 0, SHAPE_A1 | SHAPE_A2 },
    [OP_UNTYPED_SPLIT] = { CAP_UNTYPED, 0, SHAPE_A1 | SHAPE_A2 },
    [OP_POOL_ALLOC] = { CAP_POOL, RIGHT_W, SHAPE_A123 | SHAPE_VARIES },
    [OP_POOL_INFO] = { CAP_POOL, 0, SHAPE_A1 | SHAPE_A3 },
    [OP_PROCESS_INSTALL] = { CAP_PROCESS, RIGHT_W, SHAPE_A123 | SHAPE_RIGHTS_A3 },
    [OP_PROCESS_UNINSTALL] = { CAP_PROCESS, RIGHT_W, SHAPE_A1 },
    [OP_PROCESS_MOUNT] = { CAP_PROCESS, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_PROCESS_UNMOUNT] = { CAP_PROCESS, RIGHT_W, SHAPE_A1 },
    [OP_THREAD_CONFIGURE] = { CAP_THREAD, RIGHT_W, SHAPE_A123 },
    [OP_THREAD_RESUME] = { CAP_THREAD, RIGHT_W, 0 },
    [OP_THREAD_STOP] = { CAP_THREAD, RIGHT_W, 0 },
    [OP_THREAD_WATCH] = { CAP_THREAD, RIGHT_W, SHAPE_A1 | SHAPE_A2 | SHAPE_VARIES },
    [OP_THREAD_FAULT] = { CAP_THREAD, RIGHT_W, 0 },
    [OP_THREAD_OVERRUNS] = { CAP_THREAD, RIGHT_R, 0 },
    [OP_THREAD_READ_REG] = { CAP_THREAD, RIGHT_W, SHAPE_A1 },
    [OP_THREAD_WRITE_REG] = { CAP_THREAD, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_NOTIFY_SIGNAL] = { CAP_NOTIFICATION, RIGHT_W, SHAPE_A1 },
    [OP_NOTIFY_WAIT] = { CAP_NOTIFICATION, RIGHT_R, SHAPE_A1 },
    [OP_NOTIFY_LEND] = { CAP_NOTIFICATION, RIGHT_R, SHAPE_A1 | SHAPE_A2 },
    [OP_NOTIFY_CARVE] = { CAP_NOTIFICATION, 0, SHAPE_A1 | SHAPE_A2 },
    [OP_IRQ_CARVE] = { CAP_IRQ_LINE, 0, SHAPE_A123 },
    [OP_IRQ_BIND] = { CAP_IRQ_LINE, RIGHT_W, SHAPE_A123 },
    [OP_IRQ_SET] = { CAP_IRQ, RIGHT_W, SHAPE_A123 | SHAPE_VARIES },
    [OP_CLOCK_READ] = { CAP_CLOCK, RIGHT_R, 0 },
    [OP_CLOCK_WATCHDOG] = { CAP_CLOCK, RIGHT_W, SHAPE_A1 },
    [OP_TIME_CARVE] = { CAP_TIME, 0, SHAPE_A123 },
    [OP_TIME_BIND] = { CAP_TIME, RIGHT_W, SHAPE_A123 },
    [OP_ROUTE_CARVE] = { CAP_ROUTE_LINE, 0, SHAPE_A123 },
    [OP_ROUTE_BIND] = { CAP_ROUTE_LINE, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_ROUTE_END] = { CAP_ROUTE, RIGHT_W, SHAPE_A123 },
    [OP_ROUTE_SET] = { CAP_ROUTE, RIGHT_W, SHAPE_A1 },
    [OP_DMA_CARVE] = { CAP_DMA_LINE, 0, SHAPE_A123 },
    [OP_DMA_BIND] = { CAP_DMA_LINE, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_DMA_END] = { CAP_DMA, RIGHT_W, SHAPE_A123 },
    [OP_DMA_START] = { CAP_DMA, RIGHT_W, SHAPE_A1 | SHAPE_A2 },
    [OP_DMA_STOP] = { CAP_DMA, RIGHT_W, 0 },
};

/*
 * What of a1 to a3 an operation reads, by its shape and, for three, its own values:
 * an allocation of a notification reads no a3, a watch with no bits no a1,
 * and a set with no bits, or of a line that is no timer's, no a2 or a3.
 */
static uint8_t shape_reads(uint32_t op, const struct cap *cap, const uint32_t *arg, uint8_t reads)
{
    switch (op) {
    case OP_POOL_ALLOC:
        return arg[1] == CAP_NOTIFICATION ? reads & (uint8_t)~SHAPE_A3 : reads;
    case OP_THREAD_WATCH:
        return arg[2] == 0 ? reads & (uint8_t)~SHAPE_A1 : reads;
    case OP_IRQ_SET: {
        const struct irq *irq = (const struct irq *)cap_object(cap);
        return arg[1] == 0 || !line_is_timer(irq->line) ? reads & (uint8_t)~(SHAPE_A2 | SHAPE_A3) : reads;
    }
    default:
        return reads;
    }
}

static int shape_check(uint32_t op, const struct cap *cap, const uint32_t *arg)
{
    const struct op_shape *shape = op < OP_COUNT ? &op_shapes[op] : NULL;
    if (shape == NULL || shape->type != cap->type) {
        return KERR_WRONG_TYPE;
    }
    if ((cap->rights & shape->right) != shape->right) {
        return KERR_NO_RIGHTS;
    }
    uint8_t reads = shape->reads;
    if (reads & SHAPE_VARIES) {
        reads = shape_reads(op, cap, arg, reads);
    }
    uint32_t unread = ((reads & SHAPE_A1) ? 0 : arg[1]) | ((reads & SHAPE_A2) ? 0 : arg[2]) |
                      ((reads & SHAPE_A3) ? 0 : arg[3]) | ((reads & SHAPE_RIGHTS_A3) ? arg[3] & ~(uint32_t)RIGHT_ALL : 0);
    return unread != 0 ? KERR_INVALID_ARG : KERR_OK;
}

static int dispatch(struct thread *t, uint32_t op, uint32_t slot, uint32_t *arg)
{
    /*
     * A thread whose process was taken names nothing,
     * and one whose process mounts no table where the slot points names nothing there.
     * Within a call only a revoke takes either, or an unmount a table, and the call then returns.
     * The lookup is slot_lookup's spelt out, which keeps the compiler from inlining the handlers into this frame,
     * on the stack's deepest path; see DESIGN.md, "Bounded stack".
     */
    struct captable *table = slot_table(t, slot);
    if (table == NULL) {
        return KERR_INVALID_CAP;
    }
    struct cap cap;
    int err = cap_lookup(table, slot_index(slot), &cap);
    if (err != KERR_OK) {
        return err;
    }
    /* Describing grants no authority, so every type takes it, with no right, before the types' own checks. */
    if (op == OP_CAP_INFO) {
        return cap_info(&cap, arg);
    }

    /* The type it is invoked on, the right it needs, and zero in what it does not read; see op_shapes. */
    err = shape_check(op, &cap, arg);
    if (err != KERR_OK) {
        return err;
    }

    switch (cap.type) {
    case CAP_PRINT:
        err = op_print(arg);
        break;
    case CAP_HALT:
        err = op_halt(arg);
        break;
    case CAP_TEST:
        err = op_test(op, arg);
        break;
    case CAP_FRAME:
        err = op_frame(t, slot, &cap, op, arg);
        break;
    case CAP_UNTYPED:
        err = op_untyped(t, slot, &cap, op, arg);
        break;
    case CAP_CLOCK:
        err = op_clock(op, arg);
        break;
    case CAP_CAPTABLE:
        err = op_captable(t, slot, &cap, op, arg);
        break;
    case CAP_POOL:
        err = op_pool(t, slot, &cap, op, arg);
        break;
    case CAP_PROCESS:
        err = op_process(t, &cap, op, arg);
        break;
    case CAP_THREAD:
        err = op_thread(t, &cap, op, arg);
        break;
    case CAP_NOTIFICATION:
        err = op_notification(t, slot, &cap, op, arg);
        break;
    case CAP_IRQ_LINE:
        err = op_irq_line(t, slot, &cap, op, arg);
        break;
    case CAP_IRQ:
        err = op_irq(&cap, op, arg);
        break;
    case CAP_TIME:
        err = op_time(t, slot, &cap, op, arg);
        break;
    case CAP_ROUTE_LINE:
        err = op_route_line(t, slot, &cap, op, arg);
        break;
    case CAP_ROUTE:
        err = op_route(t, &cap, op, arg);
        break;
    case CAP_DMA_LINE:
        err = op_dma_line(t, slot, &cap, op, arg);
        break;
    case CAP_DMA:
        err = op_dma(t, &cap, op, arg);
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
    /* A stopped call is made again before a turn on spare time gives the processor up; see spare_end in sched.c. */
    core->recall = err == KERR_PREEMPTED;
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
        if ((op == OP_CAP_INFO || op == OP_POOL_INFO || op == OP_THREAD_OVERRUNS) && err == KERR_OK) {
            trace_info(op, arg);
        }
        trace_wake();
    }
    /*
     * The stop OP_TEST_PREEMPT armed is where a step of the clock lands, as OP_TEST_TICK does it:
     * the caller stays at its call instruction, ready, and the next thread has its turn.
     * Whatever the call's attempt woke is left untraced, as a stopped attempt's always is.
     */
    if (core->preempt_stopped) {
        core->preempt_stopped = false;
        if (debug_trace) {
            kputs("trace: preempt\n");
        }
        core->trace_wake_bits = 0;
        sched_tick();
        if (debug_trace) {
            trace_wake();
        }
    }
    /*
     * A thread that waits gives the processor up, and so does one whose process the call took, or that stopped itself,
     * unless it handed its turn to the thread it lends its time to, which runs now.
     * One that lost its time without spare time gives it up as the trap ends, after a preempted call is made again,
     * and a turn left on spare time a thread with time the call made ready takes at once.
     */
    if (core->current->state != THREAD_READY || core->run_queue != 0) {
        sched_call_end();
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
        sched_signal(watch, notify_bits(&t->watch));
    }
    if (debug_trace) {
        trace_wake();
    }
    sched_run_next();
    if (debug_trace) {
        selfcheck_run();
    }
}
