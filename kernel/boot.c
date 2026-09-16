/*
 * Construction of the root task.
 * See DESIGN.md, "Boot".
 */

#include "kernel.h"
#include "object.h"

#define ROOT_CAPTABLE_SLOTS 64

struct granted_range boot_granted[GRANTED_RANGES];
struct pool *boot_pool;

struct thread *boot_create_root(paddr_t boot_pool_base, uint32_t boot_pool_size,
                                paddr_t free_base, uint32_t free_size)
{
    memset(p2v(boot_pool_base), 0, boot_pool_size);
    struct pool *pool = pool_create(boot_pool_base, boot_pool_size, RIGHT_ALL);
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
    proc->ctable = v2p(table);
    thread->proc = v2p(proc);
    /* The root thread is the one the kernel drops into; it never waits. */
    thread->state = THREAD_READY;
    thread->flags = 0;
    thread->frame.regs[REG_SP] = USER_DATA_BASE + USER_DATA_SIZE;
    thread->frame.mepc = USER_CODE_BASE;

    if (process_install(proc, 0, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X) != KERR_OK ||
        process_install(proc, 1, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W) != KERR_OK) {
        kpanic("cannot install the root task's regions");
    }

    struct cap debug = { .type = CAP_DEBUG, .rights = RIGHT_ALL };
    table->slots[BOOT_CAP_CAPTABLE] = cap_to_object(&table->hdr, RIGHT_ALL);
    table->slots[BOOT_CAP_PROCESS] = cap_to_object(&proc->hdr, RIGHT_ALL);
    table->slots[BOOT_CAP_THREAD] = cap_to_object(&thread->hdr, RIGHT_ALL);
    table->slots[BOOT_CAP_POOL] = cap_to_object(&pool->hdr, RIGHT_ALL);
    table->slots[BOOT_CAP_DEBUG] = debug;
    table->slots[BOOT_CAP_CODE] = cap_to_region(USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X);
    table->slots[BOOT_CAP_DATA] = cap_to_region(USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W);
    table->slots[BOOT_CAP_FREE_RAM] = cap_to_region(free_base, free_size, RIGHT_ALL);
    table->slots[BOOT_CAP_INPUT] = cap_to_region(INPUT_BASE, INPUT_SIZE, RIGHT_R);

    boot_granted[0] = (struct granted_range){ USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X };
    boot_granted[1] = (struct granted_range){ USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W };
    boot_granted[2] = (struct granted_range){ free_base, free_size, RIGHT_ALL };
    boot_granted[3] = (struct granted_range){ INPUT_BASE, INPUT_SIZE, RIGHT_R };

    return thread;
}
