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
 * The kernel's stack, one of this size for each core, below the one of the core before;
 * tools/stack-depth.py bounds the stack from the linked image
 * and fails the build when the bound exceeds it; see DESIGN.md, "Bounded stack".
 */
#define KERNEL_STACK_SHIFT 10
#define KERNEL_STACK_SIZE  (1 << KERNEL_STACK_SHIFT)

/* The boot pool, where the kernel builds the root task's objects. */
#define BOOT_POOL_SIZE U32(0x00001000)

/*
 * Whether a range lies in the root task's memory, or outside RAM altogether, as in flash a board maps.
 * The root task's code and input regions lie in either,
 * and boot.c makes them of its memory only where they lie in it.
 */
#define IN_ROOT_RAM(base, size) (ROOT_RAM_BASE <= (base) && (base) + (size) <= ROOT_RAM_BASE + ROOT_RAM_SIZE)
#define OUTSIDE_RAM(base, size) ((base) + (size) <= RAM_BASE || RAM_BASE + RAM_SIZE <= (base))

#ifndef __ASSEMBLER__
/*
 * The root task's memory holds its data region and the boot pool, in that order,
 * and its code before the data and its input between the data and the pool, unless they lie outside RAM.
 */
_Static_assert(IN_ROOT_RAM(USER_DATA_BASE, USER_DATA_SIZE) && USER_DATA_BASE + USER_DATA_SIZE <= BOOT_POOL_BASE &&
                   BOOT_POOL_BASE % BOOT_POOL_SIZE == 0 && IN_ROOT_RAM(BOOT_POOL_BASE, BOOT_POOL_SIZE),
               "the root task's data and the boot pool lie apart in its memory");
_Static_assert((IN_ROOT_RAM(USER_CODE_BASE, USER_CODE_SIZE) && USER_CODE_BASE + USER_CODE_SIZE <= USER_DATA_BASE) ||
                   OUTSIDE_RAM(USER_CODE_BASE, USER_CODE_SIZE),
               "the root task's code lies below its data in its memory, or outside RAM");
_Static_assert((IN_ROOT_RAM(INPUT_BASE, INPUT_SIZE) && USER_DATA_BASE + USER_DATA_SIZE <= INPUT_BASE &&
                INPUT_BASE + INPUT_SIZE <= BOOT_POOL_BASE) ||
                   OUTSIDE_RAM(INPUT_BASE, INPUT_SIZE),
               "the root task's input lies between its data and the boot pool in its memory, or outside RAM");
#endif

/*
 * The kernel's log, see klog.h: its header and a ring of KLOG_SIZE bytes,
 * one block at the top of the kernel's memory, right below the root task's code,
 * or a block lower where the log may not touch the code; see PMP_SPLIT_STORE_AS_READ in board.h.
 * A board whose free RAM lies between the kernel and the root task's memory places it in board.h,
 * as does one whose root task's code lies in flash.
 */
#define KLOG_REGION_SIZE U32(0x00001000)
#define KLOG_SIZE        (KLOG_REGION_SIZE - RVUOS_LOG_HEADER)
#ifndef KLOG_BASE
#if PMP_SPLIT_STORE_AS_READ
#define KLOG_BASE        (USER_CODE_BASE - 2 * KLOG_REGION_SIZE)
#else
#define KLOG_BASE        (USER_CODE_BASE - KLOG_REGION_SIZE)
#endif
#endif

#ifndef __ASSEMBLER__
_Static_assert(KLOG_BASE % KLOG_REGION_SIZE == 0 && KLOG_BASE + KLOG_REGION_SIZE <= ROOT_RAM_BASE &&
                   KLOG_BASE + KLOG_REGION_SIZE <= FREE_RAM_BASE &&
                   (!PMP_SPLIT_STORE_AS_READ || KLOG_BASE + KLOG_REGION_SIZE < USER_CODE_BASE),
               "the kernel's log is a block of its own below what the root task is given, off its code where it must be");
_Static_assert(!PMP_SPLIT_STORE_AS_READ || USER_DATA_BASE + USER_DATA_SIZE < INPUT_BASE,
               "the data region may not touch the input region");
#endif

#endif
