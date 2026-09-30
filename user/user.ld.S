/*
 * The single user program of the skeleton.
 * Code and read-only data live in the read-execute region,
 * zero-initialised data and the stack in the read-write region.
 * Initialised data is forbidden for now
 * because nobody would copy it into the data region.
 * The Makefile runs this through the preprocessor with the board's board.h;
 * see kernel/layout.h for the addresses.
 */

#include "layout.h"

ENTRY(_start)

/* For the escape suite, see user/escape-*.c: where the kernel starts, which no program may reach. */
__kernel_start = RAM_BASE;

SECTIONS
{
    . = USER_CODE_BASE;

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
    ASSERT(SIZEOF(.data) == 0, "user programs may not have initialised data yet")
    ASSERT(. <= USER_CODE_BASE + USER_CODE_SIZE, "user code does not fit its region")

    . = USER_DATA_BASE;

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }
    ASSERT(. <= USER_DATA_BASE + USER_DATA_SIZE, "user data does not fit its region")

    /* Nothing unwinds, and ARM's compiler marks every function as not unwinding all the same. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note.GNU-stack .ARM.exidx* .ARM.extab*)
    }
}
