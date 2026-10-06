/*
 * The child runs this, in its own process, out of the same code region.
 * It has no data region in common with the root task,
 * so it may touch no global: everything it needs is on its stack
 * or behind a capability the root task put in its table.
 * The link checks that this file keeps none.
 */

#include <stdint.h>

#include "init.h"
#include "rvuos.h"

void child_main(void)
{
    uint32_t base, size, bits;

    rv_puts(CHILD_DEBUG, "child: started\n");
    /* Its log writes and does no more: the machine is not the child's to stop. */
    rv_puts(CHILD_DEBUG, rv_invoke(OP_DEBUG_HALT, CHILD_DEBUG, 3, 0, 0) == KERR_NO_RIGHTS ? "child: may not halt ok\n"
                                                                                       : "child: may not halt FAILED\n");
    if (rv_frame_info(CHILD_SHARED, &base, &size) != KERR_OK) {
        rv_puts(CHILD_DEBUG, "child: no shared region FAILED\n");
        for (;;) {
            rv_wait(CHILD_DOWN, &bits);
        }
    }

    volatile uint32_t *shared = (volatile uint32_t *)base;
    shared[SHARED_MESSAGE] = MAGIC;
    rv_signal(CHILD_UP, BIT_REQUEST);

    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_REPLY && shared[SHARED_ANSWER] == MAGIC + 1 ? "child: reply ok\n"
                                                                    : "child: reply FAILED\n");

    /* The other half of the root task's preemption check. */
    while (shared[SHARED_TURN] != TURN_CHILD) {
    }
    shared[SHARED_TURN] = TURN_ROOT;

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
     * The root task leased this process a frame by deriving a capability into this table
     * and mapping it here, then revoked below its own capability.
     * Both the derived capability and the mapping have to be gone;
     * the mapping is checked from the root task's side, since touching it here would fault.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_LEASE && rv_frame_info(CHILD_LEASE, &base, &size) == KERR_INVALID_CAP
                ? "child: lease revoked here too\n"
                : "child: lease revoked here too FAILED\n");
    rv_signal(CHILD_UP, BIT_LEASED);

    /*
     * The root task lends this process untyped memory, derived from its own,
     * and it becomes a pool here, below the lent Untyped in the tree.
     * The root task then revokes below its own, and this pool has to go with it,
     * or the memory would carry kernel objects nobody can reach.
     */
    rv_wait(CHILD_DOWN, &bits);
    rv_puts(CHILD_DEBUG,
            bits == BIT_POOL &&
                    rv_retype(CHILD_LENT, CAP_POOL, CHILD_LENT_POOL, &base) == KERR_OK &&
                    rv_invoke(OP_POOL_ALLOC, CHILD_LENT_POOL, CAP_NOTIFICATION, CHILD_OWN_NTFN, 0) ==
                        KERR_OK
                ? "child: pool made\n"
                : "child: pool made FAILED\n");
    rv_signal(CHILD_UP, BIT_POOLED);

    /* Nothing more is asked of it: it waits until its pool goes, and it with it. */
    for (;;) {
        rv_wait(CHILD_DOWN, &bits);
    }
}
