/*
 * Kernel image layout, for every board.
 * The Makefile runs this through the preprocessor with the board's board.h;
 * see layout.h for the addresses.
 *
 * The kernel starts at RAM_BASE, where the board enters in machine mode,
 * and owns everything up to its log, which ends where the user code begins.
 * The user program's code is placed at USER_CODE_BASE;
 * its data and stack region lives at USER_DATA_BASE.
 */

#include "layout.h"

ENTRY(_start)

/*
 * tools/stack-depth.py bounds the stack from the linked image
 * and fails the build when the bound exceeds this; see DESIGN.md, "Bounded stack".
 */
KERNEL_STACK_SIZE = 1K;
BOOT_POOL_SIZE    = 4K;

/* For tools/loop-bounds.py: a load from a fixed address outside RAM waits on a device. */
__ram_start = RAM_BASE;
__ram_end   = RAM_BASE + RAM_SIZE;

SECTIONS
{
    . = RAM_BASE;

    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    .stack (NOLOAD) : {
        . = ALIGN(16);
        __kernel_stack_bottom = .;
        . += KERNEL_STACK_SIZE;
        __kernel_stack_top = .;
    }

    /* Memory for the root task's kernel objects; see boot.c. */
    .bootpool (NOLOAD) : {
        . = ALIGN(16);
        __boot_pool_start = .;
        . += BOOT_POOL_SIZE;
        __boot_pool_end = .;
    }

    ASSERT(. <= KLOG_BASE, "kernel does not fit below its log")

    . = USER_CODE_BASE;
    .user_code : {
        KEEP(*(.user_code))
    }
    ASSERT(. <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack)
    }
}
