/*
 * Kernel image layout, for every board.
 * The Makefile runs this through the preprocessor with the board's board.h;
 * see layout.h for the addresses.
 *
 * The kernel starts at RAM_BASE, where the board enters in machine mode,
 * and owns everything up to its log, which ends where the user code begins or a block below.
 * The user program's code is placed at USER_CODE_BASE;
 * its data and stack region lives at USER_DATA_BASE.
 */

#include "layout.h"

ENTRY(_start)

/* For tools/loop-bounds.py: a load from a fixed address outside RAM waits on a device. */
__ram_start = RAM_BASE;
__ram_end   = RAM_BASE + RAM_SIZE;

SECTIONS
{
    . = RAM_BASE;

    /* What a board's boot ROM looks for at the start of an image; RP2350's, and nothing elsewhere. */
    .board_header : {
        KEEP(*(.board_header))
    }

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

    /* A stack for each core, KERNEL_STACK_SIZE of layout.h, the first core's at the top and the next below it. */
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += KERNEL_STACK_SIZE * CORES;
        __kernel_stack_top = .;
    }
    /* The first core's stack ends here, and tools/stack-depth.py holds every core's to the size of this one. */
    __kernel_stack_bottom = __kernel_stack_top - KERNEL_STACK_SIZE;

    ASSERT(. <= KLOG_BASE, "kernel does not fit below its log")

    . = USER_CODE_BASE;
    .user_code : {
        KEEP(*(.user_code))
    }
    ASSERT(. <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    /* Nothing unwinds, and ARM's compiler marks every function as not unwinding all the same. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack .ARM.exidx* .ARM.extab*)
    }
}
