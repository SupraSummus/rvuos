#ifndef RVUOS_REPLAY_H
#define RVUOS_REPLAY_H

/*
 * The state the replay driver builds before it turns tracing on.
 *
 * The host build boots the kernel without running any user code,
 * so it must reach exactly the kernel state the driver reaches on QEMU
 * or the two transcripts diverge for reasons that are not kernel bugs.
 * Keeping the setup here, as records both sides perform,
 * is what stops user/fuzzdrv.c and host/shim.c from drifting apart.
 *
 * The driver runs a second thread in the root process
 * so that a record which blocks one thread has somewhere to go.
 * Both threads take records from the same cursor,
 * so the order of the calls follows the kernel's own choice
 * of what runs, on the target and on the host alike.
 */

#include "rvuos/abi.h"

/* Region slot the input is installed in. */
#define REPLAY_REGION_SLOT 7

/* Capability slots the setup fills, above the boot capabilities. */
#define REPLAY_CAP_NOTIFY  12
#define REPLAY_CAP_THREAD  13

/*
 * The second thread's stack: the middle of the root task's data region.
 * host/shim.c checks that it lies inside that region.
 */
#define REPLAY_THREAD_SP 0x80208000u

/*
 * The second thread's entry point is the one value the two builds differ on:
 * the driver patches OP_THREAD_CONFIGURE with the address of its record loop,
 * and the host leaves the zero, because the kernel stores
 * a thread's program counter without looking at it
 * and the host never fetches an instruction.
 */
static const struct replay_record replay_prologue[] = {
    { OP_PROCESS_INSTALL, 0, BOOT_CAP_PROCESS, REPLAY_REGION_SLOT, BOOT_CAP_INPUT, RIGHT_R },
    { OP_POOL_ALLOC, 0, BOOT_CAP_POOL, CAP_NOTIFICATION, REPLAY_CAP_NOTIFY, 0 },
    { OP_POOL_ALLOC, 0, BOOT_CAP_POOL, CAP_THREAD, REPLAY_CAP_THREAD, BOOT_CAP_PROCESS },
    { OP_THREAD_CONFIGURE, 0, REPLAY_CAP_THREAD, 0, REPLAY_THREAD_SP, 0 },
    { OP_THREAD_RESUME, 0, REPLAY_CAP_THREAD, 0, 0, 0 },
};

#define REPLAY_PROLOGUE_COUNT \
    (sizeof(replay_prologue) / sizeof(replay_prologue[0]))

#endif
