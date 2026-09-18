/*
 * Root task for the current stage.
 * It walks the capability system once end to end on real hardware paths:
 * carve memory, make a pool, allocate from it,
 * build a second process out of nothing but capabilities,
 * exchange a word with it through shared memory and two notifications,
 * take turns with it through shared memory alone, which only the tick allows,
 * destroy a pool and watch both processes lose their capabilities to it,
 * lend the child memory it turns into a pool of its own
 * and take it back by destroying the child's pool,
 * then unmap a region and fault on it.
 * Negative paths are covered by the fuzz corpus and tests/differential.py.
 */

#include <stdint.h>

#include "rvuos.h"

/* Slots in the root task's own table. */
enum {
    SLOT_POOL_REGION = 10,
    SLOT_CHILD_DATA,
    SLOT_SHARED,
    SLOT_POOL,
    SLOT_CHILD_TABLE,
    SLOT_CHILD_PROCESS,
    SLOT_CHILD_THREAD,
    SLOT_UP,   /* the child signals, the root waits */
    SLOT_DOWN, /* the root signals, the child waits */
    SLOT_DOOMED_REGION, /* the memory of the pool that gets destroyed */
    SLOT_DOOMED_POOL,
    SLOT_DOOMED_NTFN,   /* a notification allocated from it */
    SLOT_STALE,         /* a second capability to that same notification */
    SLOT_NEW_POOL,      /* the pool built over the same memory afterwards */
    SLOT_NEW_NTFN,
    SLOT_LENT,          /* memory lent to the child, which pools it */
    SLOT_LENT_POOL,     /* what the root task makes of it once it is back */
};

/*
 * Slots in the child's table.
 * Which capability sits where is a convention between the two of them
 * and means nothing to the kernel.
 */
enum {
    CHILD_DEBUG = 1,
    CHILD_SHARED,
    CHILD_UP,
    CHILD_DOWN,
    CHILD_DOOMED, /* a notification whose pool the root task destroys */
    CHILD_LENT,   /* memory the root task lends; the child's pool replaces it */
    CHILD_OWN_NTFN, /* allocated from that pool */
    CHILD_BOOT_POOL, /* the pool above the child's own; the child may not destroy it */
    CHILD_TABLE_SLOTS = 10,
};

/* Region slots. The root task boots with code in 0 and data in 1. */
#define ROOT_SHARED_SLOT 2
#define CHILD_CODE_SLOT 0
#define CHILD_DATA_SLOT 1
#define CHILD_SHARED_SLOT 2

/* Offsets into the free RAM the root task was granted. */
#define POOL_OFFSET  0x0000u
#define DATA_OFFSET  0x1000u
#define SHARED_OFFSET 0x2000u
#define DOOMED_OFFSET 0x3000u
#define LENT_OFFSET  0x4000u
#define CHUNK 0x1000u

/* The protocol between the two processes. */
#define BIT_REQUEST 0x1u
#define BIT_REPLY   0x2u
#define BIT_DONE    0x4u
#define BIT_CHECK   0x8u /* look at the capability whose pool is gone */
#define BIT_CHECKED 0x10u
#define BIT_POOL    0x20u /* turn the lent memory into a pool */
#define BIT_POOLED  0x40u
#define MAGIC 0x5eaf00du

/* Whose turn it is in the handshake that uses no notification. */
#define TURN_CHILD 0x1u
#define TURN_ROOT  0x2u

static void puts(const char *s)
{
    rv_puts(BOOT_CAP_DEBUG, s);
}

static void expect(const char *what, uint32_t status)
{
    puts(what);
    puts(status == KERR_OK ? ": ok\n" : ": FAILED\n");
    if (status != KERR_OK) {
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

/*
 * The child runs this, in its own process, out of the same code region.
 * It has no data region in common with the root task,
 * so it may touch no global: everything it needs is on its stack
 * or behind a capability the root task put in its table.
 */
static void child_main(void)
{
    uint32_t base, size, bits;

    rv_puts(CHILD_DEBUG, "child: started\n");
    if (rv_region_info(CHILD_SHARED, &base, &size) != KERR_OK) {
        rv_puts(CHILD_DEBUG, "child: no shared region\n");
        rv_halt(CHILD_DEBUG, 3);
    }

    volatile uint32_t *shared = (volatile uint32_t *)base;
    shared[0] = MAGIC;
    rv_signal(CHILD_UP, BIT_REQUEST);

    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_REPLY && shared[1] == MAGIC + 1 ? "child: reply ok\n"
                                                        : "child: reply FAILED\n");

    /* The other half of the root task's preemption check. */
    while (shared[2] != TURN_CHILD) {
    }
    shared[2] = TURN_ROOT;

    rv_signal(CHILD_UP, BIT_DONE);

    /*
     * The root task destroys the pool the CHILD_DOOMED notification lives in
     * while this thread is stopped here.
     * Revocation has to reach this table, which belongs to another process
     * and which this thread never touches.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_CHECK && rv_signal(CHILD_DOOMED, BIT_REQUEST) == KERR_INVALID_CAP
                ? "child: revoked here too\n"
                : "child: revoked here too FAILED\n");
    rv_signal(CHILD_UP, BIT_CHECKED);

    /*
     * The root task lends this process memory and it becomes a pool here,
     * below this process's own pool in the tree.
     * The root task then destroys that pool, and this one has to go with it,
     * or the memory would carry kernel objects nobody can reach.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_POOL &&
                    rv_invoke(OP_REGION_TO_POOL, CHILD_LENT, CHILD_LENT, 0, 0) == KERR_OK &&
                    rv_invoke(OP_POOL_ALLOC, CHILD_LENT, CAP_NOTIFICATION, CHILD_OWN_NTFN, 0) ==
                        KERR_OK
                ? "child: pool made\n"
                : "child: pool made FAILED\n");
    /*
     * The pool this process lives in lies below the boot pool,
     * so destroying the boot pool would destroy this thread,
     * and a thread cannot destroy the pool it lives in.
     */
    rv_puts(CHILD_DEBUG,
            rv_invoke(OP_POOL_DESTROY, CHILD_BOOT_POOL, CHILD_BOOT_POOL, 0, 0) == KERR_STATE
                ? "child: cannot destroy the pool above\n"
                : "child: cannot destroy the pool above FAILED\n");
    rv_signal(CHILD_UP, BIT_POOLED);

    for (;;) {
        rv_wait(CHILD_DOWN, &bits);
    }
}

int main(void);

int main(void)
{
    uint32_t free_base, free_size, grain, bits;

    puts("hello from user mode\n");
    expect("free ram info", rv_region_info(BOOT_CAP_FREE_RAM, &free_base, &free_size));
    /* Read through a4; the offsets below are on a page and a coarser grain is another machine. */
    expect("the layout fits the grain",
           rv_region_grain(BOOT_CAP_FREE_RAM, &grain) == KERR_OK && CHUNK % grain == 0
               ? KERR_OK : KERR_INVALID_ARG);

    expect("carve pool region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, POOL_OFFSET, CHUNK, SLOT_POOL_REGION));
    expect("carve the child's data region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, DATA_OFFSET, CHUNK, SLOT_CHILD_DATA));
    expect("carve the shared region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, SHARED_OFFSET, CHUNK, SLOT_SHARED));
    expect("region to pool",
           rv_invoke(OP_REGION_TO_POOL, SLOT_POOL_REGION, SLOT_POOL, 0, 0));

    expect("allocate the child's table",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_CAPTABLE, SLOT_CHILD_TABLE, CHILD_TABLE_SLOTS));
    expect("allocate the child's process",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_PROCESS, SLOT_CHILD_PROCESS, SLOT_CHILD_TABLE));
    expect("allocate the child's thread",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_THREAD, SLOT_CHILD_THREAD, SLOT_CHILD_PROCESS));
    expect("allocate the upward notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_UP, 0));
    expect("allocate the downward notification",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_DOWN, 0));

    /* The child executes the same flash as the root task, with its own data. */
    expect("map the child's code",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_CODE_SLOT, BOOT_CAP_CODE,
                     RIGHT_R | RIGHT_X));
    expect("map the child's data",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_DATA_SLOT, SLOT_CHILD_DATA,
                     RIGHT_R | RIGHT_W));
    expect("map the shared region there",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));

    /* Authority the child starts with, and nothing besides. */
    expect("give the child the console",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG, BOOT_CAP_DEBUG, RIGHT_ALL));
    expect("give the child the shared region",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED, RIGHT_R | RIGHT_W));
    expect("give the child a way to signal",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP, SLOT_UP, RIGHT_W));
    expect("give the child a way to wait",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN, SLOT_DOWN, RIGHT_R));

    expect("configure the child's thread",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
                     free_base + DATA_OFFSET + CHUNK, 0));
    expect("start the child",
           rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0));

    expect("map the shared region here",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    volatile uint32_t *shared = (volatile uint32_t *)(free_base + SHARED_OFFSET);

    /* The child may or may not have run by now; the bits are sticky either way. */
    expect("wait for the child", rv_wait(SLOT_UP, &bits));
    expect("the child's word arrived",
           bits == BIT_REQUEST && shared[0] == MAGIC ? KERR_OK : KERR_INVALID_ARG);
    puts("root: message ok\n");

    shared[1] = MAGIC + 1;
    expect("answer the child", rv_signal(SLOT_DOWN, BIT_REPLY));

    /*
     * Preemption.
     * Each side spins on a word only the other writes and neither waits,
     * so only the tick gets them past this.
     */
    shared[2] = TURN_CHILD;
    while (shared[2] != TURN_ROOT) {
    }
    puts("root: preemption ok\n");

    expect("wait for the child to finish", rv_wait(SLOT_UP, &bits));
    expect("the child is done", bits == BIT_DONE ? KERR_OK : KERR_INVALID_ARG);

    /*
     * Revocation.
     * A capability to an object in a destroyed pool must stay dead
     * when the same memory is rebuilt into the same kind of object.
     * A generation counter in the object could not promise that:
     * it dies with the memory it lives in and the rebuilt object
     * starts counting from one.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    expect("carve the doomed region",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, DOOMED_OFFSET, CHUNK,
                     SLOT_DOOMED_REGION));
    expect("make it a pool",
           rv_invoke(OP_REGION_TO_POOL, SLOT_DOOMED_REGION, SLOT_DOOMED_POOL, 0, 0));
    expect("allocate a notification in it",
           rv_invoke(OP_POOL_ALLOC, SLOT_DOOMED_POOL, CAP_NOTIFICATION, SLOT_DOOMED_NTFN, 0));
    expect("copy that capability aside",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_STALE, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("give the child one as well",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOOMED, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("the notification works while its pool lives",
           rv_signal(SLOT_DOOMED_NTFN, BIT_REQUEST));

    /* REGION_TO_POOL cleared SLOT_DOOMED_REGION, so the memory comes back there. */
    expect("destroy the pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_DOOMED_POOL, SLOT_DOOMED_REGION, 0, 0));
    expect("the copy is revoked",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);

    /*
     * The new notification lands at the address the old one had,
     * with the type the stale capability names.
     * Nothing in that slot tells the two objects apart,
     * so only the sweep at destroy time keeps the next line honest.
     */
    expect("build a pool over the same memory",
           rv_invoke(OP_REGION_TO_POOL, SLOT_DOOMED_REGION, SLOT_NEW_POOL, 0, 0));
    expect("allocate a notification at the same address",
           rv_invoke(OP_POOL_ALLOC, SLOT_NEW_POOL, CAP_NOTIFICATION, SLOT_NEW_NTFN, 0));
    expect("the stale capability did not come back",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    puts("root: revocation ok\n");

    /* The sweep reached the child's table too, and the child never asked. */
    expect("ask the child to look", rv_signal(SLOT_DOWN, BIT_CHECK));
    expect("wait for the child's answer", rv_wait(SLOT_UP, &bits));
    expect("the child looked", bits == BIT_CHECKED ? KERR_OK : KERR_INVALID_ARG);

    /*
     * Cascade.
     * The child turns lent memory into a pool of its own,
     * which makes the root task's capability to that memory inert.
     * Destroying the child's pool takes the child's own pool with it,
     * and the lent memory answers to the root task's capability again.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    expect("carve memory to lend",
           rv_invoke(OP_REGION_CARVE, BOOT_CAP_FREE_RAM, LENT_OFFSET, CHUNK, SLOT_LENT));
    expect("lend it to the child",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_LENT, SLOT_LENT, RIGHT_ALL));
    expect("give the child the boot pool",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_BOOT_POOL, BOOT_CAP_POOL, RIGHT_ALL));
    expect("ask the child to pool it", rv_signal(SLOT_DOWN, BIT_POOL));
    expect("wait for the child's pool", rv_wait(SLOT_UP, &bits));
    expect("the child made a pool", bits == BIT_POOLED ? KERR_OK : KERR_INVALID_ARG);
    expect("the lent memory is inert here",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LENT, SLOT_LENT_POOL, 0, 0) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);

    /* SLOT_POOL_REGION was cleared when the pool was made; the memory comes back there. */
    expect("destroy the child's pool",
           rv_invoke(OP_POOL_DESTROY, SLOT_POOL, SLOT_POOL_REGION, 0, 0));
    expect("the lent memory is back",
           rv_invoke(OP_REGION_TO_POOL, SLOT_LENT, SLOT_LENT_POOL, 0, 0));
    puts("root: cascade ok\n");

    expect("unmap the shared region",
           rv_invoke(OP_PROCESS_UNINSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, 0, 0));

    puts("reading the removed region, expecting a fault\n");
    uint32_t word = shared[0];

    /* Not reached when PMP works. */
    puts("PMP did not stop the read\n");
    (void)word;
    rv_halt(BOOT_CAP_DEBUG, 2);
}
