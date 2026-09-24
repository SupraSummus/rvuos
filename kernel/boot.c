/*
 * Construction of the root task.
 * See DESIGN.md, "Boot".
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"

#define ROOT_CAPTABLE_SLOTS 64

struct granted_range boot_granted[GRANTED_RANGES];
struct pool *boot_pool;

struct thread *boot_create_root(paddr_t boot_pool_base, uint32_t boot_pool_size,
                                paddr_t free_base, uint32_t free_size)
{
    memset(p2v(boot_pool_base), 0, boot_pool_size);
    struct pool *pool = pool_create(boot_pool_base, boot_pool_size, RIGHT_ALL, NULL);
    boot_pool = pool;

    struct captable *table = pool_alloc(
        pool, CAP_CAPTABLE,
        sizeof(*table) + ROOT_CAPTABLE_SLOTS * sizeof(struct cap));
    struct process *proc = pool_alloc(pool, CAP_PROCESS, sizeof(*proc));
    struct thread *thread = pool_alloc(pool, CAP_THREAD, sizeof(*thread));
    if (table == NULL || proc == NULL || thread == NULL) {
        kpanic("boot pool too small for the root task");
    }

    table->nslots = ROOT_CAPTABLE_SLOTS;
    /* A boot capability, so a root of the tree like the others. */
    proc->table = cap_to_object(&table->hdr, RIGHT_ALL);
    cap_attach(NULL, &proc->table);
    thread->proc = v2p(proc);
    /* The root thread is the one the kernel drops into; it never waits. */
    thread->state = THREAD_READY;
    thread->flags = 0;
    thread->frame.regs[REG_SP] = USER_DATA_BASE + USER_DATA_SIZE;
    thread->frame.mepc = USER_CODE_BASE;

    boot_granted[0] = (struct granted_range){ USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X };
    boot_granted[1] = (struct granted_range){ USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W };
    boot_granted[2] = (struct granted_range){ free_base, free_size, RIGHT_ALL };
    boot_granted[3] = (struct granted_range){ INPUT_BASE, INPUT_SIZE, RIGHT_R };
    /* A device: read and write, never execute, and never a pool; see ram_contains. */
    boot_granted[4] = (struct granted_range){ UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W };
    /* The kernel's log; the reader writes its mark into the header. Never a pool; see OP_REGION_TO_POOL. */
    boot_granted[5] = (struct granted_range){ KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W };

    /* The grants become region capabilities as they are, and every one of those is a NAPOT block. */
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        if (!napot_block(boot_granted[i].base, boot_granted[i].size)) {
            kpanic("boot layout is not made of NAPOT blocks");
        }
    }

    /* The boot capabilities are the roots of the derivation tree. */
    struct cap boot[BOOT_CAP_COUNT] = {
        [BOOT_CAP_CAPTABLE] = cap_to_object(&table->hdr, RIGHT_ALL),
        [BOOT_CAP_PROCESS] = cap_to_object(&proc->hdr, RIGHT_ALL),
        [BOOT_CAP_THREAD] = cap_to_object(&thread->hdr, RIGHT_ALL),
        [BOOT_CAP_POOL] = cap_to_object(&pool->hdr, RIGHT_ALL),
        [BOOT_CAP_DEBUG] = { .type = CAP_DEBUG, .rights = RIGHT_ALL },
        [BOOT_CAP_CODE] = cap_to_region(USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X),
        [BOOT_CAP_DATA] = cap_to_region(USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W),
        [BOOT_CAP_FREE_RAM] = cap_to_region(free_base, free_size, RIGHT_ALL),
        [BOOT_CAP_INPUT] = cap_to_region(INPUT_BASE, INPUT_SIZE, RIGHT_R),
        /* Line 0 is the log's, which no controller has; the controller's lines start at 1. */
        [BOOT_CAP_IRQ_LINES] = cap_to_lines(LOG_IRQ_LINE, IRQ_LINES, RIGHT_W),
        [BOOT_CAP_UART] = cap_to_region(UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W),
        [BOOT_CAP_LOG] = cap_to_region(KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W),
    };
    for (unsigned i = BOOT_CAP_NULL + 1; i < BOOT_CAP_COUNT; i++) {
        if (cap_store(table, i, &boot[i], NULL) != KERR_OK) {
            kpanic("cannot fill the root task's table");
        }
    }

    /* The root task's own mappings derive from its region capabilities, as every mapping does. */
    if (process_install(proc, 0, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X,
                        &table->slots[BOOT_CAP_CODE]) != KERR_OK ||
        process_install(proc, 1, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W,
                        &table->slots[BOOT_CAP_DATA]) != KERR_OK) {
        kpanic("cannot install the root task's regions");
    }

    return thread;
}
