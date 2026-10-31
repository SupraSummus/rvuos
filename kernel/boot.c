/*
 * Construction of the root task.
 * See DESIGN.md, "Boot".
 */

#include "dma.h"
#include "irq.h"
#include "kernel.h"
#include "klog.h"
#include "object.h"
#include "route.h"
#include "sched.h"

struct granted_range boot_granted[GRANTED_RANGES];

/*
 * Whether block g, the boot's grant at, lies in RAM and apart from the kernel, its log, the root task's memory,
 * the free RAM and every grant before it, as a block of RAM no other Untyped covers must.
 * RAM is RAM_BASE's, or the RAM a board has outside it, EXT_RAM_BASE's, such as the ESP32-C5's PSRAM.
 */
static bool ram_block_apart(const struct granted_range *g, unsigned at)
{
    bool in_ram = g->base - RAM_BASE < RAM_SIZE && g->size <= RAM_SIZE - (g->base - RAM_BASE);
#ifdef EXT_RAM_BASE
    in_ram = in_ram || (g->base - EXT_RAM_BASE < EXT_RAM_SIZE && g->size <= EXT_RAM_SIZE - (g->base - EXT_RAM_BASE));
#endif
    if (g->size == 0 || !in_ram ||
        ranges_overlap(g->base, g->size, RAM_BASE, KLOG_BASE + KLOG_REGION_SIZE - RAM_BASE)) {
        return false;
    }
    for (unsigned i = 0; i < at; i++) {
        if (ranges_overlap(g->base, g->size, boot_granted[i].base, boot_granted[i].size)) {
            return false;
        }
    }
    return true;
}

/*
 * The root task's code, data and input and the boot pool's block, BOOT_CAP_POOL_RAM,
 * are made of its memory, BOOT_CAP_ROOT_RAM, and hang right below it,
 * as they would once a loader had split that memory down to them
 * and deleted the halves between; the boot leaves nothing more.
 * Code and input a board places in flash are no RAM's, and are roots of their own, as a device's frame is.
 * The boot pool hangs below its block as a retype would have put it,
 * so a revoke there destroys the root task and leaves its code where it is.
 */
struct thread *boot_create_root(void)
{
    /* Its own node is a root until the Untyped it lies below is in the table. */
    struct pool *pool = pool_create(BOOT_POOL_BASE, BOOT_POOL_SIZE, NULL);

    struct captable *table = pool_alloc(pool, CAP_CAPTABLE, obj_type_size(CAP_CAPTABLE, ROOT_TABLE_SLOTS));
    uint32_t regions = ROOT_REGION_SLOTS < pmp_entry_count ? ROOT_REGION_SLOTS : pmp_entry_count;
    struct process *proc = pool_alloc(pool, CAP_PROCESS, obj_type_size(CAP_PROCESS, regions));
    struct thread *thread = pool_alloc(pool, CAP_THREAD, obj_type_size(CAP_THREAD, CORES));
    if (table == NULL || proc == NULL || thread == NULL) {
        kpanic("boot pool too small for the root task");
    }

    table->nslots = ROOT_TABLE_SLOTS;
    proc->regions = (uint8_t)regions;
    /* A place on every core, so that the root thread may hold the time of each. */
    thread_make_places(thread, CORES);
    thread->flags = 0;
    frame_start(&thread->frame, ENTRY_PC(USER_CODE_BASE), USER_DATA_BASE + USER_DATA_SIZE, 0);

    boot_granted[0] = (struct granted_range){ ROOT_RAM_BASE, ROOT_RAM_SIZE, RIGHT_ALL };
    boot_granted[1] = (struct granted_range){ FREE_RAM_BASE, FREE_RAM_SIZE, RIGHT_ALL };
    /* A device: read and write, never execute, and never a pool; see ram_contains. */
    boot_granted[2] = (struct granted_range){ UART_BASE, UART_SIZE, RIGHT_R | RIGHT_W };
    /* The kernel's log; the reader writes its mark into the header. Never a pool: a frame no Untyped covers. */
    boot_granted[3] = (struct granted_range){ KLOG_BASE, KLOG_REGION_SIZE, RIGHT_R | RIGHT_W };
    /* The root task's code and input, granted wherever they lie, in its memory or in flash. */
    boot_granted[GRANT_CODE] = (struct granted_range){ USER_CODE_BASE, USER_CODE_SIZE, RIGHT_R | RIGHT_X };
    boot_granted[GRANT_INPUT] = (struct granted_range){ INPUT_BASE, INPUT_SIZE, RIGHT_R };
    /* The board's devices, each a frame from BOOT_CAP_DEVICES up; the array is never empty. */
    static const struct granted_range devices[BOOT_DEVICES + 1] = { DEVICE_RANGE_LIST };
    for (unsigned i = GRANT_DEVICES; i < GRANT_RAM_BLOCKS; i++) {
        boot_granted[i] = devices[i - GRANT_DEVICES];
    }
    /* The board's further blocks of RAM, each an Untyped from BOOT_CAP_RAM_BLOCKS up; this array is never empty either. */
    static const struct granted_range ram_blocks[BOOT_RAM_BLOCKS + 1] = { RAM_BLOCK_LIST };
    for (unsigned i = GRANT_RAM_BLOCKS; i < GRANTED_RANGES; i++) {
        boot_granted[i] = ram_blocks[i - GRANT_RAM_BLOCKS];
        if (!ram_block_apart(&boot_granted[i], i)) {
            kpanic("a block of RAM the board lists is not RAM of its own");
        }
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
    /* The root task's data is a block too, as every frame is, so that one word names it; its code and input are grants. */
    if (!napot_block(USER_DATA_BASE, USER_DATA_SIZE)) {
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
    proc->tables[0] = cap_to_object(&table->hdr, RIGHT_ALL);
    cap_attach(&pool->node, &proc->tables[0]);
    thread->proc = (struct cap){ .type = CAP_PROCESS, .rights = RIGHT_ALL, .a = v2p(proc) };
    thread->proc.b = held_word(v2p(thread), 0);
    cap_attach(&pool->node, &thread->proc);

    /*
     * The boot capabilities are the roots of the derivation tree,
     * but for the frames and the block made of the root task's memory, which hang below it,
     * and those to the boot pool and its objects, which hang below the pool's node;
     * revoking below BOOT_CAP_POOL takes what was allocated through it and not these.
     */
    struct cap boot[BOOT_CAP_DEVICES] = {
        [BOOT_CAP_CAPTABLE] = cap_to_object(&table->hdr, RIGHT_ALL),
        [BOOT_CAP_PROCESS] = cap_to_object(&proc->hdr, RIGHT_ALL),
        [BOOT_CAP_THREAD] = cap_to_object(&thread->hdr, RIGHT_ALL),
        [BOOT_CAP_POOL] = cap_to_object(&pool->hdr, RIGHT_ALL),
        [BOOT_CAP_PRINT] = { .type = CAP_PRINT, .rights = RIGHT_ALL },
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
        [BOOT_CAP_HALT] = { .type = CAP_HALT, .rights = RIGHT_ALL },
        [BOOT_CAP_TEST] = { .type = CAP_TEST, .rights = RIGHT_ALL },
        /* Every channel the board's router lends, and nothing on a board without one. */
        [BOOT_CAP_ROUTES] = ROUTE_CHANNELS != 0 ? cap_to_routes(ROUTE_FIRST, ROUTE_CHANNELS, RIGHT_W) : (struct cap){ 0 },
        /* Every channel the board's DMA controller lends, and nothing on a board without one. */
        [BOOT_CAP_DMA] = DMA_CHANNELS != 0 ? cap_to_dma(DMA_FIRST, DMA_CHANNELS, RIGHT_W) : (struct cap){ 0 },
    };
    for (unsigned i = BOOT_CAP_NULL + 1; i < BOOT_CAP_DEVICES; i++) {
        if (i == BOOT_CAP_ROOT_RAM || i == BOOT_CAP_POOL_RAM || boot[i].type == CAP_NONE) {
            continue;
        }
        bool pooled = i == BOOT_CAP_CAPTABLE || i == BOOT_CAP_PROCESS || i == BOOT_CAP_THREAD ||
                      i == BOOT_CAP_POOL;
        bool own = (i == BOOT_CAP_CODE && IN_ROOT_RAM(USER_CODE_BASE, USER_CODE_SIZE)) || i == BOOT_CAP_DATA ||
                   (i == BOOT_CAP_INPUT && IN_ROOT_RAM(INPUT_BASE, INPUT_SIZE));
        if (cap_store(table, i, &boot[i], pooled ? &pool->node : own ? root_ram : NULL) != KERR_OK) {
            kpanic("cannot fill the root task's table");
        }
    }
    /*
     * The board's devices and further blocks of RAM, each made as it is stored, so that the stack does not grow
     * with their count; they follow one another in the grants as in the slots.
     */
    _Static_assert(GRANT_RAM_BLOCKS - GRANT_DEVICES == BOOT_CAP_RAM_BLOCKS - BOOT_CAP_DEVICES,
                   "the grants of the devices and the blocks lie as their slots do");
    for (unsigned i = BOOT_CAP_DEVICES; i < BOOT_CAP_COUNT; i++) {
        const struct granted_range *g = &boot_granted[GRANT_DEVICES + i - BOOT_CAP_DEVICES];
        struct cap made = i < BOOT_CAP_RAM_BLOCKS ? cap_to_frame(g->base, g->size, g->rights)
                                                  : cap_to_untyped(g->base, g->size, g->rights);
        if (cap_store(table, i, &made, NULL) != KERR_OK) {
            kpanic("cannot fill the root task's table");
        }
    }

    /*
     * The root thread earns the whole of the first core in its first place, bound through the capability to every unit,
     * which lets it run on spare time too, and starts with its virtual time at the count.
     * It is the one the kernel drops into, so it is ready and on no queue: bound and filled while stopped.
     */
    sched_bind(thread_place(thread, 0), 0, TIME_UNITS, RIGHT_W | RIGHT_X, &table->slots[BOOT_CAP_TIME], 0);
    sched_vtimes_reset();
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
