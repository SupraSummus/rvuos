/*
 * Replacements for the hardware-facing parts of the kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "irq.h"
#include "klog.h"
#include "trap.h"
#include "uart.h"

_Static_assert(HOST_RAM_BASE == RAM_BASE && HOST_RAM_SIZE == RAM_SIZE,
               "host RAM must match the kernel's layout");
_Static_assert(REPLAY_THREAD_SP > USER_DATA_BASE &&
               REPLAY_THREAD_SP <= USER_DATA_BASE + USER_DATA_SIZE,
               "the replay driver's second stack must lie in its data region");

uint8_t *host_ram;
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
    /* Reached only untraced with a timer or an Irq armed, which the harness never sets up. */
    kpanic("the host build has no clock and no devices to wait for");
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
    /* Bits below the grain are lost, as on hardware; the self-check sees the difference. */
    pmp_addr[idx] = addr & ~(uint32_t)(PMP_GRAIN - 1);
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
    if (host_ram == NULL) {
        host_ram = calloc(HOST_RAM_SIZE, 1);
        if (host_ram == NULL) {
            abort();
        }
    }
    /*
     * The kernel zeroes pool memory itself when it takes it,
     * so RAM outside the boot pool may keep the previous run's contents,
     * exactly as on a warm reset.
     */
    pool_list = NULL;
    current = NULL;
    sched_ticks = 0;
    debug_trace = false;
    pmp_init();
    irq_init();
    klog_init();

    struct thread *root = boot_create_root(
        HOST_BOOT_POOL_BASE, HOST_BOOT_POOL_SIZE,
        FREE_BASE, INPUT_BASE - FREE_BASE);
    current = root;
    process_activate(thread_process(root));

    /*
     * What the replay driver does before it turns tracing on,
     * from the records both builds share; see rvuos/replay.h.
     */
    for (unsigned i = 0; i < REPLAY_PROLOGUE_COUNT; i++) {
        if (host_syscall(&replay_prologue[i]) != KERR_OK) {
            abort();
        }
    }
    for (unsigned i = 0; i < REPLAY_AFTER_INPUT_COUNT; i++) {
        if (host_syscall(&replay_after_input[i]) != KERR_OK) {
            abort();
        }
    }

    struct cap second;
    if (cap_lookup(thread_table(root), REPLAY_CAP_THREAD, &second) != KERR_OK ||
        second.type != CAP_THREAD) {
        abort();
    }
    host_threads[1] = root;
    host_threads[2] = (struct thread *)cap_object(&second);
    _Static_assert(REPLAY_THREADS == 2, "host_boot names the driver's threads by hand");
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
    syscall_dispatch(current);
    return f->regs[REG_A0];
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
 * or the round is back where it started.
 */
bool host_event(const struct replay_record *c)
{
    struct thread *start = current;
    while (replay_passes(c, host_actor())) {
        host_syscall(&replay_tick);
        if (!host_driver_alive()) {
            return false;
        }
        if (current == start) {
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
