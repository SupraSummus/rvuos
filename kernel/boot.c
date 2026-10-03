/*
 * Construction of the root task.
 * See DESIGN.md, "Boot".
 */

#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"

struct granted_range boot_granted[GRANTED_RANGES];

/*
 * The root task's code, data and input and the boot pool's block, BOOT_CAP_POOL_RAM,
 * are made of its memory, BOOT_CAP_ROOT_RAM, and hang right below it,
 * as they would once a loader had split that memory down to them
 * and deleted the halves between; the boot leaves nothing more.
 * The boot pool hangs below its block as a retype would have put it,
 * so a revoke there destroys the root task and leaves its code where it is.
 */
struct thread *boot_create_root(void)
{
    /* Its own node is a root until the Untyped it lies below is in the table. */
    struct pool *pool = pool_create(BOOT_POOL_BASE, BOOT_POOL_SIZE, NULL);

    struct captable *table = pool_alloc(
        pool, CAP_CAPTABLE,
        sizeof(*table) + ROOT_TABLE_SLOTS * sizeof(struct cap));
    struct process *proc = pool_alloc(pool, CAP_PROCESS, sizeof(*proc));
    struct thread *thread = pool_alloc(pool, CAP_THREAD, sizeof(*thread));
    if (table == NULL || proc == NULL || thread == NULL) {
        kpanic("boot pool too small for the root task");
    }

    table->nslots = ROOT_TABLE_SLOTS;
    thread->flags = 0;
    frame_start(&thread->frame, ENTRY_PC(USER_CODE_BASE), USER_DATA_BASE + USER_DATA_SIZE, 0);

    boot_granted[0] = (struct granted_range){ ROOT_RAM_BASE, ROOT_RAM_SIZE, RIGHT_ALL };
    boot_granted[1] = (struct granted_range){ FREE_RAM_BASE, FREE_RAM_SIZE, RIGHT_ALL };
    /* A device: read and write, never execute, and never a pool; see ram_contains. */
    boot_granted[2] = (struct granted_range){ UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W };
    /* The kernel's log; the reader writes its mark into the header. Never a pool: a frame no Untyped covers. */
    boot_granted[3] = (struct granted_range){ KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W };
    /* The counter, read only, in the smallest block that holds its two words; see OP_CLOCK_FRAME. */
    uint32_t counter_size = region_min_size();
    boot_granted[GRANT_COUNTER] =
        (struct granted_range){ COUNTER_ADDR & ~(counter_size - 1), counter_size, RIGHT_R };
    /* The board's devices, each a frame from BOOT_CAP_DEVICES up; the array is never empty. */
    static const struct granted_range devices[BOOT_DEVICES + 1] = { DEVICE_RANGE_LIST };
    for (unsigned i = GRANT_DEVICES; i < GRANTED_RANGES; i++) {
        boot_granted[i] = devices[i - GRANT_DEVICES];
    }

    /* The grants become frames and the Untypeds as they are, and every one of those is a NAPOT block. */
    for (unsigned i = 0; i < GRANTED_RANGES; i++) {
        if (!napot_block(boot_granted[i].base, boot_granted[i].size)) {
            kpanic("boot layout is not made of NAPOT blocks");
        }
    }

    /*
     * The root task's memory makes nothing while what it holds lies below it;
     * nothing is below the pool's node yet, so attaching it loses nothing.
     */
    if (!napot_block(BOOT_POOL_BASE, BOOT_POOL_SIZE)) {
        kpanic("boot layout is not made of NAPOT blocks");
    }
    struct cap ram = cap_to_untyped(ROOT_RAM_BASE, ROOT_RAM_SIZE, RIGHT_ALL);
    struct cap pool_ram = cap_to_untyped(BOOT_POOL_BASE, BOOT_POOL_SIZE, RIGHT_ALL);
    struct cap *root_ram = &table->slots[BOOT_CAP_ROOT_RAM];
    if (cap_store(table, BOOT_CAP_ROOT_RAM, &ram, NULL) != KERR_OK ||
        cap_store(table, BOOT_CAP_POOL_RAM, &pool_ram, root_ram) != KERR_OK) {
        kpanic("cannot fill the root task's table");
    }
    cap_attach(&table->slots[BOOT_CAP_POOL_RAM], &pool->node);

    /*
     * Below the pool's node, as every capability to an object of the pool is,
     * so that only a destroy of the boot pool takes the root task's table or process.
     */
    proc->table = cap_to_object(&table->hdr, RIGHT_ALL);
    cap_attach(&pool->node, &proc->table);
    thread->proc = (struct cap){ .type = CAP_HOSTED, .rights = RIGHT_ALL, .a = v2p(proc) };
    cap_attach(&pool->node, &thread->proc);

    /*
     * The boot capabilities are the roots of the derivation tree,
     * but for the frames and the block made of the root task's memory, which hang below it,
     * and those to the boot pool and its objects, which hang below the pool's node;
     * revoking below BOOT_CAP_POOL takes what was allocated through it and not these.
     */
    struct cap boot[BOOT_CAP_COUNT] = {
        [BOOT_CAP_CAPTABLE] = cap_to_object(&table->hdr, RIGHT_ALL),
        [BOOT_CAP_PROCESS] = cap_to_object(&proc->hdr, RIGHT_ALL),
        [BOOT_CAP_THREAD] = cap_to_object(&thread->hdr, RIGHT_ALL),
        [BOOT_CAP_POOL] = cap_to_object(&pool->hdr, RIGHT_ALL),
        [BOOT_CAP_DEBUG] = { .type = CAP_DEBUG, .rights = RIGHT_ALL },
        [BOOT_CAP_CODE] = cap_to_frame(USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X),
        [BOOT_CAP_DATA] = cap_to_frame(USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W),
        [BOOT_CAP_FREE_RAM] = cap_to_untyped(FREE_RAM_BASE, FREE_RAM_SIZE, RIGHT_ALL),
        [BOOT_CAP_INPUT] = cap_to_frame(INPUT_BASE, INPUT_SIZE, RIGHT_R),
        /* Line 0 is the log's, which no controller has; the controller's lines start at 1. */
        [BOOT_CAP_IRQ_LINES] = cap_to_lines(LOG_IRQ_LINE, IRQ_LINES, RIGHT_W),
        [BOOT_CAP_UART] = cap_to_frame(UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W),
        [BOOT_CAP_LOG] = cap_to_frame(KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W),
        /* The timer lines follow the controller's, and are granted apart so that no board's count shows. */
        [BOOT_CAP_TIMER_LINES] = cap_to_lines(IRQ_LINES, TIMER_LINES, RIGHT_W),
        [BOOT_CAP_CLOCK] = { .type = CAP_CLOCK, .rights = RIGHT_ALL },
        [BOOT_CAP_TIME] = cap_to_time(0, MACHINE_UNITS, RIGHT_W | RIGHT_X),
    };
    for (unsigned i = BOOT_CAP_DEVICES; i < BOOT_CAP_COUNT; i++) {
        const struct granted_range *g = &boot_granted[GRANT_DEVICES + i - BOOT_CAP_DEVICES];
        boot[i] = cap_to_frame(g->base, g->size, g->rights);
    }
    for (unsigned i = BOOT_CAP_NULL + 1; i < BOOT_CAP_COUNT; i++) {
        if (i == BOOT_CAP_ROOT_RAM || i == BOOT_CAP_POOL_RAM) {
            continue;
        }
        bool pooled = i == BOOT_CAP_CAPTABLE || i == BOOT_CAP_PROCESS || i == BOOT_CAP_THREAD ||
                      i == BOOT_CAP_POOL;
        bool own = i == BOOT_CAP_CODE || i == BOOT_CAP_DATA || i == BOOT_CAP_INPUT;
        if (own && !napot_block(boot[i].a, boot[i].b)) {
            kpanic("boot layout is not made of NAPOT blocks");
        }
        if (cap_store(table, i, &boot[i], pooled ? &pool->node : own ? root_ram : NULL) != KERR_OK) {
            kpanic("cannot fill the root task's table");
        }
    }

    /*
     * The root thread earns the whole of the first core, bound through the capability to every unit of it,
     * which lets it run on spare time too, and starts with a full account.
     * It is the one the kernel drops into, so it is ready and on no queue: bound and filled while stopped.
     */
    sched_bind(thread, 0, TIME_UNITS, RIGHT_W | RIGHT_X, &table->slots[BOOT_CAP_TIME]);
    sched_accounts_fill();
    thread->state = THREAD_READY;

    /* The root task's own mappings derive from its frames, as every mapping does. */
    if (process_install(proc, 0, USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X,
                        &table->slots[BOOT_CAP_CODE]) != KERR_OK ||
        process_install(proc, 1, USER_DATA_BASE, USER_DATA_SIZE, RIGHT_R | RIGHT_W,
                        &table->slots[BOOT_CAP_DATA]) != KERR_OK) {
        kpanic("cannot install the root task's regions");
    }

    return thread;
}
