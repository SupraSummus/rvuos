/*
 * Replacements for the hardware-facing parts of the kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csr.h"
#include "harness.h"
#include "irq.h"
#include "klog.h"
#include "timer.h"
#include "trap.h"

_Static_assert(HOST_RAM_BASE == RAM_BASE && HOST_RAM_SIZE == RAM_SIZE,
               "host RAM must match the kernel's layout");
_Static_assert(REPLAY_THREAD_SP > USER_DATA_BASE && REPLAY_THREAD_SP <= USER_DATA_BASE + USER_DATA_SIZE &&
               REPLAY_THIRD_SP > USER_DATA_BASE && REPLAY_THIRD_SP <= USER_DATA_BASE + USER_DATA_SIZE,
               "the replay driver's stacks must lie in its data region");
_Static_assert(REPLAY_OWN_BASE == KLOG_BASE && KLOG_BASE + KLOG_REGION_SIZE == USER_CODE_BASE &&
               USER_CODE_BASE + USER_CODE_SIZE == USER_DATA_BASE && REPLAY_OWN_END == USER_DATA_BASE + USER_DATA_SIZE &&
               REPLAY_UART_BASE == UART_BASE && REPLAY_UART_END == UART_BASE + UART_SIZE,
               "an access record leaves alone the log, the driver's code and data, and the UART");

uint8_t *host_ram;
/* What RAM holds before anything writes it. */
#define HOST_RAM_PATTERN 0xa5
/* The driver's threads, by actor number; see host_event. */
static struct thread *host_threads[REPLAY_THREADS + 1];
/*
 * Which of them drain the log before their next call, by actor number:
 * a thread that performed a record drains next, when it runs again if the record blocked it,
 * while one that passed a record on, or has not begun, looks at the cursor first.
 * A fault leaves a thread where it was, and so does a resume after one.
 */
static bool host_drains[REPLAY_THREADS + 1];
/*
 * Which of them stand at the instruction of a record they have yet to make, by actor number:
 * the ecall of a call, whose registers are loaded, or a load or a store, see rvuos/replay.h.
 * A thread stands there from when it takes the record, and again after a stop OP_DEBUG_PREEMPT armed
 * or a fault on the access, until host_run_on finds it running.
 */
enum { HOST_ON, HOST_AT_CALL, HOST_AT_ACCESS };
static uint8_t host_at[REPLAY_THREADS + 1];
static struct replay_record host_access_at[REPLAY_THREADS + 1];
jmp_buf host_halt_jmp;
int host_halt_code;
bool host_verbose;
bool host_isolated;
unsigned pmp_entry_count;
unsigned pmp_entry_end;
uint32_t pmp_grain;

/* The simulated core's grain. QEMU has 4; host/build.mk also builds RP2350's Hazard3, with 32. */
#ifndef PMP_GRAIN
#define PMP_GRAIN 4
#endif
_Static_assert(PMP_GRAIN >= 4 && (PMP_GRAIN & (PMP_GRAIN - 1)) == 0,
               "the PMP grain is a power of two of at least four bytes");

/*
 * PMP_HARDWIRED builds RP2350's Hazard3: eight entries that take a write,
 * then three that take none, NAPOT with every right over the boot ROM, the peripherals and SIO,
 * as its pmpaddr8 to pmpaddr10 read.
 */
#ifdef PMP_HARDWIRED
#define PMP_WRITABLE 8
static const uint32_t pmp_hardwired_addr[] = { 0x01ffffff, 0x13ffffff, 0x35ffffff };
#define PMP_HARDWIRED_COUNT (sizeof(pmp_hardwired_addr) / sizeof(pmp_hardwired_addr[0]))
_Static_assert(PMP_WRITABLE + PMP_HARDWIRED_COUNT <= PMP_MAX_ENTRIES, "the hardwired entries lie within the CSRs");
#else
#define PMP_WRITABLE PMP_MAX_ENTRIES
#define PMP_HARDWIRED_COUNT 0
#endif

/* Each core's PMP CSRs as last written, the hardwired entries as the core holds them. */
static uint32_t pmp_addrs[CORES][PMP_MAX_ENTRIES];
static uint8_t pmp_cfgs[CORES][PMP_MAX_ENTRIES];
#define pmp_addr (pmp_addrs[core_index()])
#define pmp_cfg (pmp_cfgs[core_index()])

#if CORES > 1
/*
 * The core the kernel runs on: the one a record goes to, see host_pick_core,
 * or the one an interrupt went to, whose trap the host takes once the trap that raised it is over.
 * Of the ways the cores' traps can follow each other on the target, that is one:
 * the core traps as the interrupt comes, its registers saved, and waits for the lock the trap holds.
 */
static uint32_t host_core;
static bool host_ipi[CORES];

uint32_t core_id(void)
{
    return host_core;
}

/*
 * The core traps once the trap that interrupts it gives the lock up, see host_interrupts,
 * unless that trap waits for it in core_shoot, where it traps at once.
 */
void ipi_enable(void)
{
}

void ipi_send(uint32_t core)
{
    host_ipi[core] = true;
}

void host_core_traps(uint32_t core)
{
    if (!host_ipi[core]) {
        fprintf(stderr, "invariant violated: a core waits for another it did not interrupt\n");
        host_violated();
    }
    __atomic_store_n(&cores[core].in_user, false, __ATOMIC_RELEASE);
}

void ipi_clear(void)
{
    host_ipi[host_core] = false;
}
#endif

/* The last line the kernel printed, ended or not, which selfcheck_fail reports when the transcript is not printed. */
static char host_line[160];
static size_t host_line_len;
static bool host_line_ended;

/*
 * The log takes every byte, as on the target, so its head and its line agree
 * with QEMU's; the transcript is the same bytes as they come.
 */
void kputc(char c)
{
    klog_append(c);
    if (host_verbose) {
        putchar(c);
    }
    if (host_line_ended) {
        host_line_len = 0;
        host_line_ended = false;
    }
    if (c == '\n') {
        host_line_ended = true;
    } else if (host_line_len < sizeof(host_line)) {
        host_line[host_line_len++] = c;
    }
}

void khalt(int code)
{
    host_halt_code = code;
    longjmp(host_halt_jmp, 1);
}

void kpanic(const char *msg)
{
    /* A panic is reachable only through a kernel bug. */
    fprintf(stderr, "kernel panic: %s\n", msg);
    abort();
}

void host_outside_ram(uint32_t p)
{
    fprintf(stderr, "invariant violated: the kernel reached outside RAM 0x%08x\n", (unsigned)p);
    host_violated();
}

/* ASan goes on after a report, see host_asan_report, and reports each, not only the first from a place. */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
    return "halt_on_error=0:suppress_equal_pcs=0";
}

/* The kernel reached poisoned RAM in an isolated input; see host_asan_report. */
static bool poison_reached;

/*
 * ASan found an access to poison, see ram_poison, and makes it once this returns.
 * In RAM that is the kernel reaching outside its objects, which ends the run as any invariant does.
 * An isolated input cannot longjmp out of ASan, so its call goes on, with no poison left to report again,
 * and the input ends as the call returns; see host_syscall.
 * Anywhere else it is the harness's own bug.
 */
static void host_asan_report(const char *report)
{
    (void)report;
    const uint8_t *at = __asan_get_report_address();
    if (host_ram == NULL || at < host_ram || at >= host_ram + HOST_RAM_SIZE) {
        abort();
    }
    fprintf(stderr, "invariant violated: the kernel reached RAM outside its objects 0x%08x\n",
            (unsigned)(HOST_RAM_BASE + (uint32_t)(at - host_ram)));
    if (!host_isolated) {
        host_violated();
    }
    ram_unpoison(host_ram, HOST_RAM_SIZE);
    poison_reached = true;
}

void host_violated(void)
{
    /* The report may have gone to stdout; make sure it is seen. */
    fflush(stdout);
    if (host_isolated) {
        longjmp(host_halt_jmp, 1);
    }
    abort();
}

/* The report is the last line the kernel printed; see fail in kernel/selfcheck.c. */
void selfcheck_fail(void)
{
    if (!host_verbose) {
        fprintf(stderr, "%.*s\n", (int)host_line_len, host_line);
    }
    host_violated();
}

bool intr_wait(uint32_t wake, uint32_t *ticks)
{
    (void)wake;
    (void)ticks;
    /* Reached only untraced with an Irq armed, which the harness never sets up; a core of several idles with sched_idle_sleep. */
    kpanic("the host build has no clock and no devices to wait for");
}

/* The host is QEMU's board, whose counter runs at a rate fixed in board.h. */
uint32_t timer_counter_hz(void)
{
    return COUNTER_HZ;
}

uint32_t timer_tick_counts(void)
{
    return COUNTER_HZ / TIMER_HZ;
}

uint64_t timer_now(void)
{
    return (uint64_t)sched_ticks * timer_tick_counts();
}

/* QEMU's board has no watchdog of its own. */
void board_watchdog(uint32_t us)
{
    (void)us;
}

/*
 * The host's clock is the tick count, and the scheduler asks where the counter is only untraced.
 * Half way into the tick stands for wherever QEMU's is when the replay driver turns tracing on,
 * so that OP_DEBUG_TRACE has an offset to clear.
 */
uint32_t timer_offset(void)
{
    return timer_tick_counts() / 2;
}

/* The worst case: every preemptible call stops after its first step. */
bool intr_pending(void)
{
#ifdef RVUOS_WORK
    work_ask();
#endif
    return true;
}

/* The interrupt controller as far as the kernel touches it: which lines it forwards. */
static uint32_t forwarded[(IRQ_LINES + 31) / 32];

void irq_init(void)
{
    memset(forwarded, 0, sizeof(forwarded));
}

void irq_enable(uint32_t line, bool on)
{
    uint32_t bit = 1u << (line % 32);
    forwarded[line / 32] = on ? (forwarded[line / 32] | bit) : (forwarded[line / 32] & ~bit);
}

bool irq_enabled(uint32_t line)
{
    return (forwarded[line / 32] >> (line % 32)) & 1u;
}

uint32_t irq_claim(void)
{
    /* No device ever raises a line here; OP_DEBUG_IRQ fires them by name instead. */
    return 0;
}

void irq_complete(uint32_t line)
{
    (void)line;
}

void board_user_csrs_switch(struct process *from, struct process *to)
{
    (void)from;
    (void)to;
}

void pmp_init(void)
{
    memset(pmp_addrs, 0, sizeof(pmp_addrs));
    memset(pmp_cfgs, 0, sizeof(pmp_cfgs));
#ifdef PMP_HARDWIRED
    for (unsigned c = 0; c < CORES; c++) {
        for (unsigned i = 0; i < PMP_HARDWIRED_COUNT; i++) {
            pmp_addrs[c][PMP_WRITABLE + i] = pmp_hardwired_addr[i];
            pmp_cfgs[c][PMP_WRITABLE + i] = PMP_A_NAPOT | PMP_R | PMP_W | PMP_X;
        }
    }
#endif
    pmp_entry_count = PMP_WRITABLE;
    pmp_entry_end = PMP_WRITABLE + PMP_HARDWIRED_COUNT;
    pmp_grain = PMP_GRAIN;
}

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    if (idx >= PMP_MAX_ENTRIES) {
        kpanic("pmp_set index out of range");
    }
    if (idx >= PMP_WRITABLE) {
        return;
    }
    /*
     * Bits below the grain read as the specification says:
     * with G = log2(grain) - 2, bits G-1..0 read as zeros in the other modes
     * and bits G-2..0 as ones under NAPOT; the self-check sees any difference.
     */
    if ((cfg & 0x18) == PMP_A_NAPOT) {
        pmp_addr[idx] = addr | (uint32_t)(PMP_GRAIN >= 8 ? PMP_GRAIN / 8 - 1 : 0);
    } else {
        pmp_addr[idx] = addr & ~(uint32_t)(PMP_GRAIN / 4 - 1);
    }
    pmp_cfg[idx] = cfg;
}

void pmp_clear(unsigned idx)
{
    if (idx >= PMP_MAX_ENTRIES) {
        kpanic("pmp_clear index out of range");
    }
    if (idx >= PMP_WRITABLE) {
        return;
    }
    pmp_addr[idx] = 0;
    pmp_cfg[idx] = 0;
}

void pmp_get(unsigned idx, uint32_t *addr, uint8_t *cfg)
{
    if (idx >= PMP_MAX_ENTRIES) {
        kpanic("pmp_get index out of range");
    }
    *addr = pmp_addr[idx];
    *cfg = pmp_cfg[idx];
}

struct thread *host_boot(void)
{
#ifdef RVUOS_WORK
    /* The boot runs kernel code before the first call's work_begin. */
    work_reset();
#endif
    /*
     * RAM holds anything at power-up; a pattern stands for it,
     * so an object the kernel hands out without zeroing shows.
     */
    bool power_up = host_ram == NULL || host_isolated;
    if (host_ram == NULL) {
        host_ram = malloc(HOST_RAM_SIZE);
        if (host_ram == NULL) {
            abort();
        }
        __asan_set_error_report_callback(host_asan_report);
    }
    if (power_up) {
        ram_unpoison(host_ram, HOST_RAM_SIZE);
        memset(host_ram, HOST_RAM_PATTERN, HOST_RAM_SIZE);
    }
    /* The kernel holds no object yet, and may touch its log alone; see ram_poison. */
    ram_poison(host_ram, HOST_RAM_SIZE);
    ram_unpoison(p2v(KLOG_BASE), KLOG_REGION_SIZE);
    poison_reached = false;
    /*
     * The kernel clears each object as it hands it out,
     * so RAM may keep the previous run's contents, the boot pool's too,
     * exactly as on a warm reset; an isolated input starts from power-up instead.
     */
    pool_list = NULL;
    memset(line_irq, 0, LINES * sizeof(line_irq[0]));
    for (unsigned c = 0; c < CORES; c++) {
        cores[c] = (struct core){ .nearest_release = NEAREST_NONE, .nearest_deadline = NEAREST_NONE };
    }
#if CORES > 1
    kernel_lock = (struct kernel_lock){ 0 };
    host_core = 0;
    memset(host_ipi, 0, sizeof(host_ipi));
#endif
    memset(unit_thread, 0, sizeof(unit_thread));
    armed_sources = 0;
    sched_ticks = 0;
    watchdog = (struct watchdog){ 0 };
    debug_trace = false;
    preempt_countdown = 0;
    pmp_init();
    process_fence();
    irq_init();
    klog_init();

    /* The layout is the target's, the log and the boot pool included, so the PMP images of the two builds agree. */
    struct thread *root = boot_create_root();
    sched_start(root);
    process_activate(thread_process(root));
#if CORES > 1
    /* The root task runs in user mode, and the other cores start, see core_start, and idle in their stall. */
    cores[0].in_user = true;
    for (host_core = 1; host_core < CORES; host_core++) {
        process_fence_core();
        sched_idle_sleep();
    }
    host_core = 0;
#endif

    /*
     * What the replay driver does before it turns tracing on,
     * from the records both builds share; see rvuos/replay.h.
     */
    for (unsigned i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        int err = (int)host_syscall(&replay_prologue[i]);
        if (err != KERR_OK) {
            fprintf(stderr, "invariant violated: a fresh kernel refused prologue record %u with %d\n", i, err);
            host_violated();
        }
    }
    for (unsigned i = 0; i < REPLAY_AFTER_INPUT_COUNT; i++) {
        int err = (int)host_syscall(&replay_after_input[i]);
        if (err != KERR_OK) {
            fprintf(stderr, "invariant violated: a fresh kernel refused prologue record %u with %d\n",
                    (unsigned)(REPLAY_PROLOGUE_COUNT + i), err);
            host_violated();
        }
    }

    static const uint16_t slots[] = { REPLAY_CAP_THREAD, REPLAY_CAP_THIRD };
    _Static_assert(sizeof(slots) / sizeof(slots[0]) == REPLAY_THREADS - 1, "a slot per thread but the root");
    memset(host_drains, 0, sizeof(host_drains));
    memset(host_at, 0, sizeof(host_at));
    host_threads[1] = root;
    for (unsigned i = 0; i < REPLAY_THREADS - 1; i++) {
        struct cap c;
        if (cap_lookup(thread_table(root), slots[i], &c) != KERR_OK || c.type != CAP_THREAD) {
            abort();
        }
        host_threads[i + 2] = (struct thread *)cap_object(&c);
    }
    return root;
}

static void host_interrupts(void);

/*
 * The core goes back to user mode, or with nobody's turn to its stall, see sched_idle, which gives the lock up,
 * unless nothing can ever run again.
 */
static void host_leave(void)
{
    if (core_self()->turn != NULL) {
        core_leave();
    } else {
        sched_idle_sleep();
        core_stall_begin();
    }
}

/*
 * A trap of the running thread, a call or a fault, between the checks every trap gets,
 * and what the trap does on its way back, which the self-check of the next call holds;
 * then the traps of the cores it interrupted.
 */
static void host_trap(bool call)
{
    /* What another core counted since this one last trapped, as timer_trap_enter counts it before the call. */
    core_enter();
    sched_count(0);
    history_begin(call);
#ifdef RVUOS_WORK
    work_begin();
#endif
    if (call) {
        syscall_dispatch(core_self()->current);
    } else {
        fault_dispatch(core_self()->current);
    }
    sched_wake();
    host_leave();
    /* The report was made as the kernel reached poisoned RAM; see host_asan_report. */
    if (poison_reached) {
        host_violated();
    }
#ifdef RVUOS_WORK
    work_end();
#endif
    history_end();
    host_interrupts();
}

/*
 * Every core another core's trap interrupted traps in turn, as the target's trap_handler and sched_idle have it:
 * one that idles wakes in its stall and looks for a turn,
 * and one that runs a thread hands the processor on if another core stopped or destroyed the thread,
 * through the kernel's own core_trapped, as trap_handler does.
 * Either may interrupt others again, and the host takes their traps too, until none is left.
 * Under tracing the self-check holds what each left.
 */
static void host_interrupts(void)
{
#if CORES > 1
    uint32_t back = host_core;
    for (unsigned rounds = 0;; rounds++) {
        unsigned c = 0;
        while (c < CORES && !host_ipi[c]) {
            c++;
        }
        if (c == CORES) {
            break;
        }
        if (rounds == 4 * CORES * (REPLAY_THREADS + 1)) {
            fprintf(stderr, "invariant violated: the cores interrupt each other for ever\n");
            host_violated();
        }
        host_core = c;
        struct core *core = core_self();
        if (core->turn == NULL) {
            core_stall_end();
            ipi_clear();
            if (sched_idle_wake(0, false)) {
                sched_wake();
            }
        } else {
            core_enter();
            sched_count(0);
            ipi_clear();
            core_trapped();
            sched_wake();
        }
        if (debug_trace) {
            selfcheck_run();
        }
        host_leave();
        if (poison_reached) {
            host_violated();
        }
    }
    /* A core idles only with no thread waiting for a turn there, or its interrupt would have woken it. */
    for (uint32_t c = 0; c < CORES; c++) {
        if (cores[c].turn == NULL && (cores[c].run_queue != 0 || cores[c].spare_queue != 0)) {
            fprintf(stderr, "invariant violated: a thread waits for a turn on a core that idles\n");
            host_violated();
        }
    }
    host_core = back;
#endif
}

/* The actor number of a thread; 0 when it is not a driver thread, which never runs traced. */
static unsigned host_actor_of(const struct thread *t)
{
    for (unsigned i = 1; i <= REPLAY_THREADS; i++) {
        /* Compared, never followed: a destroyed thread is simply never current again. */
        if (host_threads[i] == t) {
            return i;
        }
    }
    return 0;
}

static unsigned host_actor(void)
{
    return host_actor_of(core_self()->current);
}

/*
 * The running thread makes the call its registers hold, a record's when `record`,
 * after which the thread drains the log.
 * The host stops every restartable call after each step, and the thread makes it again at once,
 * as it would once the interrupt is taken;
 * after a stop OP_DEBUG_PREEMPT armed it stands at its ecall, since another thread may run first.
 */
static void host_call(bool record)
{
    struct thread *caller = core_self()->current;
    struct trap_frame *f = &caller->frame;
    uint32_t op = f->regs[REG_A7], slot = f->regs[REG_A0];
    bool stopped;
    do {
        uint32_t pc = f->pc, ticks = sched_ticks;
        unsigned before[UNITS];
        host_units(before);
        host_trap(true);
        /* A step of a preemptible walk takes a unit away, and it stops only after one. */
        unsigned after[UNITS];
        host_units(after);
        bool took = false;
        for (unsigned u = 0; u < UNITS; u++) {
            took |= after[u] < before[u];
        }
        stopped = f->pc == pc;
        /* Under tracing only OP_DEBUG_TICK moves time, and the tick an armed stop lands. */
        bool armed = debug_trace && sched_ticks != ticks && op != OP_DEBUG_TICK;
        if (stopped && !took) {
            fprintf(stderr, "invariant violated: a call was preempted before it took anything away\n");
            host_violated();
        }
        if (armed && !stopped) {
            fprintf(stderr, "invariant violated: a call went on past the stop of an armed tick\n");
            host_violated();
        }
        /*
         * The thread makes a stopped call again, so it runs on and can still name what it invoked:
         * the slot resolved as the call began, and nothing refills a slot within a call.
         * Only an armed stop hands the processor on.
         */
        if (stopped && ((core_self()->current != caller && !armed) || thread_table(caller) == NULL ||
                        thread_table(caller)->slots[slot].type == CAP_NONE)) {
            fprintf(stderr, "invariant violated: a call stopped where its caller cannot make it again\n");
            host_violated();
        }
        if (armed) {
            host_at[host_actor_of(caller)] = HOST_AT_CALL;
            return;
        }
    } while (stopped);
    if (record) {
        host_drains[host_actor_of(caller)] = true;
    }
}

static void host_load(const struct replay_record *c)
{
    struct trap_frame *f = &core_self()->current->frame;
    f->regs[REG_A7] = c->op;
    f->regs[REG_A0] = c->slot;
    f->regs[REG_A1] = c->a1;
    f->regs[REG_A2] = c->a2;
    f->regs[REG_A3] = c->a3;
    f->regs[REG_A4] = 0;
    f->regs[REG_A5] = 0;
    f->regs[REG_A6] = 0;
    f->mcause = 8;
}

/*
 * The record goes to whichever thread the kernel is running,
 * which is what happens on the target: the driver's threads
 * take records from one cursor.
 * A call that blocks leaves its thread's registers alone,
 * so the status returned here is that thread's previous one;
 * only the setup, which never blocks, looks at it.
 */
uint32_t host_syscall(const struct replay_record *c)
{
    struct thread *caller = core_self()->current;
    host_load(c);
    host_call(false);
    return caller->frame.regs[REG_A0];
}

static void count_node(const struct cap *c, unsigned out[UNITS])
{
    if (c->type != CAP_NONE) {
        out[UNIT_NODE]++;
        if (c->next != LINK_UP) {
            out[UNIT_LINK]++;
        }
    }
}

void host_units(unsigned out[UNITS])
{
    memset(out, 0, UNITS * sizeof(out[0]));
    for (struct obj_header *o = object_first(); o != NULL; o = object_next(o)) {
        out[UNIT_OBJECT]++;
        uint32_t count;
        const struct cap *nodes = obj_nodes(o, &count);
        for (uint32_t i = 0; i < count; i++) {
            count_node(&nodes[i], out);
        }
        if (o->type == CAP_THREAD && ((struct thread *)o)->state == THREAD_WAITING) {
            out[UNIT_WAITER]++;
        }
    }
}

static bool mapped_with(const struct process *proc, uint32_t base, uint32_t size, uint8_t rights)
{
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct cap *s = &proc->slots[i];
        /* base within the slot first, or the remaining length below wraps. */
        if (s->type != CAP_NONE && base - s->a < s->b && size <= s->b - (base - s->a) &&
            (s->rights & rights) == rights) {
            return true;
        }
    }
    return false;
}

/* Whether the running thread's process maps the driver's code, which is all an ecall made again needs. */
static bool code_mapped(void)
{
    const struct process *proc = thread_process(core_self()->current);
    return proc != NULL && mapped_with(proc, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X);
}

/* Whether the running thread's process maps the driver's code and data, and to drain the log, the UART and the log. */
static bool driver_mapped(bool drain)
{
    const struct process *proc = thread_process(core_self()->current);
    return code_mapped() && mapped_with(proc, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W) &&
           (!drain || (mapped_with(proc, UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W) &&
                       mapped_with(proc, KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W)));
}

bool host_driver_alive(void)
{
    return driver_mapped(host_drains[host_actor()]);
}

/*
 * The running thread faults as the trap would have it.
 * The pc stays the host's own count, since the host knows no address of the driver's code;
 * the report prints it, and nothing compares it.
 */
void host_fault(uint32_t cause)
{
    struct trap_frame *f = &core_self()->current->frame;
    f->mcause = cause;
    f->mtval = cause == CAUSE_BREAKPOINT ? 0 : f->pc;
    host_trap(false);
}

/* A word RAM holds wherever an object may lie, so ASan must not see the store. */
__attribute__((no_sanitize("address"))) static void host_store(uint32_t at, uint32_t word)
{
    *(uint32_t *)p2v(at) = word;
}

/*
 * The running driver thread makes a load or a store record, see rvuos/replay.h,
 * if its process maps the word with the right; else it is left the cause of its fault.
 * Only a word of RAM holds what is stored; a device's is the host's to leave out.
 */
static bool host_access(const struct replay_record *c, uint32_t *cause)
{
    uint32_t at = c->a1 & ~3u;
    bool store = c->op == REPLAY_OP_STORE;
    if (!replay_leaves_alone(at)) {
        if (!mapped_with(thread_process(core_self()->current), at, 4, store ? RIGHT_W : RIGHT_R)) {
            *cause = store ? CAUSE_STORE_ACCESS : CAUSE_LOAD_ACCESS;
            return false;
        }
        if (store && at - HOST_RAM_BASE < HOST_RAM_SIZE) {
            host_store(at, c->a2);
        }
    }
    host_drains[host_actor()] = true;
    return true;
}

/*
 * The running driver thread goes on as the driver's code does, up to its next look at the cursor:
 * one that stands at a record makes it, one that cannot run on faults, see host_driver_alive,
 * and the kernel hands the processor on, maybe to another such thread.
 * Nothing but a record resumes a thread, so each faults once;
 * one that drains the log leaves the mark the drain leaves in its header.
 */
static void host_run_on(void)
{
    unsigned faults = 0;
    for (;;) {
        /* A core of several that has nobody's turn idles, the last thread it ran stopped or waiting. */
        if (core_self()->turn == NULL) {
            return;
        }
        unsigned actor = host_actor();
        uint8_t at = host_at[actor];
        uint32_t cause = CAUSE_INSN_ACCESS;
        /* A thread at its record's instruction needs only the code it fetches it from. */
        bool runs = at == HOST_ON ? host_driver_alive() : code_mapped();
        if (runs && at == HOST_ON) {
            break;
        }
        if (runs && at == HOST_AT_CALL) {
            host_at[actor] = HOST_ON;
            host_call(true);
            continue;
        }
        if (runs && host_access(&host_access_at[actor], &cause)) {
            host_at[actor] = HOST_ON;
            continue;
        }
        if (faults++ == REPLAY_THREADS) {
            fprintf(stderr, "invariant violated: a thread ran again after every driver thread faulted\n");
            host_violated();
        }
        host_fault(cause);
    }
    if (host_drains[host_actor()]) {
        volatile struct rvuos_log *log = p2v(KLOG_BASE);
        log->taken = log->head;
        host_drains[host_actor()] = false;
    }
}

#if CORES > 1
/* Whether a thread waits on a queue of the core, which holds only live threads, for its turn there. */
static bool host_queued(const struct core *core, const struct thread *t)
{
    const paddr_t heads[] = { core->run_queue, core->spare_queue, core->spent_queue };
    for (unsigned q = 0; q < sizeof(heads) / sizeof(heads[0]); q++) {
        paddr_t at = heads[q];
        while (at != 0) {
            const struct thread *w = p2v(at);
            if (w == t) {
                return true;
            }
            at = w->queue_next == heads[q] ? 0 : w->queue_next;
        }
    }
    return false;
}

/*
 * A trap on one core may have handed another a driver thread that stands at a record, or one that cannot run:
 * each such core's thread runs on as host_run_on has it, until every core's stands at the cursor or the core idles.
 */
static void host_settle_cores(void)
{
    for (unsigned rounds = 0;; rounds++) {
        uint32_t c = 0;
        for (; c < CORES; c++) {
            if (cores[c].turn == NULL) {
                continue;
            }
            host_core = c;
            if (host_at[host_actor()] != HOST_ON || !host_driver_alive()) {
                break;
            }
        }
        if (c == CORES) {
            return;
        }
        if (rounds == CORES * (REPLAY_THREADS + 1)) {
            fprintf(stderr, "invariant violated: the cores hand driver threads to each other for ever\n");
            host_violated();
        }
        host_run_on();
    }
}

/* The first core whose turn it is, from the one the host runs on; CORES for none. */
static uint32_t host_core_with_turn(void)
{
    for (uint32_t i = 0; i < CORES; i++) {
        uint32_t c = (host_core + i) % CORES;
        if (cores[c].turn != NULL) {
            return c;
        }
    }
    return CORES;
}
#else
static void host_settle_cores(void)
{
}
#endif

/*
 * The core a record goes to with more than one, as the driver's threads on each look at the cursor:
 * the core whose turn its actor has, or else the one it waits on for its turn, where the passing goes round;
 * a record for any thread, for one that can have no turn, or for one that waits on a core that idles,
 * goes where the last went, or to the next core whose turn it is if that one idles.
 * Some core has a turn, since the kernel stops the machine when none can have one again.
 * Its driver thread, or one that stands at a record there, then runs on; see host_run_on.
 */
static void host_pick_core(const struct replay_record *c)
{
#if CORES > 1
    /* Compared, never followed, unless the thread lies on a queue: a destroyed thread is simply never current again. */
    const struct thread *actor = c->actor >= 1 && c->actor <= REPLAY_THREADS ? host_threads[c->actor] : NULL;
    for (uint32_t i = 0; actor != NULL && i < CORES; i++) {
        if (cores[i].turn == actor || (cores[i].turn != NULL && host_queued(&cores[i], actor))) {
            host_core = i;
            return;
        }
    }
    if (cores[host_core].turn == NULL) {
        host_core = host_core_with_turn();
    }
    if (host_core == CORES) {
        fprintf(stderr, "invariant violated: no core runs a thread, and the kernel went on\n");
        host_violated();
    }
#else
    (void)c;
#endif
}

/*
 * The driver's passing protocol from rvuos/replay.h, run from the kernel's state:
 * every thread the round visits ticks until the actor has the processor
 * or the processor comes back to a thread that ticked, with the record untaken.
 * That is the thread the round started from,
 * or one whose tick failed, its table no longer holding the debug capability,
 * and gave the processor to nobody.
 * A thread the processor comes to that could not run faults, and the round goes on from the next.
 * With more than one core the round goes on the core of host_pick_core.
 */
void host_event(const struct replay_record *c)
{
    host_pick_core(c);
    struct thread *start = core_self()->current;
    while (replay_passes(c, host_actor())) {
        struct thread *ticked = core_self()->current;
        host_syscall(&replay_tick);
        host_run_on();
        if (core_self()->current == start || core_self()->current == ticked) {
            break;
        }
    }
    /* The thread takes the record; host_run_on makes it, and it drains the log after. */
    unsigned actor = host_actor();
    if (replay_accesses(c)) {
        host_at[actor] = HOST_AT_ACCESS;
        host_access_at[actor] = *c;
    } else {
        host_load(c);
        host_at[actor] = HOST_AT_CALL;
    }
    host_run_on();
    host_settle_cores();
}

void host_end(void)
{
    /*
     * Each thread that runs faults, at its breakpoint or before it, on whichever core runs it,
     * and the kernel halts after the last.
     */
    for (unsigned i = 0; i < REPLAY_THREADS; i++) {
#if CORES > 1
        host_core = host_core_with_turn();
        if (host_core == CORES) {
            fprintf(stderr, "invariant violated: no core runs a thread, and the kernel went on\n");
            host_violated();
        }
#endif
        host_run_on();
        host_fault(CAUSE_BREAKPOINT);
    }
    fprintf(stderr, "invariant violated: a thread ran again after every driver thread faulted\n");
    host_violated();
}
