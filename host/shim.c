/*
 * Replacements for the hardware-facing parts of the kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
jmp_buf host_halt_jmp;
int host_halt_code;
bool host_verbose;
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

void selfcheck_fail(void)
{
    /* The report went to stdout when verbose; make sure it is seen. */
    if (!host_verbose) {
        fprintf(stderr, "invariant violated; rerun with --verbose for the report\n");
    }
    fflush(stdout);
    abort();
}

unsigned intr_wait(void)
{
    /* Reached only untraced with an Irq armed, which the harness never sets up. */
    kpanic("the host build has no clock and no devices to wait for");
}

/* The host is QEMU's board, whose counter runs at a rate fixed in board.h. */
uint32_t timer_counter_hz(void)
{
    return COUNTER_HZ;
}

/* The worst case: every preemptible call stops after its first step. */
bool intr_pending(void)
{
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
    /*
     * RAM holds anything at power-up; a pattern stands for it,
     * so an object the kernel hands out without zeroing shows.
     */
    if (host_ram == NULL) {
        host_ram = malloc(HOST_RAM_SIZE);
        if (host_ram == NULL) {
            abort();
        }
        memset(host_ram, HOST_RAM_PATTERN, HOST_RAM_SIZE);
    }
    /*
     * The kernel clears each object as it hands it out,
     * so RAM may keep the previous run's contents, the boot pool's too,
     * exactly as on a warm reset.
     */
    pool_list = NULL;
    memset(line_irq, 0, LINES * sizeof(line_irq[0]));
    current = NULL;
    run_queue = 0;
    armed_sources = 0;
    sched_ticks = 0;
    debug_trace = false;
    pmp_init();
    irq_init();
    klog_init();

    struct thread *root = boot_create_root(
        HOST_BOOT_POOL_BASE, HOST_BOOT_POOL_SIZE,
        FREE_RAM_BASE, FREE_RAM_SIZE);
    current = root;
    process_activate(thread_process(root));

    /*
     * What the replay driver does before it turns tracing on,
     * from the records both builds share; see rvuos/replay.h.
     */
    for (unsigned i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        int err = (int)host_syscall(&replay_prologue[i]);
        if (err != KERR_OK) {
            fprintf(stderr, "invariant violated: a fresh kernel refused prologue record %u with %d\n", i, err);
            abort();
        }
    }
    for (unsigned i = 0; i < REPLAY_AFTER_INPUT_COUNT; i++) {
        int err = (int)host_syscall(&replay_after_input[i]);
        if (err != KERR_OK) {
            fprintf(stderr, "invariant violated: a fresh kernel refused prologue record %u with %d\n",
                    (unsigned)(REPLAY_PROLOGUE_COUNT + i), err);
            abort();
        }
    }

    static const uint16_t slots[] = { REPLAY_CAP_THREAD, REPLAY_CAP_THIRD };
    _Static_assert(sizeof(slots) / sizeof(slots[0]) == REPLAY_THREADS - 1, "a slot per thread but the root");
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
 * The record goes to whichever thread the kernel is running,
 * which is what happens on the target: the driver's threads
 * take records from one cursor.
 * A call that blocks leaves its thread's registers alone,
 * so the status returned here is that thread's previous one;
 * only the setup, which never blocks, looks at it.
 */
uint32_t host_syscall(const struct replay_record *c)
{
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
        history_begin();
#ifdef RVUOS_WORK
        work_begin();
#endif
        syscall_dispatch(current);
#ifdef RVUOS_WORK
        work_end();
#endif
        history_end();
        /* A step of a preemptible walk takes a node or a link away, and it stops only after one. */
        unsigned after[UNITS];
        host_units(after);
        if (f->mepc == mepc && after[UNIT_NODE] >= before[UNIT_NODE] &&
            after[UNIT_LINK] >= before[UNIT_LINK]) {
            fprintf(stderr, "invariant violated: a call was preempted before it took anything away\n");
            abort();
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
        if (o->type == CAP_CAPTABLE) {
            struct captable *t = (struct captable *)o;
            for (uint32_t i = 0; i < t->nslots; i++) {
                count_node(&t->slots[i], out);
            }
        } else if (o->type == CAP_PROCESS) {
            struct process *p = (struct process *)o;
            count_node(&p->table, out);
            for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
                count_node(&p->slots[i], out);
            }
        } else if (o->type == CAP_THREAD && ((struct thread *)o)->state == THREAD_WAITING) {
            out[UNIT_WAITER]++;
        }
    }
}

/* The actor number of the running thread; 0 when it is not a driver thread. */
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

/*
 * The driver's passing protocol from rvuos/replay.h, run from the kernel's state:
 * every thread the round visits ticks until the actor has the processor
 * or the processor comes back to a thread that ticked, with the record untaken.
 * That is the thread the round started from,
 * or one whose tick failed, its table no longer holding the debug capability,
 * and gave the processor to nobody.
 */
bool host_event(const struct replay_record *c)
{
    struct thread *start = current;
    while (replay_passes(c, host_actor())) {
        struct thread *ticked = current;
        host_syscall(&replay_tick);
        if (!host_driver_alive()) {
            return false;
        }
        if (current == start || current == ticked) {
            break;
        }
    }
    host_syscall(c);
    if (!host_driver_alive()) {
        return false;
    }
    /* The driver drains the log to the UART after each record, which leaves this mark. */
    volatile struct rvuos_log *log = p2v(KLOG_BASE);
    log->taken = log->head;
    return true;
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

bool host_driver_alive(void)
{
    const struct process *proc = thread_process(current);
    return mapped_with(proc, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X) &&
           mapped_with(proc, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W) &&
           mapped_with(proc, UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W) &&
           mapped_with(proc, KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W);
}
