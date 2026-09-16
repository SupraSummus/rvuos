/*
 * Replacements for the hardware-facing parts of the kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

_Static_assert(HOST_RAM_BASE == RAM_BASE && HOST_RAM_SIZE == RAM_SIZE,
               "host RAM must match the kernel's layout");

uint8_t *host_ram;
jmp_buf host_halt_jmp;
int host_halt_code;
bool host_verbose;
unsigned pmp_entry_count;

/* The PMP CSRs as last written. */
static uint32_t pmp_addr[PMP_MAX_ENTRIES];
static uint8_t pmp_cfg[PMP_MAX_ENTRIES];

void kputc(char c)
{
    if (host_verbose) {
        putchar(c);
    }
}

void kputs(const char *s)
{
    if (host_verbose) {
        fputs(s, stdout);
    }
}

void kput_hex(uint32_t v)
{
    if (host_verbose) {
        printf("0x%08x", v);
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

unsigned pmp_init(void)
{
    memset(pmp_addr, 0, sizeof(pmp_addr));
    memset(pmp_cfg, 0, sizeof(pmp_cfg));
    return PMP_MAX_ENTRIES;
}

void pmp_set(unsigned idx, uint32_t addr, uint8_t cfg)
{
    if (idx >= PMP_MAX_ENTRIES) {
        kpanic("pmp_set index out of range");
    }
    /* The CSR keeps addr >> 2, so the low two bits are lost, as on hardware. */
    pmp_addr[idx] = addr & ~3u;
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
    debug_trace = false;
    pmp_entry_count = pmp_init();

    struct thread *root = boot_create_root(
        HOST_BOOT_POOL_BASE, HOST_BOOT_POOL_SIZE,
        FREE_BASE, INPUT_BASE - FREE_BASE);
    current = root;
    process_activate(thread_process(root));

    /* What the replay driver does before it turns tracing on. */
    if (process_install(thread_process(root), INPUT_SLOT, INPUT_BASE, INPUT_SIZE, RIGHT_R) != KERR_OK) {
        abort();
    }
    return root;
}

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

static bool mapped_with(const struct process *proc, uint32_t base, uint32_t size, uint8_t rights)
{
    for (unsigned i = 0; i < PROCESS_REGION_SLOTS; i++) {
        const struct region_slot *s = &proc->slots[i];
        if (s->rights && s->base <= base && size <= s->size - (base - s->base) &&
            (s->rights & rights) == rights) {
            return true;
        }
    }
    return false;
}

bool host_root_alive(void)
{
    const struct process *proc = thread_process(current);
    return mapped_with(proc, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X) &&
           mapped_with(proc, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W);
}
