#ifndef RVUOS_LAYOUT_H
#define RVUOS_LAYOUT_H

/*
 * The memory layout, read by the kernel, the host build and both linker scripts.
 * The board's board.h, kernel/board/<board>/, says where RAM and the root task lie;
 * what follows from that is worked out here the same way for every board.
 *
 * The linker scripts are run through the preprocessor and know no integer suffixes,
 * so every constant is written through U32.
 */
#ifdef __ASSEMBLER__
#define U32(x) x
#else
#define U32(x) x##u
#endif

#include "board.h"
#include "rvuos/abi.h"

/* The last INPUT_SIZE bytes of RAM hold test input placed there by the loader. */
#define INPUT_BASE (RAM_BASE + RAM_SIZE - INPUT_SIZE)

/*
 * The kernel's log, see klog.h: its header and KLOG_SIZE bytes of ring,
 * at the top of the kernel's memory, touching the root task's code
 * so that the two cost one PMP boundary.
 */
#define KLOG_SIZE        U32(0x00001000)
#define KLOG_REGION_SIZE (RVUOS_LOG_HEADER + KLOG_SIZE)
#define KLOG_BASE        (USER_CODE_BASE - KLOG_REGION_SIZE)

#endif
