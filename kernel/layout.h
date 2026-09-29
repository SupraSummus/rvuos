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

/* The boot pool, where the kernel builds the root task's objects. */
#define BOOT_POOL_SIZE U32(0x00001000)

#ifndef __ASSEMBLER__
/* The root task's memory holds its code, data and input regions and the boot pool, in that order. */
_Static_assert(ROOT_RAM_BASE <= USER_CODE_BASE && USER_CODE_BASE + USER_CODE_SIZE <= USER_DATA_BASE &&
                   USER_DATA_BASE + USER_DATA_SIZE <= INPUT_BASE && INPUT_BASE + INPUT_SIZE <= BOOT_POOL_BASE &&
                   BOOT_POOL_BASE % BOOT_POOL_SIZE == 0 &&
                   BOOT_POOL_BASE + BOOT_POOL_SIZE <= ROOT_RAM_BASE + ROOT_RAM_SIZE,
               "the root task's regions and the boot pool lie apart in its memory");
#endif

/*
 * The kernel's log, see klog.h: its header and a ring of KLOG_SIZE bytes,
 * one block at the top of the kernel's memory, right below the root task's code,
 * or a block lower where the log may not touch the code; see PMP_SPLIT_STORE_AS_READ in board.h.
 */
#define KLOG_REGION_SIZE U32(0x00001000)
#define KLOG_SIZE        (KLOG_REGION_SIZE - RVUOS_LOG_HEADER)
#if PMP_SPLIT_STORE_AS_READ
#define KLOG_BASE        (USER_CODE_BASE - 2 * KLOG_REGION_SIZE)
#else
#define KLOG_BASE        (USER_CODE_BASE - KLOG_REGION_SIZE)
#endif

#ifndef __ASSEMBLER__
_Static_assert(!PMP_SPLIT_STORE_AS_READ || USER_DATA_BASE + USER_DATA_SIZE < INPUT_BASE,
               "the data region may not touch the input region");
#endif

#endif
