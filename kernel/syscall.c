/*
 * Capability invocation.
 * The ABI is documented in include/rvuos/abi.h.
 */

#include "kernel.h"
#include "object.h"

#define POOL_MIN_SIZE 64

/*
 * Not a status code: the thread blocked and is not returning yet.
 * Its registers are left alone until something wakes it.
 */
#define KERR_BLOCKED (-1)

bool debug_trace;

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
    default:
        return KERR_WRONG_TYPE;
    }
}

static int op_captable(struct thread *t, const struct cap *cap,
                       uint32_t op, const uint32_t *arg)
{
    struct captable *table = (struct captable *)cap_object(cap);

    switch (op) {
    case OP_CAP_COPY: {
        struct cap src;
        int err = cap_lookup(thread_table(t), arg[2], &src);
        if (err != KERR_OK) {
            return err;
        }
        src.rights &= (uint8_t)arg[3];
        return cap_store(table, arg[1], &src);
    }
    case OP_CAP_DELETE:
        return cap_clear(table, arg[1]);
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
        return KERR_OK;
    case OP_REGION_CARVE: {
        uint32_t off = arg[1];
        uint32_t len = arg[2];
        if ((off | len) & 3u || len == 0 || off > size || len > size - off) {
            return KERR_INVALID_ARG;
        }
        struct cap sub = cap_to_region(base + off, len, cap->rights);
        return cap_store(thread_table(t), arg[3], &sub);
    }
    case OP_REGION_TO_POOL: {
        if ((cap->rights & (RIGHT_R | RIGHT_W)) != (RIGHT_R | RIGHT_W)) {
            return KERR_NO_RIGHTS;
        }
        if ((base | size) & (OBJ_ALIGN - 1) || size < POOL_MIN_SIZE) {
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
        cap_clear(thread_table(t), slot);
        return cap_store(thread_table(t), arg[1], &pc);
    }
    default:
        return KERR_WRONG_TYPE;
    }
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

    pool_destroy(pool);

    /* The pools below gave their memory back to nobody; this one gives it to the caller. */
    struct cap rc = cap_to_region(base, size, rights);
    return cap_store(thread_table(t), arg[1], &rc);
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

    struct cap c = cap_to_object(obj, RIGHT_ALL);
    return cap_store(thread_table(t), dst, &c);
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
        return process_install(proc, arg[1], region.a, region.b, rights);
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

/*
 * A wake is traced after the line of the call that caused it.
 * A signal of no bits is refused, so zero here means nothing woke.
 */
static uint32_t trace_wake_bits;

/*
 * Hand a waiting thread the bits it waited for.
 * It resumes after its ecall with the status and the bits in place.
 */
static void wake(struct thread *t, uint32_t bits)
{
    t->frame.regs[REG_A0] = KERR_OK;
    t->frame.regs[REG_A1] = bits;
    t->waiting_on = 0;
    t->state = THREAD_READY;
    trace_wake_bits = bits;
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
        ntfn->bits |= arg[1];
        struct thread *waiter = sched_waiter(v2p(&ntfn->hdr));
        if (waiter != NULL) {
            wake(waiter, ntfn->bits);
            ntfn->bits = 0;
        }
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
        t->waiting_on = v2p(&ntfn->hdr);
        t->state = THREAD_WAITING;
        return KERR_BLOCKED;
    default:
        return KERR_WRONG_TYPE;
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
    struct cap cap;
    int err = cap_lookup(thread_table(t), slot, &cap);
    if (err != KERR_OK) {
        return err;
    }

    /*
     * Operations that change an object need RIGHT_W on it.
     * Regions, notifications and the debug capability
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
    /* a0 is the slot, a1..a3 the arguments and, on return, the results. */
    uint32_t in[4], arg[4];
    for (unsigned i = 0; i < 4; i++) {
        in[i] = arg[i] = f->regs[REG_A0 + i];
    }

    /* ecall is a 4-byte instruction; resume after it. */
    f->mepc += 4;

    /* The call that turns tracing on is not itself traced. */
    bool traced = debug_trace;

    trace_wake_bits = 0;
    int err = dispatch(t, op, arg[0], arg);
    if (err != KERR_BLOCKED) {
        f->regs[REG_A0] = (uint32_t)err;
        for (unsigned i = 1; i < 4; i++) {
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
