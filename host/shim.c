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
jmp_buf host_halt_jmp;
int host_halt_code;
bool host_verbose;
bool host_isolated;
unsigned pmp_entry_count;
uint32_t pmp_grain;

/* The simulated core's grain. QEMU has 4; the Makefile also builds with 32. */
#ifndef PMP_GRAIN
#define PMP_GRAIN 4
#endif
_Static_assert(PMP_GRAIN >= 4 && (PMP_GRAIN & (PMP_GRAIN - 1)) == 0,
               "the PMP grain is a power of two of at least four bytes");

/* The PMP CSRs as last written. */
static uint32_t pmp_addr[PMP_MAX_ENTRIES];
static uint8_t pmp_cfg[PMP_MAX_ENTRIES];

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

void selfcheck_fail(void)
{
    if (!host_verbose) {
        fprintf(stderr, "invariant violated; rerun with --verbose for the report\n");
    }
    host_violated();
}

bool intr_wait(uint32_t wake, uint32_t *ticks)
{
    (void)wake;
    (void)ticks;
    /* Reached only untraced with an Irq armed, which the harness never sets up. */
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

void pmp_init(void)
{
    memset(pmp_addr, 0, sizeof(pmp_addr));
    memset(pmp_cfg, 0, sizeof(pmp_cfg));
    pmp_entry_count = PMP_MAX_ENTRIES;
    pmp_grain = PMP_GRAIN;
}

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    if (idx >= PMP_MAX_ENTRIES) {
        kpanic("pmp_set index out of range");
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
    current = NULL;
    memset(unit_thread, 0, sizeof(unit_thread));
    run_queue = 0;
    spare_queue = 0;
    spent_queue = 0;
    turn = NULL;
    turn_from = 0;
    armed_sources = 0;
    nearest_deadline = NEAREST_NONE;
    nearest_release = NEAREST_NONE;
    wake_stale = false;
    turn_due = false;
    sched_ticks = 0;
    debug_trace = false;
    pmp_init();
    irq_init();
    klog_init();

    /* The layout is the target's, the log and the boot pool included, so the PMP images of the two builds agree. */
    struct thread *root = boot_create_root();
    sched_start(root);
    process_activate(thread_process(root));

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

/*
 * A trap of the running thread, a call or a fault, between the checks every trap gets,
 * and what the trap does on its way back, which the self-check of the next call holds.
 */
static void host_trap(bool call)
{
    history_begin(call);
#ifdef RVUOS_WORK
    work_begin();
#endif
    if (call) {
        syscall_dispatch(current);
    } else {
        fault_dispatch(current);
    }
    sched_wake();
    /* The report was made as the kernel reached poisoned RAM; see host_asan_report. */
    if (poison_reached) {
        host_violated();
    }
#ifdef RVUOS_WORK
    work_end();
#endif
    history_end();
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
    const struct thread *caller = current;
    struct trap_frame *f = &current->frame;
    f->regs[REG_A7] = c->op;
    f->regs[REG_A0] = c->slot;
    f->regs[REG_A1] = c->a1;
    f->regs[REG_A2] = c->a2;
    f->regs[REG_A3] = c->a3;
    f->regs[REG_A4] = 0;
    f->regs[REG_A5] = 0;
    f->regs[REG_A6] = 0;
    f->mcause = 8;
    /*
     * A preempted call leaves the thread at its ecall, which it executes again
     * once the interrupt is taken; on the host that takes nothing, and no other thread runs.
     */
    uint32_t mepc;
    do {
        mepc = f->mepc;
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
        if (f->mepc == mepc && !took) {
            fprintf(stderr, "invariant violated: a call was preempted before it took anything away\n");
            host_violated();
        }
        /*
         * The thread makes a stopped call again, so it runs on and can still name what it invoked:
         * the slot resolved as the call began, and nothing refills a slot within a call.
         */
        if (f->mepc == mepc && (current != caller || thread_table(caller) == NULL ||
                                thread_table(caller)->slots[c->slot].type == CAP_NONE)) {
            fprintf(stderr, "invariant violated: a call stopped where its caller cannot make it again\n");
            host_violated();
        }
    } while (f->mepc == mepc);
    return f->regs[REG_A0];
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

/* The actor number of the running thread; 0 when it is not a driver thread, which never runs traced. */
static unsigned host_actor(void)
{
    for (unsigned i = 1; i <= REPLAY_THREADS; i++) {
        /* Compared, never followed: a destroyed thread is simply never current again. */
        if (host_threads[i] == current) {
            return i;
        }
    }
    return 0;
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

/* Whether the running thread's process maps the driver's code and data, and to drain the log, the UART and the log. */
static bool driver_mapped(bool drain)
{
    const struct process *proc = thread_process(current);
    return proc != NULL && mapped_with(proc, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X) &&
           mapped_with(proc, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W) &&
           (!drain || (mapped_with(proc, UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W) &&
                       mapped_with(proc, KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W)));
}

bool host_driver_alive(void)
{
    return driver_mapped(host_drains[host_actor()]);
}

/*
 * The running thread faults as the trap would have it.
 * mepc stays the host's own count, since the host knows no address of the driver's code;
 * the report prints it, and nothing compares it.
 */
void host_fault(uint32_t cause)
{
    struct trap_frame *f = &current->frame;
    f->mcause = cause;
    f->mtval = cause == CAUSE_BREAKPOINT ? 0 : f->mepc;
    host_trap(false);
}

/*
 * The running driver thread goes on as the driver's code does, up to its next call:
 * one that cannot faults, see host_driver_alive,
 * and the kernel hands the processor on, maybe to another such thread;
 * one that drains the log leaves the mark the drain leaves in its header.
 * A thread faults once, since nothing but a record starts it again,
 * so after a fault each the kernel has found a thread that runs, or halted.
 */
static void host_run_on(void)
{
    for (unsigned i = 0; i < REPLAY_THREADS && !host_driver_alive(); i++) {
        host_fault(CAUSE_INSN_ACCESS);
    }
    if (!host_driver_alive()) {
        fprintf(stderr, "invariant violated: a thread ran again after every driver thread faulted\n");
        host_violated();
    }
    if (host_drains[host_actor()]) {
        volatile struct rvuos_log *log = p2v(KLOG_BASE);
        log->taken = log->head;
        host_drains[host_actor()] = false;
    }
}

/*
 * The driver's passing protocol from rvuos/replay.h, run from the kernel's state:
 * every thread the round visits ticks until the actor has the processor
 * or the processor comes back to a thread that ticked, with the record untaken.
 * That is the thread the round started from,
 * or one whose tick failed, its table no longer holding the debug capability,
 * and gave the processor to nobody.
 * A thread the processor comes to that could not run faults, and the round goes on from the next.
 */
void host_event(const struct replay_record *c)
{
    struct thread *start = current;
    while (replay_passes(c, host_actor())) {
        struct thread *ticked = current;
        host_syscall(&replay_tick);
        host_run_on();
        if (current == start || current == ticked) {
            break;
        }
    }
    /* The driver drains the log to the UART after each record it performs; see host_run_on. */
    unsigned actor = host_actor();
    host_syscall(c);
    host_drains[actor] = true;
    host_run_on();
}

void host_end(void)
{
    /* Each thread that runs faults, at its breakpoint or before it, and the kernel halts after the last. */
    for (unsigned i = 0; i < REPLAY_THREADS; i++) {
        host_run_on();
        host_fault(CAUSE_BREAKPOINT);
    }
    fprintf(stderr, "invariant violated: a thread ran again after every driver thread faulted\n");
    host_violated();
}
