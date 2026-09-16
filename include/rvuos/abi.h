#ifndef RVUOS_ABI_H
#define RVUOS_ABI_H

#ifndef __ASSEMBLER__
#include <stdint.h>
#endif

/*
 * The kernel ABI shared by the kernel and user programs.
 *
 * Every system call is an invocation on a capability.
 * Registers at the ecall:
 *   a7      operation code, one of OP_*
 *   a0      slot index of the invoked capability in the caller's table
 *   a1..a3  operation arguments; a4..a6 are reserved
 * Registers on return:
 *   a0      status, one of KERR_*
 *   a1..a3  results, operation specific; unchanged unless documented
 * No operation takes a pointer into user memory.
 *
 * Slots that receive a new capability are always in the caller's own table.
 * A process needs no capability to write its own table;
 * the CapTable capability exists to write another process's table.
 */

/* Status codes returned in a0. */
#define KERR_OK           0
#define KERR_INVALID_CAP  1 /* slot out of range, empty, or stale */
#define KERR_WRONG_TYPE   2 /* capability does not accept this operation */
#define KERR_NO_RIGHTS    3 /* capability lacks a right the operation needs */
#define KERR_INVALID_ARG  4
#define KERR_NO_MEMORY    5 /* pool exhausted */
#define KERR_SLOT_IN_USE  6 /* destination slot already holds a capability */
#define KERR_OVERLAP      7 /* region overlaps a pool or an installed region */
#define KERR_LIMIT        8 /* a fixed kernel limit was hit, such as PMP entries */

/* Capability and object types. */
#define CAP_NONE     0
#define CAP_REGION   1 /* physical address range; no kernel object behind it */
#define CAP_POOL     2
#define CAP_CAPTABLE 3
#define CAP_PROCESS  4
#define CAP_THREAD   5
#define CAP_DEBUG    6 /* console output and machine halt, for bring-up */

/*
 * Rights bits.
 * Region: RIGHT_R, RIGHT_W, RIGHT_X as memory permissions.
 * CapTable: RIGHT_W to copy into or delete from the table.
 * Pool: RIGHT_W to allocate objects.
 * Process, Thread: RIGHT_W to control the object.
 * Debug: any right.
 * Copying a capability can only remove rights.
 */
#define RIGHT_R 0x1
#define RIGHT_W 0x2
#define RIGHT_X 0x4
#define RIGHT_ALL 0x7

/*
 * Operations, with the capability type they apply to and their arguments.
 */

/* Debug: write one character. a1 = character. */
#define OP_DEBUG_PUTC 1
/* Debug: halt the machine. a1 = exit code. Does not return. */
#define OP_DEBUG_HALT 2
/*
 * Debug: turn on tracing and self-checking.
 * From then on the kernel prints every system call with its status
 * and runs the invariant checker after each one.
 * It cannot be turned off again, so a traced program cannot hide.
 */
#define OP_DEBUG_TRACE 10

/*
 * CapTable (RIGHT_W): copy a capability from the caller's table.
 * a1 = destination slot in the invoked table,
 * a2 = source slot in the caller's table,
 * a3 = rights mask applied to the copy.
 */
#define OP_CAP_COPY 3
/* CapTable (RIGHT_W): clear a slot. a1 = slot. */
#define OP_CAP_DELETE 4

/* Region: describe it. Returns a1 = base, a2 = size, a3 = rights. */
#define OP_REGION_INFO 11
/*
 * Region: derive a smaller region with the same rights.
 * a1 = offset from the region base, a2 = size, a3 = destination slot.
 * Offset and size must be multiples of 4.
 */
#define OP_REGION_CARVE 5
/*
 * Region (RIGHT_R and RIGHT_W): hand the memory to the kernel as a pool.
 * a1 = destination slot for the Pool capability.
 * The invoked slot is cleared.
 * Fails with KERR_OVERLAP if the range overlaps an existing pool
 * or a region installed in any process.
 */
#define OP_REGION_TO_POOL 6

/*
 * Pool (RIGHT_W): allocate a kernel object.
 * a1 = object type, a2 = destination slot,
 * a3 = type specific: number of slots for CAP_CAPTABLE.
 */
#define OP_POOL_ALLOC 7

/*
 * Process (RIGHT_W): install a region into one of the process's region slots.
 * a1 = region slot index, a2 = Region capability slot in the caller's table,
 * a3 = rights to install, a subset of the region's rights.
 * RIGHT_W without RIGHT_R is rejected with KERR_INVALID_ARG,
 * because PMP reserves that encoding.
 * Fails with KERR_OVERLAP if the range overlaps a pool
 * or another region installed in the same process.
 */
#define OP_PROCESS_INSTALL 8
/* Process (RIGHT_W): clear a region slot. a1 = region slot index. */
#define OP_PROCESS_UNINSTALL 9

/*
 * Capability slots the kernel fills in the root task's table at boot.
 * Slot 0 is left empty so that an uninitialised index is an error.
 */
#define BOOT_CAP_NULL      0
#define BOOT_CAP_CAPTABLE  1 /* the root task's own table */
#define BOOT_CAP_PROCESS   2 /* the root task's own process */
#define BOOT_CAP_THREAD    3 /* the root task's only thread */
#define BOOT_CAP_POOL      4 /* the boot pool the root objects live in */
#define BOOT_CAP_DEBUG     5
#define BOOT_CAP_CODE      6 /* Region: the root task's code, read and execute */
#define BOOT_CAP_DATA      7 /* Region: the root task's data and stack */
#define BOOT_CAP_FREE_RAM  8 /* Region: all RAM the kernel does not use */
#define BOOT_CAP_INPUT     9 /* Region, read only: test input the loader placed in RAM */
#define BOOT_CAP_COUNT     10

/*
 * Replay input, as loaded into the BOOT_CAP_INPUT region:
 * a header followed by count records, little-endian.
 * The same records feed the host fuzzer, without the header.
 */
#define REPLAY_MAGIC 0x5a465652u /* "RVFZ" */
#define REPLAY_MAX_RECORDS 256

#ifndef __ASSEMBLER__
struct replay_header {
    uint32_t magic;
    uint32_t count;
};

struct replay_record {
    uint8_t op;
    uint8_t pad;
    uint16_t slot;
    uint32_t a1, a2, a3;
};
#endif

/* Fixed limits visible to user programs. */
#define PROCESS_REGION_SLOTS 8

#endif
