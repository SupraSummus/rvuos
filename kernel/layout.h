#ifndef RVUOS_LAYOUT_H
#define RVUOS_LAYOUT_H

/*
 * The memory layout, read by the kernel, the host build and both linker scripts.
 * The board's board.h, kernel/board/<board>/, says where RAM and the root task lie;
 * what follows from that is worked out here the same way for every board.
 * Everything the root task is granted is a block one NAPOT entry describes,
 * a power of two aligned to its own size, and boot.c checks that it is;
 * see DESIGN.md, "Physical Memory Protection".
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

/*
 * The kernel's log, see klog.h: its header and a ring of KLOG_SIZE bytes,
 * one block at the top of the kernel's memory, right below the root task's code.
 */
#define KLOG_REGION_SIZE U32(0x00001000)
#define KLOG_SIZE        (KLOG_REGION_SIZE - RVUOS_LOG_HEADER)
#define KLOG_BASE        (USER_CODE_BASE - KLOG_REGION_SIZE)

#endif
