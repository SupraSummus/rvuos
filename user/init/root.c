/*
 * Root task for the current stage.
 * It walks the capability system once end to end on real hardware paths,
 * a part at a time, in the order main runs them:
 *   logger.c     a thread that carries the kernel's log to the UART
 *   root.c       memory, a second process running child.c, a message, preemption, revocation, derivation, cascade
 *   time.c       the timer, the clock and a period
 *   units.c      units of the processor, spare time, charging, and a spin no tick interrupts
 *   root.c       an Irq on a line nothing drives
 *   cores.c      the second core, on a machine of several
 *   successor.c  the handover, a fault, and the watchdog, whose halt ends the demo
 * Negative paths are covered by the fuzz corpus and tests/differential.py.
 */

#include <stdint.h>

#include "console.h"
#include "csrs.h"
#include "init.h"
#include "rvuos.h"

/*
 * The least block the root task takes, see take_untyped:
 * the sixth halving of the least free RAM a board gives, nRF52840's 128 KiB.
 */
#define CHUNK 0x800u

/* Initialised data, which start.S copies from behind the code before main; see user/user.ld.S. */
static volatile uint32_t initialised[2] = { MAGIC, ~MAGIC };

void expect(const char *what, uint32_t status)
{
    puts(what);
    puts(status == KERR_OK ? ": ok\n" : ": FAILED\n");
    if (status != KERR_OK) {
        rv_halt(BOOT_CAP_DEBUG, 1);
    }
}

/*
 * The root task keeps no allocator.
 * It takes each block it needs off what is left of its free RAM:
 * the lower half, while the upper half is what is left from then on,
 * so the blocks halve one after another, and the last one is all that is left.
 * The Untyped it split goes each time, and what it made hangs right below the free RAM,
 * which holds on to every block.
 */
static uint32_t rest = BOOT_CAP_FREE_RAM;

/* The next block as an Untyped in slot: where it lies and how large it is, at least a chunk. */
static void take_untyped(uint32_t slot, uint32_t *base, uint32_t *size)
{
    uint32_t made;
    expect("halve what is left of the free ram", rv_split(rest, slot, SLOT_SPLIT));
    if (rest == SLOT_REST) {
        expect("let the halved one go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_REST, 0, 0));
    }
    expect("keep the upper half",
           rv_invoke(OP_CAP_MOVE, BOOT_CAP_CAPTABLE, SLOT_REST, SLOT_SPLIT, 0));
    rest = SLOT_REST;
    expect("the block holds a chunk",
           rv_untyped_info(slot, base, size, &made) == KERR_OK && *size >= CHUNK
               ? KERR_OK : KERR_INVALID_ARG);
}

/*
 * The next block made into a frame in slot.
 * A frame is taken back by revoking below it, so its Untyped may go;
 * a pool goes only with its Untyped, so main takes the child's pool's with take_untyped and keeps it.
 */
static void take_frame(uint32_t slot, uint32_t *base, uint32_t *size)
{
    take_untyped(SLOT_BLOCK, base, size);
    expect("make the block", rv_retype(SLOT_BLOCK, CAP_FRAME, slot, base));
    expect("let its Untyped go", rv_invoke(OP_CAP_DELETE, BOOT_CAP_CAPTABLE, SLOT_BLOCK, 0, 0));
}

int main(void);

int main(void)
{
    uint32_t free_base, free_size, free_made, min_size, bits;
    uint32_t shared_base, child_data_base, pool_base, doomed_base, new_pool_base, lease_base;
    uint32_t shared_size, child_data_size, pool_size, doomed_size, lease_size;
    uint32_t data_base, data_size, uart_base, uart_size, log_base, log_size;

    puts("hello from user mode\n");

    /*
     * The logger first, so that everything above and below reaches the UART
     * while the machine runs, and not only when the halt writes the log out.
     * Its thread runs on a stack in the middle of the data region; this thread's is at the top.
     */
    expect("uart info", rv_frame_info(BOOT_CAP_UART, &uart_base, &uart_size));
    expect("log info", rv_frame_info(BOOT_CAP_LOG, &log_base, &log_size));
    expect("data info", rv_frame_info(BOOT_CAP_DATA, &data_base, &data_size));
    logger_start(uart_base, log_base, data_base + data_size / 2);

    expect("initialised data was copied",
           initialised[0] == MAGIC && initialised[1] == ~MAGIC ? KERR_OK : KERR_INVALID_ARG);

    expect("free ram info", rv_untyped_info(BOOT_CAP_FREE_RAM, &free_base, &free_size, &free_made));
    expect("nothing is made of the free ram yet", free_made == 0 ? KERR_OK : KERR_INVALID_ARG);
    /* Read through a4. Every block below holds a chunk, which is a block no smaller than the smallest region. */
    expect("the layout fits the smallest region",
           rv_frame_min_size(BOOT_CAP_DATA, &min_size) == KERR_OK && CHUNK % min_size == 0
               ? KERR_OK : KERR_INVALID_ARG);

    take_frame(SLOT_SHARED, &shared_base, &shared_size);
    take_frame(SLOT_CHILD_DATA, &child_data_base, &child_data_size);
    take_untyped(SLOT_POOL_MEMORY, &pool_base, &pool_size);
    expect("make the child's pool", rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base));
    expect("the blocks halve one after another",
           shared_base == free_base && shared_size == free_size / 2 &&
                   child_data_base == free_base + free_size / 2 && child_data_size == free_size / 4 &&
                   pool_base == child_data_base + child_data_size && pool_size == free_size / 8
               ? KERR_OK : KERR_INVALID_ARG);
    uint32_t none;
    expect("they hang below the free ram, which makes nothing more",
           rv_retype(BOOT_CAP_FREE_RAM, CAP_FRAME, SLOT_BLOCK, &none) == KERR_NO_MEMORY
               ? KERR_OK : KERR_INVALID_ARG);

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
    expect("give the child the log, to write",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DEBUG, BOOT_CAP_DEBUG, RIGHT_W));
    expect("give the child the shared region",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_SHARED, SLOT_SHARED, RIGHT_R | RIGHT_W));
    expect("give the child a way to signal",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_UP, SLOT_UP, RIGHT_W));
    expect("give the child a way to wait",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOWN, SLOT_DOWN, RIGHT_R));

    expect("configure the child's thread",
           rv_invoke(OP_THREAD_CONFIGURE, SLOT_CHILD_THREAD, (uint32_t)&child_main,
                     child_data_base + child_data_size, 0));
    /*
     * The child earns the other half of the processor, carved out of the boot grant
     * so that revoking below it stops the child alone.
     * Its account starts empty and fills in two ticks, and then this thread and the child both have time:
     * while both want the processor, they take turns.
     */
    expect("carve the child half the processor",
           rv_invoke(OP_TIME_CARVE, BOOT_CAP_TIME, HALF_UNITS, HALF_UNITS, SLOT_HALF_TIME));
    expect("bind the child to it",
           rv_invoke(OP_TIME_BIND, SLOT_HALF_TIME, SLOT_CHILD_THREAD, 0, HALF_UNITS));
    expect("start the child",
           rv_invoke(OP_THREAD_RESUME, SLOT_CHILD_THREAD, 0, 0, 0));

    expect("map the shared region here",
           rv_invoke(OP_PROCESS_INSTALL, BOOT_CAP_PROCESS, ROOT_SHARED_SLOT, SLOT_SHARED,
                     RIGHT_R | RIGHT_W));
    volatile uint32_t *shared = (volatile uint32_t *)shared_base;

    /* The child may or may not have run by now; the bits are sticky either way. */
    expect("wait for the child", rv_wait(SLOT_UP, &bits));
    expect("the child's word arrived",
           bits == BIT_REQUEST && shared[SHARED_MESSAGE] == MAGIC ? KERR_OK : KERR_INVALID_ARG);
    puts("root: message ok\n");

    shared[SHARED_ANSWER] = MAGIC + 1;
    expect("answer the child", rv_signal(SLOT_DOWN, BIT_REPLY));

    /*
     * Preemption.
     * Each side spins on a word only the other writes and neither waits,
     * so only the tick gets them past this.
     */
    uint32_t count = counter_start();
    shared[SHARED_TURN] = TURN_CHILD;
    while (shared[SHARED_TURN] != TURN_ROOT) {
    }
    puts("root: preemption ok\n");
    /* The child's process never started the counter, and the root task's counted on from where it was. */
    expect("the counter counts on across the child's turns", counter_kept(count) ? KERR_OK : KERR_INVALID_ARG);

    expect("wait for the child to finish", rv_wait(SLOT_UP, &bits));
    expect("the child is done", bits == BIT_DONE ? KERR_OK : KERR_INVALID_ARG);

    /*
     * Revocation.
     * A capability to an object in a destroyed pool must stay dead
     * when the same memory is rebuilt into the same kind of object.
     * A generation counter in the object could not promise that:
     * it dies with the memory it lives in and the rebuilt object
     * starts counting from one.
     * The pool is made of an Untyped of its own,
     * which is free again once the pool is gone and makes the same block again.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    take_untyped(SLOT_DOOMED_MEMORY, &doomed_base, &doomed_size);
    expect("make it a pool",
           rv_retype(SLOT_DOOMED_MEMORY, CAP_POOL, SLOT_DOOMED_POOL, &doomed_base));
    expect("allocate a notification in it",
           rv_invoke(OP_POOL_ALLOC, SLOT_DOOMED_POOL, CAP_NOTIFICATION, SLOT_DOOMED_NTFN, 0));
    expect("copy that capability aside",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_STALE, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("give the child one as well",
           rv_invoke(OP_CAP_COPY, SLOT_CHILD_TABLE, CHILD_DOOMED, SLOT_DOOMED_NTFN, RIGHT_ALL));
    expect("the notification works while its pool lives",
           rv_signal(SLOT_DOOMED_NTFN, BIT_REQUEST));

    /* A revoke below the Untyped the pool was made of destroys it, and the memory is free again. */
    expect("destroy the pool",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_DOOMED_MEMORY, 0, 0));
    expect("the copy is revoked",
           rv_signal(SLOT_STALE, BIT_REQUEST) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);

    /*
     * The new notification lands at the address the old one had,
     * with the type the stale capability names.
     * Nothing in that slot tells the two objects apart,
     * so only the sweep at destroy time keeps the next line honest.
     */
    expect("build a pool over the same memory",
           rv_retype(SLOT_DOOMED_MEMORY, CAP_POOL, SLOT_NEW_POOL, &new_pool_base));
    expect("it is the same memory", new_pool_base == doomed_base ? KERR_OK : KERR_INVALID_ARG);
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
     * Derivation.
     * A capability derived from another is revoked with it,
     * wherever it went and whatever was installed from it,
     * while a copy made beside the original stays;
     * see DESIGN.md, "The derivation tree".
     * Whether the child's mapping is gone shows in the child's region slot,
     * which takes a region again once it is empty.
     */
    take_frame(SLOT_LEASE, &lease_base, &lease_size);
    expect("copy it beside itself",
           rv_invoke(OP_CAP_COPY, BOOT_CAP_CAPTABLE, SLOT_LEASE_COPY, SLOT_LEASE, RIGHT_ALL));
    expect("derive it into the child's table",
           rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_LEASE, SLOT_LEASE, RIGHT_R | RIGHT_W));
    expect("map it in the child",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE,
                     RIGHT_R | RIGHT_W));
    expect("the child's slot is taken while it is mapped",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE_COPY,
                     RIGHT_R) == KERR_SLOT_IN_USE
               ? KERR_OK : KERR_INVALID_ARG);
    expect("revoke below the lease",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_LEASE, 0, 0));
    expect("the lease itself stays", rv_frame_info(SLOT_LEASE, &lease_base, &lease_size));
    expect("the copy beside it stays", rv_frame_info(SLOT_LEASE_COPY, &lease_base, &lease_size));
    expect("the child's mapping is gone",
           rv_invoke(OP_PROCESS_INSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, SLOT_LEASE_COPY,
                     RIGHT_R));
    expect("unmap it again",
           rv_invoke(OP_PROCESS_UNINSTALL, SLOT_CHILD_PROCESS, CHILD_LEASE_SLOT, 0, 0));
    expect("ask the child to look at the lease", rv_signal(SLOT_DOWN, BIT_LEASE));
    expect("wait for the child's answer", rv_wait(SLOT_UP, &bits));
    expect("the child looked at the lease", bits == BIT_LEASED ? KERR_OK : KERR_INVALID_ARG);
    puts("root: derivation ok\n");

    /*
     * Cascade.
     * The child turns lent untyped memory into a pool of its own,
     * and the root task's Untyped makes nothing while the lent one lives.
     * Revoking below it destroys the child's pool,
     * and the root task's Untyped is free again, the whole of it.
     * See DESIGN.md, "Kernel pools and revocation".
     */
    uint32_t lent_base, lent_size;
    take_untyped(SLOT_LENT, &lent_base, &lent_size);
    expect("lend it to the child",
           rv_invoke(OP_CAP_DERIVE, SLOT_CHILD_TABLE, CHILD_LENT, SLOT_LENT, RIGHT_ALL));
    expect("ask the child to pool it", rv_signal(SLOT_DOWN, BIT_POOL));
    expect("wait for the child's pool", rv_wait(SLOT_UP, &bits));
    expect("the child made a pool", bits == BIT_POOLED ? KERR_OK : KERR_INVALID_ARG);
    uint32_t lent_again;
    expect("the lent memory is inert here",
           rv_retype(SLOT_LENT, CAP_POOL, SLOT_LENT_POOL, &lent_again) == KERR_NO_MEMORY
               ? KERR_OK : KERR_INVALID_ARG);
    expect("revoke below the lent memory",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_LENT, 0, 0));
    expect("the lent memory is back",
           rv_retype(SLOT_LENT, CAP_POOL, SLOT_LENT_POOL, &lent_again));
    expect("it is the same memory", lent_again == lent_base ? KERR_OK : KERR_INVALID_ARG);
    puts("root: cascade ok\n");

    uint32_t hz, counter;
    time_demo(&hz, &counter);
    units_demo(data_base, data_size, counter, hz);

    /* The child is done, and goes with its pool. */
    expect("destroy the child's pool",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_POOL_MEMORY, 0, 0));

    /*
     * Interrupts, beyond the two lines the logger lives on.
     * A line nothing drives shows what the logger cannot:
     * that a line is bound once, that an armed line nothing raises stays quiet,
     * and that the Irq dies with its pool, which frees the line.
     * It shares the timer's notification, one bit each,
     * which is what makes a wait with a timeout the same call as a wait.
     */
    expect("carve a spare line",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, SPARE_IRQ, 1, SLOT_SPARE_LINE));
    expect("carve it again",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_IRQ_LINES, SPARE_IRQ, 1, SLOT_SPARE_LINE_COPY));
    expect("bind the line to the timer's notification",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE, SLOT_NEW_POOL, SLOT_TIMER_NTFN, SLOT_SPARE_IRQ));
    expect("the line is taken",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE_COPY, SLOT_NEW_POOL, SLOT_TIMER_NTFN,
                     SLOT_SPARE_IRQ_AGAIN) == KERR_OVERLAP
               ? KERR_OK : KERR_INVALID_ARG);
    expect("arm the line", rv_irq_set(SLOT_SPARE_IRQ, BIT_SPARE));
    /* Unmasked at the controller and never raised: the timer's bit comes alone. */
    expect("the idle line stays quiet", sleep_us(SLEEP_US));

    /* The Irq dies with its pool, which masks the line and frees it for the copy. */
    expect("destroy the driver's pool",
           rv_invoke(OP_CAP_REVOKE, BOOT_CAP_CAPTABLE, SLOT_DOOMED_MEMORY, 0, 0));
    expect("the irq is revoked",
           rv_irq_set(SLOT_SPARE_IRQ, BIT_SPARE) == KERR_INVALID_CAP ? KERR_OK : KERR_INVALID_ARG);
    expect("allocate a notification in the boot pool",
           rv_invoke(OP_POOL_ALLOC, BOOT_CAP_POOL, CAP_NOTIFICATION, SLOT_BOOT_NTFN, 0));
    expect("the line is free again",
           rv_invoke(OP_IRQ_BIND, SLOT_SPARE_LINE_COPY, BOOT_CAP_POOL, SLOT_BOOT_NTFN,
                     SLOT_SPARE_IRQ_AGAIN));
    puts("root: irq ok\n");

    /*
     * The timer went with the driver's pool, so it comes back in a pool of the child's memory,
     * with the second core's threads below, for the sleeps that follow and the successor.
     */
    expect("make a pool of the child's memory again", rv_retype(SLOT_POOL_MEMORY, CAP_POOL, SLOT_POOL, &pool_base));
    expect("allocate the timer's notification again",
           rv_invoke(OP_POOL_ALLOC, SLOT_POOL, CAP_NOTIFICATION, SLOT_TIMER_NTFN, 0));
    expect("carve the timer line again, which its bind took",
           rv_invoke(OP_IRQ_CARVE, BOOT_CAP_TIMER_LINES, 0, 1, SLOT_TIMER_LINE));
    expect("bind it to the notification",
           rv_invoke(OP_IRQ_BIND, SLOT_TIMER_LINE, SLOT_POOL, SLOT_TIMER_NTFN, SLOT_TIMER));

    cores_demo(data_base, data_size, shared_base, counter, hz);

    hand_over(data_base + 3 * (data_size / 4));
}
